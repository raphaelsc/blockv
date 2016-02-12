/*
 * Copyright (C) 2016 Raphael S. Carvalho
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "blockv_protocol.hh"
#include <assert.h>

struct fake_device {
    char data[11] = "hello sir!";
    size_t len = 10;

    fake_device() = default;

    int read(char* buf, size_t size, off_t offset) {
        int ret = 0;

        if (offset < len) {
            if (offset + size > len) {
                size = len - offset;
            }
            memcpy(buf, (const char *)data + offset, size);
            ret = size;
        }

        return ret;
    }

    int write(const char* buf, size_t size, off_t offset) {
        int ret = 0;

        if (offset < len) {
            if (offset + size > len) {
                size = len - offset;
            }
            memcpy((char *)data + offset, buf, size);
            ret = size;
        }

        return ret;
    }
};

int main()
{
    char str[4096];
    int listen_fd, comm_fd, ret;

    struct sockaddr_in servaddr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    bzero( &servaddr, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(22000);

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
    printf("Listening on port number 22000...\n");

    blockv_server_info server_info_to_network = blockv_server_info::to_network(10, true);
    fake_device dev;

    for (;;) {
        comm_fd = accept(listen_fd, (struct sockaddr*) NULL, NULL);

        // send server info to client
        write(comm_fd, (const void*)&server_info_to_network, server_info_to_network.serialized_size());

        printf("\n{ NEW CLIENT }\n");

        for (;;) {
            bzero(str, sizeof(str));
            ret = read(comm_fd, str, sizeof(str));
            if (ret == 0) {
                printf("Client disconnected.\n");
                break;
            }

            blockv_request* request = (blockv_request*) str;
            assert(request->is_valid());

            if (request->request == blockv_requests::READ) {
                blockv_read_request* read_request = (blockv_read_request*) request;
                blockv_read_request::to_host(*read_request);

                blockv_read_response* read_response = blockv_read_response::to_network(read_request->size);
                if (!read_response) {
                    printf("Failed to allocate data to fulfill read request\n");
                    break;
                }

                ret = dev.read(read_response->buf, read_request->size, read_request->offset);
                printf("Read %u bytes at offset %u: \'%.*s\'\n", read_request->size, read_request->offset, ret, read_response->buf);

                // adjust size of read response because dev.read() may return
                // less data than what read request asked for.
                read_response->set_size_to_network(ret);
                write(comm_fd, (const void*)read_response, read_response->serialized_size());

                delete read_response;
            } else if (request->request == blockv_requests::WRITE) {
                // TODO: ignore write request if device is read-only.
                blockv_write_request* write_request = (blockv_write_request*) request;
                blockv_write_request::to_host(*write_request);

                char* buf;
                try {
                    buf = new char[write_request->size];
                } catch (...) {
                    printf("Failed to allocate %u bytes to write request\n", write_request->size);
                    break;
                }

                // Buffer may be fragmented in multiple messages, so we may need to perform
                // multiple reads to get the complete buffer.
                uint32_t buf_size_in_this_message = ret - blockv_write_request::serialized_size(0);
                memcpy(buf, write_request->buf, buf_size_in_this_message);

                int64_t remaining_bytes = write_request->size - buf_size_in_this_message;
                uint32_t offset = buf_size_in_this_message;
                while (remaining_bytes > 0) {
                    ret = read(comm_fd, buf + offset, remaining_bytes);
                    remaining_bytes -= ret;
                    offset += ret;
                }
                assert(remaining_bytes == 0);

                dev.write(buf, write_request->size, write_request->offset);
                printf("Wrote \'%.*s\' (%u bytes) at offset %u\n", write_request->size, buf,
                    write_request->size, write_request->offset);
                delete buf;
            } else if (request->request == blockv_requests::FINISH) {
                printf("Asked to finish\n");
                break;
            }
        }

        close(comm_fd);
    }

    close(listen_fd);
}
