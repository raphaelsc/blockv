/*
 * Copyright (C) 2016 Raphael S. Carvalho
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <memory>
#include "blockv_protocol.hh"

static int log(const char *format, ...);

struct virtual_block_device {
    virtual ~virtual_block_device(){}

    virtual bool read_only() = 0;
    virtual uint64_t size() = 0;
    virtual ssize_t read(char *buf, size_t size, off_t offset) = 0;
    virtual ssize_t write(const char *buf, size_t size, off_t offset) = 0;
};

struct memory_based_block_device : public virtual_block_device {
private:
    void* _block_device_content = nullptr;
    uint64_t _block_device_size = 0;

public:
    ~memory_based_block_device() {
        if (_block_device_content) {
            delete (char *)_block_device_content;
        }
    }

    void set_block_device_content(void* block_device_content, size_t block_device_size) {
        _block_device_content = block_device_content;
        _block_device_size = block_device_size;
    }

    virtual bool read_only() {
        return false;
    }

    virtual uint64_t size() {
        return _block_device_size;
    }

    virtual ssize_t read(char *buf, size_t size, off_t offset) {
        memcpy(buf, (const char *)_block_device_content + offset, size);
        return size;
    }
    virtual ssize_t write(const char *buf, size_t size, off_t offset) {
        memcpy((char *)_block_device_content + offset, buf, size);
        return size;
    }
};

// Contains information about connection to a blockv server.
struct blockv_server_connection {
    blockv_server_info* server_info = nullptr;
    int sockfd = -1;

    static void cleanup_server_connection(blockv_server_connection& server_connection) {
        if (server_connection.server_info) {
            delete server_connection.server_info;
        }
        if (server_connection.sockfd != -1) {
            close(server_connection.sockfd);
        }
    }
};

struct network_block_device : public virtual_block_device {
private:
    blockv_server_connection _server_connection;
    std::string _target;
    std::mutex _mutex;
public:
    network_block_device(blockv_server_connection server_connection, const char *target)
        : _server_connection(server_connection)
        , _target(std::string(target)) {}

    ~network_block_device() {
        blockv_server_connection::cleanup_server_connection(_server_connection);
    }

    static bool is_target_valid(const char *path) { return true; }

    static int read_from_server(int sockfd, char *buf, size_t size, size_t buf_offset = 0) {
        int64_t remaining_bytes = size;
        int ret, read_bytes = 0;
        while (remaining_bytes > 0) {
            ret = ::read(sockfd, buf + buf_offset, remaining_bytes);
            // checks for underflow and possible failure on server, for example, server may be
            // killed in middle of operation.
            if (ret <= 0 || ret > size) {
                return 0;
            }
            if (ret != size) {
                log("Failed to get full response from server: expected: %ld, actual %d\n", remaining_bytes, ret);
            }
            remaining_bytes -= ret;
            buf_offset += ret;
            read_bytes += ret;
        }
        return read_bytes;
    }

    static int connect_to_blockv_server(blockv_server_connection& server_connection, const char *target) {
        int sockfd, ret;
        struct sockaddr_in servaddr;

        sockfd = socket(AF_INET,SOCK_STREAM,0);
        if (sockfd == -1) {
            log("socket: %s", strerror(errno));
            return -1;
        }
        memset(&servaddr, 0, sizeof servaddr);
        servaddr.sin_family=AF_INET;
        // FIXME: use target later to determine ip and port; ip and port are hardcoded by the time being.
        servaddr.sin_port=htons(22000);
        inet_pton(AF_INET, "127.0.0.1", &(servaddr.sin_addr));

        ret = connect(sockfd,(struct sockaddr *)&servaddr,sizeof(servaddr));
        if (ret == -1) {
            log("connect: %s", strerror(errno));
            close(sockfd);
            return -1;
        }

        size_t blockv_server_info_size = blockv_server_info::serialized_size();
        char *buf = new (std::nothrow) char[blockv_server_info_size];
        if (!buf) {
            close(sockfd);
            return -1;
        }
        ret = read_from_server(sockfd, buf, blockv_server_info_size);
        if (ret != blockv_server_info_size) {
            close(sockfd);
            delete buf;
            return -1;
        }

        blockv_server_info* server_info = (blockv_server_info*) buf;
        blockv_server_info::to_host(*server_info);
        if (!server_info->is_valid()) {
            close(sockfd);
            delete buf;
            return -1;
        }
        server_connection.server_info = server_info;
        server_connection.sockfd = sockfd;

        return 0;
    }

    // When blockv fuse faces an error trying to read or write from/to blockvserver,
    // it's important to create another socket so that subsequent requests will
    // not be affected. Example: a read request may read irrelevant data from a
    // previous read request that failed if the same socket is still used.
    int reconnect_to_blockv_server() {
        blockv_server_connection::cleanup_server_connection(_server_connection);
        blockv_server_connection server_connection;
        int ret = connect_to_blockv_server(server_connection, _target.data());
        if (!ret) {
            _server_connection = server_connection;
        }
        return ret;
    }

    const std::string& read_target() {
        return _target;
    }

    virtual bool read_only() {
        return _server_connection.server_info->read_only;
    }
    virtual uint64_t size() {
        return _server_connection.server_info->device_size;
    }

    virtual ssize_t read(char *buf, size_t size, off_t offset) {
        // TODO: avoid this lock somehow. that's needed for response to correspond the request issued to the server.
        std::lock_guard<std::mutex> lock(_mutex);
        int ret;
        blockv_read_request read_request_to_network = blockv_read_request::to_network(size, offset);

        size_t expected_response_size = blockv_read_response::predict_read_response_size(read_request_to_network);
        std::unique_ptr<char> response_buf(new (std::nothrow) char[expected_response_size]);
        if (!response_buf) {
            return 0;
        }
        memset(response_buf.get(), 0, expected_response_size);

        ret = ::write(_server_connection.sockfd, (const void*)&read_request_to_network, read_request_to_network.serialized_size());
        if (ret != read_request_to_network.serialized_size()) {
            log("Failed to send full read request to server: expected: %u, actual %d\n", read_request_to_network.serialized_size(), ret);
            reconnect_to_blockv_server();
            return 0;
        }

        // Read only blockv_read_response::size to get the size of response.
        ret = read_from_server(_server_connection.sockfd, response_buf.get(), blockv_read_response::metadata_size());
        if (ret != blockv_read_response::metadata_size()) {
            reconnect_to_blockv_server();
            return 0;
        }

        blockv_read_response* read_response = (blockv_read_response*) response_buf.get();
        blockv_read_response::to_host(*read_response);
        if (read_response->size != size) {
            // This also handles the corner case in which response size is bigger than expected,
            // potentially leading to a buffer overflow.
            log("Read response size: expected: %u, actual: %u\n", size, read_response->size);
            reconnect_to_blockv_server();
            return 0;
        }

        ret = read_from_server(_server_connection.sockfd, response_buf.get(), read_response->size, blockv_read_response::metadata_size());
        if (ret != read_response->size) {
            log("Failed to get full response from server: expected: %ld, actual %d\n", read_response->size, ret);
            reconnect_to_blockv_server();
            return 0;
        }
        memcpy(buf, (const char *)read_response->buf, read_response->size);
        return ret;
    }

    virtual ssize_t write(const char *buf, size_t size, off_t offset) {
        std::lock_guard<std::mutex> lock(_mutex);
        int ret;

        blockv_write_request* write_request = blockv_write_request::to_network(buf, size, offset);
        if (write_request == nullptr) {
            return 0;
        }

        ssize_t written = ::write(_server_connection.sockfd, (const void*)write_request, write_request->serialized_size());
        if (written != write_request->serialized_size()) {
            log("Failed to send full write request to server: expected: %u, actual %d\n", write_request->serialized_size(), ret);
            reconnect_to_blockv_server();
            delete (char *) write_request;
            return 0;
        }
        delete (char *) write_request;

        blockv_write_response write_response;
        ret = read_from_server(_server_connection.sockfd, (char*)&write_response, blockv_write_response::serialized_size());
        if (ret != blockv_write_response::serialized_size()) {
            log("Failed to get full response from server: expected: %ld, actual %d\n", blockv_write_response::serialized_size(), ret);
            reconnect_to_blockv_server();
            return 0;
        }
        // FIXME: ignoring write response by the time being.

        return size;
    }
};

struct blockv_fuse {
private:
    std::unordered_map<std::string, virtual_block_device*> _block_devices;
    std::unordered_map<std::string, virtual_block_device*> _target_to_block_device;

public:
    ~blockv_fuse() {
        for (auto it : _block_devices) {
            delete it.second;
        }
        _block_devices.clear();
        _target_to_block_device.clear();
    }

    void add_memory_based_block_device(const char *path) {
        _block_devices.emplace(std::string(path), new memory_based_block_device());
    }

    void add_network_based_block_device(const char *path, const char *target, blockv_server_connection server_connection) {
        auto nbd = new network_block_device(server_connection, target);
        _block_devices.emplace(std::string(path), nbd);
        _target_to_block_device.emplace("/" + std::string(target), nbd);
    }

    void remove_block_device(const char *path) {
        // TODO: implement.
        // we need to get iterator to block device to be removed, delete it, and if network based,
        // remove the entry from _target_to_block_device
        return;
    }

    const std::unordered_map<std::string, virtual_block_device*>& block_devices() {
        return _block_devices;
    }

    virtual_block_device* get_block_device(const char *path) {
        auto it = _block_devices.find(std::string(path));
        if (it != _block_devices.end()) {
            return it->second;
        }
        auto it2 = _target_to_block_device.find(std::string(path));
        if (it2 == _target_to_block_device.end()) {
            return nullptr;
        }
        return it2->second;
    }

    bool block_device_exists(const char *path) {
        return (get_block_device(path)) ? true : false;
    }
};

static struct blockv_fuse* get_filesystem_context() {
    struct fuse_context* context = fuse_get_context();
    return (struct blockv_fuse*) context->private_data;
}

int log(const char *format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[256];
    snprintf(buffer, 256, "blockv_fuse log: %s\n", format);
    int ret = vprintf(buffer, args);

    va_end(args);
    return ret;
}

static int fs_getattr(const char *path, struct stat *stbuf)
{
    int res = 0;
    struct blockv_fuse* fs = get_filesystem_context();

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        auto block_device = fs->get_block_device(path);
        if (!block_device) {
            res = -ENOENT;
        } else {
            bool is_regular_file = dynamic_cast<memory_based_block_device*>(block_device);
            if (!is_regular_file) {
                // target of a network block device will be a regular file. Otherwise the target
                // would point to iself, which would lead to an infinite loop of links.
                network_block_device* nbd = dynamic_cast<network_block_device*>(block_device);
                is_regular_file = (nbd->read_target() == std::string(path + 1));
            }

            stbuf->st_mode = ((is_regular_file) ? S_IFREG : S_IFLNK) | (block_device->read_only() ? 0444 : 0644);
            stbuf->st_nlink = 1;
            stbuf->st_size = block_device->size();
        }
    }

    return res;
}

static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
        off_t offset, struct fuse_file_info *fi)
{
    struct blockv_fuse* fs = get_filesystem_context();

    if (strcmp(path, "/") != 0) {
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    for (const auto& it : fs->block_devices()) {
        filler(buf, it.first.data() + 1, NULL, 0);
    }

    return 0;
}

static int fs_open(const char *path, struct fuse_file_info *fi) {
    struct blockv_fuse* fs = get_filesystem_context();
    auto block_device = fs->get_block_device(path);

    if (!block_device) {
        return -ENOENT;
    }

    if (block_device->read_only() && ((fi->flags & 3) != O_RDONLY)) {
        return -EACCES;
    }

    return 0;
}

static int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    struct blockv_fuse* fs = get_filesystem_context();
    bool exclusive = fi->flags & O_EXCL; // TODO: CHECK if it's correct

    if (fs->block_device_exists(path)) {
        if (exclusive) {
            return -EEXIST;
        }
    } else {
        fs->add_memory_based_block_device(path);
    }

    return 0;
}

static int fs_symlink(const char *target, const char *linkpath) {
    struct blockv_fuse* fs = get_filesystem_context();

    if (!network_block_device::is_target_valid(target)) {
        return -ENOENT;
    }

    if (fs->block_device_exists(linkpath)) {
        return -EEXIST;
    } else {
        blockv_server_connection server_connection;
        if (network_block_device::connect_to_blockv_server(server_connection, target) == -1) {
            return -EIO;
        }

        fs->add_network_based_block_device(linkpath, target, server_connection);
    }

    return 0;
}

static int fs_readlink(const char *path, char *buf, size_t size) {
    struct blockv_fuse* fs = get_filesystem_context();

    if (!fs->block_device_exists(path)) {
        return -ENOENT;
    }

    // Readlink is only supported by network_based_block_device.
    network_block_device* block_device = dynamic_cast<network_block_device*>(fs->get_block_device(path));
    if (!block_device) {
        return -EPERM;
    }

    const std::string& target = block_device->read_target();
    size_t to_write = std::min(target.size(), size - 1);
    memcpy(buf, target.c_str(), to_write);
    // The buffer should be filled with a null terminated string.
    buf[to_write] = '\0';

    return 0;
}

static int fs_truncate(const char *path, off_t size) {
    struct blockv_fuse* fs = get_filesystem_context();

    if (!fs->block_device_exists(path)) {
        return -ENOENT;
    }

    // Truncate is only supported by memory_based_block_device.
    memory_based_block_device* block_device = dynamic_cast<memory_based_block_device*>(fs->get_block_device(path));
    if (!block_device) {
        return -EPERM;
    }

    // Resize isn't allowed.
    if (block_device->size() != 0) {
        return -EPERM;
    }

    void* block_device_content = (void*) new (std::nothrow) char[size];
    if (!block_device_content) {
        return -EIO;
    }
    block_device->set_block_device_content(block_device_content, size);

    return 0;
}

static int rw(const char *path, const void* buf, size_t size, off_t offset, bool read,
        std::function<size_t(virtual_block_device*, const void*, size_t, off_t)> operation) {
    struct blockv_fuse* fs = get_filesystem_context();
    auto block_device = fs->get_block_device(path);

    if (!block_device) {
        return -ENOENT;
    }

    if (!read && block_device->read_only()) {
        return -EBADF;
    }

    size_t len = block_device->size();
    int ret = 0;

    if (offset < len) {
        if (offset + size > len) {
            size = len - offset;
        }
        ret = operation(block_device, buf, size, offset);
        if (ret != size) {
            log("Failed to %s %ld bytes at offset %ld of %s, actual: %ld", (read) ? "read" : "write", size, offset, path, ret);
            return -EIO;
        }
    }

    return ret;
}

static int fs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    return rw(path, buf, size, offset, true, [] (auto block_device, const void *buf, size_t size, off_t offset) {
        return block_device->read((char *)buf, size, offset);
    });
}

static int fs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    return rw(path, buf, size, offset, false, [] (auto block_device, const void *buf, size_t size, off_t offset) {
        return block_device->write((const char *)buf, size, offset);
    });
}

static struct fuse_operations fs_oper;
static struct blockv_fuse fs;

int main(int argc, char *argv[])
{
    fs_oper.getattr = fs_getattr;
    fs_oper.readdir = fs_readdir;
    fs_oper.open = fs_open;
    fs_oper.create = fs_create;
    fs_oper.symlink = fs_symlink;
    fs_oper.readlink = fs_readlink;
    fs_oper.truncate = fs_truncate;
    fs_oper.read = fs_read;
    fs_oper.write = fs_write;

    log("Initializing fuse...");
    return fuse_main(argc, argv, &fs_oper, (void*) &fs);
}
