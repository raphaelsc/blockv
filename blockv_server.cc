/*
 * Copyright (C) 2016 Raphael S. Carvalho
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <utility>
#include <memory>
#include <limits>
#include <shared_mutex>
#include <linux/fs.h>
#include "blockv_protocol.hh"

#define BLOCKV_SERVER_PORT 22000

struct block_device {
private:
    int _fd;
    uint64_t _block_device_size;
    bool _read_only;
    std::shared_timed_mutex _mutex;

    uint32_t get_actual_size(uint32_t size, uint64_t offset) const {
        uint32_t actual_size = 0;
        if (offset < _block_device_size) {
            // Do nothing if offset + size causes integer overflow.
            if (offset + size < offset) {
                return 0;
            }
            if (offset + size > _block_device_size) {
                size = _block_device_size - offset;
            }
            actual_size = size;
        }
        return actual_size;
    }
public:
    block_device() = delete;
    block_device(int fd, uint64_t size, bool read_only)
        : _fd(fd)
        , _block_device_size(size)
        , _read_only(read_only) {}
    ~block_device() {
        printf("Closing disk image...\n");
        close(_fd);
    }

    bool read_only() const {
        return _read_only;
    }

    uint64_t size() const {
        return _block_device_size;
    }

    int read(char* buf, uint32_t size, uint64_t offset) {
        int ret = 0;

        size = get_actual_size(size, offset);
        _mutex.lock_shared();
        ret = pread(_fd, buf, size, offset);
        _mutex.unlock_shared();
        if (ret == -1) {
            perror("pread");
            ret = 0;
        }
        return ret;
    }

    int write(const char* buf, uint32_t size, uint64_t offset) {
        int ret = 0;

        size = get_actual_size(size, offset);
        _mutex.lock();
        ret = pwrite(_fd, buf, size, offset);
        _mutex.unlock();
        if (ret == -1) {
            perror("pwrite");
            ret = 0;
        }
        return ret;
    }
};

static std::unique_ptr<block_device> setup_block_device(const char *block_device_path, bool read_only) {
    int device_fd = -1;
    uint64_t device_size = 0;

    printf("Block device name: %s\n", block_device_path);

    struct stat sb;
    if (stat(block_device_path, &sb) == -1) {
        printf("Unable to get status of the file %s: %s\n", block_device_path, strerror(errno));
        exit(1);
    }

    // TODO: we should probably use flock on the file representing disk image.
    device_fd = open(block_device_path, ((read_only) ? O_RDONLY : O_RDWR) | O_SYNC | O_LARGEFILE);
    if (device_fd == -1) {
        perror("open");
        exit(1);
    }

    switch (sb.st_mode & S_IFMT) {
    case S_IFREG:
        device_size = sb.st_size;
        break;
    case S_IFBLK: {
        int ret = ::ioctl(device_fd, BLKGETSIZE64, &device_size);
        assert(ret == 0);
        printf("WARNING: It's not safe to use block device. Use a disk image instead (created with qemu-img for example).\n");
        break;
    } default:
        printf("Only regular file is allowed at the moment!\n");
        exit(1);
    }

    printf("Block device size: %lu bytes (%.2fG)\n", device_size, (double)device_size/(1024*1024*1024));
    printf("Read only? %s\n", read_only ? "yes" : "no");

    std::unique_ptr<block_device> dev(new block_device(device_fd, device_size, read_only));
    return std::move(dev);
}

static void handle_client_requests(int comm_fd, block_device& dev) {
    char buffer[4096];
    int ret;

    // send server info to new client
    blockv_server_info server_info_to_network = blockv_server_info::to_network(dev.size(), dev.read_only());
    write(comm_fd, (const void*)&server_info_to_network, server_info_to_network.serialized_size());

    for (;;) {
        printf("Waiting for request... ");
        fflush(stdout);
        bzero(buffer, sizeof(buffer));
        ret = read(comm_fd, buffer, sizeof(buffer));
        if (ret == 0) {
            printf("Client disconnected.\n");
            break;
        }

        blockv_request* request = (blockv_request*) buffer;
        // kill connection with a client that is unable to send proper requests.
        if (!request->is_valid()) {
            printf("Request invalid!\n");
            break;
        }

        if (request->request == blockv_requests::READ) {
            blockv_read_request* read_request = (blockv_read_request*) request;
            blockv_read_request::to_host(*read_request);

            blockv_read_response* read_response = blockv_read_response::to_network(read_request->size);
            if (!read_response) {
                printf("Failed to allocate data to fulfill read request\n");
                break;
            }

            ret = dev.read(read_response->buf, read_request->size, read_request->offset);
            if (ret == 0) {
                printf("dev.read() returned 0 for size %u and offset %u\n", read_request->size, read_request->offset);
            }
            printf("Read %u bytes at offset %u\n", read_request->size, read_request->offset);

            // adjust size of read response because dev.read() may return
            // less data than what read request asked for.
            read_response->set_size_to_network(ret);

            ret = write(comm_fd, (const void*)read_response, read_response->serialized_size());
            if (ret != read_response->serialized_size()) {
                printf("Failed to write full response to client: expected: %u, actual %u\n", read_response->serialized_size(), ret);
            }

            delete read_response;
        } else if (request->request == blockv_requests::WRITE) {
            if (dev.read_only()) {
                continue;
            }
            blockv_write_request* write_request = (blockv_write_request*) request;
            blockv_write_request::to_host(*write_request);

            std::unique_ptr<char> buf(new (std::nothrow) char[write_request->size]);
            if (!buf) {
                printf("Failed to allocate %u bytes to write request\n", write_request->size);
                break;
            }

            // Buffer may be fragmented in multiple messages, so we may need to perform
            // multiple reads to get the complete buffer.
            uint32_t buf_size_in_this_message = ret - blockv_write_request::serialized_size(0);
            memcpy(buf.get(), write_request->buf, buf_size_in_this_message);

            int64_t remaining_bytes = write_request->size - buf_size_in_this_message;
            uint32_t offset = buf_size_in_this_message;
            while (remaining_bytes > 0) {
                ret = read(comm_fd, buf.get() + offset, remaining_bytes);
                remaining_bytes -= ret;
                offset += ret;
            }
            assert(remaining_bytes == 0);

            ret = dev.write(buf.get(), write_request->size, write_request->offset);
            if (ret == 0) {
                printf("dev.write() returned 0 for size %u and offset %u\n", write_request->size, write_request->offset);
            }
            printf("Wrote %u bytes at offset %u\n", write_request->size, write_request->offset);

            blockv_write_response write_response = blockv_write_response::to_network(write_request->size);
            ret = write(comm_fd, (const void*)&write_response, blockv_write_response::serialized_size());
            if (ret != blockv_write_response::serialized_size()) {
                printf("Failed to write full response to client: expected: %u, actual %u\n", blockv_write_response::serialized_size(), ret);
            }
        } else if (request->request == blockv_requests::FINISH) {
            printf("Asked to finish\n");
            break;
        }
    }
}

int main(int argc, const char **argv) {
    int listen_fd, comm_fd, ret;
    struct sockaddr_in servaddr;
    bool read_only = false;

    if (argc != 2 && argc != 3) {
        printf("Usage:\n" \
               "%s <device file>\n" \
               "%s <device file> --read-only\n", argv[0], argv[0]);
        return -1;
    }
    if (argc == 3) {
        read_only = (std::string(argv[2]) == "--read-only");
    }

    std::unique_ptr<block_device> dev = setup_block_device(argv[1], read_only);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return -1;
    }

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(BLOCKV_SERVER_PORT);

    ret = bind(listen_fd, (struct sockaddr *) &servaddr, sizeof(servaddr));
    if (ret == -1) {
        perror("bind");
        return -1;
    }

    ret = listen(listen_fd, 10);
    if (ret == -1) {
        perror("listen");
        return -1;
    }
    printf("Listening on port number %d...\n", BLOCKV_SERVER_PORT);

    for (;;) {
        comm_fd = accept(listen_fd, (struct sockaddr*) NULL, NULL);
        printf("\n{ NEW CLIENT }\n");
        handle_client_requests(comm_fd, *dev);
        close(comm_fd);
    }

    close(listen_fd);
}
