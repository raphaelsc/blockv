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
#include <unordered_map>
#include <functional>

struct virtual_block_device {
    virtual ~virtual_block_device(){}

    virtual bool read_only() = 0;
    virtual size_t size() = 0;
    virtual size_t read(char *buf, size_t size, off_t offset) = 0;
    virtual size_t write(const char *buf, size_t size, off_t offset) = 0;
};

struct memory_based_block_device : public virtual_block_device {
private:
    void* _block_device_content = nullptr;
    size_t _block_device_size = 0;

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

    virtual size_t size() {
        return _block_device_size;
    }

    virtual size_t read(char *buf, size_t size, off_t offset) {
        memcpy(buf, (const char *)_block_device_content + offset, size);
        return size;
    }
    virtual size_t write(const char *buf, size_t size, off_t offset) {
        memcpy((char *)_block_device_content + offset, buf, size);
        return size;
    }
};

struct network_block_device : public virtual_block_device {
private:
    std::string _target;
public:
    network_block_device(const char *target)
        : _target(std::string(target)) {}

    static bool is_target_valid(const char *path) { return true; }

    const std::string& read_target() {
        return _target;
    }

    virtual bool read_only() { return false; }
    virtual size_t size() { return 0; }
    virtual size_t read(char *buf, size_t size, off_t offset) { return 0; }
    virtual size_t write(const char *buf, size_t size, off_t offset) { return 0; };
};

struct blockv_fuse {
private:
    std::unordered_map<std::string, virtual_block_device*> _block_devices;

public:
    ~blockv_fuse() {
        for (auto it : _block_devices) {
            delete it.second;
        }
        _block_devices.clear();
    }

    void add_memory_based_block_device(const char *path) {
        _block_devices.emplace(std::string(path), new memory_based_block_device());
    }

    void add_network_based_block_device(const char *path, const char *target) {
        _block_devices.emplace(std::string(path), new network_block_device(target));
    }

    void remove_block_device(const char *path) {
        _block_devices.erase(std::string(path));
    }

    const std::unordered_map<std::string, virtual_block_device*>& block_devices() {
        return _block_devices;
    }

    virtual_block_device* get_block_device(const char *path) {
        auto it = _block_devices.find(std::string(path));
        if (it == _block_devices.end()) {
            return nullptr;
        }
        return it->second;
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
            bool is_memory_based = dynamic_cast<memory_based_block_device*>(block_device);

            stbuf->st_mode = ((is_memory_based) ? S_IFREG : S_IFLNK) | (block_device->read_only() ? 0444 : 0644);
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

    if (strcmp(path, "/") != 0)
            return -ENOENT;

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
        // TODO: create connection object here (using a static method of network_block_device)
        // to check connectivity and pass it to add_network_based_block_device.
        fs->add_network_based_block_device(linkpath, target);
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
    if (block_device->size()) {
        return -EPERM;
    }

    void* block_device_content;
    try {
        block_device_content = new char[size];
    } catch (...) {
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
    if (offset < len) {
        if (offset + size > len) {
            size = len - offset;
        }
        size_t ret = operation(block_device, buf, size, offset);
        if (ret != size) {
            log("Failed to %s %ld bytes at offset %ld of %s", (read) ? "read" : "write", size, offset, path);
            return -EIO;
        }
        size = ret;
    } else {
        size = 0;
    }

    return size;
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
