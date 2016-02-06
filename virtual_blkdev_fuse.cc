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
#include <unordered_map>
#include <functional>

struct virtual_block_device {
    virtual ~virtual_block_device(){}

    virtual size_t size() = 0;
    virtual size_t read(char *buf, size_t size, off_t offset) = 0;
    virtual size_t write(const char *buf, size_t size, off_t offset) = 0;
};

struct memory_based_block_device : public virtual_block_device {
    void *block_device_content = nullptr;
    size_t block_device_size = 0;

    memory_based_block_device() {
        block_device_content = new char[32*1024*1024];
        block_device_size = 32*1024*1024;
    }

    ~memory_based_block_device() {
        delete (char *)block_device_content;
    }

    virtual size_t size() {
        return block_device_size;
    }

    virtual size_t read(char *buf, size_t size, off_t offset) {
        memcpy(buf, (const char *)block_device_content + offset, size);
        return size;
    }
    virtual size_t write(const char *buf, size_t size, off_t offset) {
        memcpy((char *)block_device_content + offset, buf, size);
        return size;
    }
};

struct network_block_device : public virtual_block_device {
    virtual size_t size() { return 0; }
    virtual size_t read(char *buf, size_t size, off_t offset) { return 0; }
    virtual size_t write(const char *buf, size_t size, off_t offset) { return 0; };
};

struct virtual_blockdev_fs {
private:
    std::unordered_map<std::string, virtual_block_device*> _block_devices;

public:
    ~virtual_blockdev_fs() {
        for (auto it : _block_devices) {
            delete it.second;
        }
        _block_devices.clear();
    }

    void add_memory_based_block_device(const char *path) {
        _block_devices.emplace(std::string(path), new memory_based_block_device());
    }

    void add_network_based_block_device(const char *path, const char *linkpath) {
        _block_devices.emplace(std::string(path), new network_block_device());
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

static struct virtual_blockdev_fs* get_filesystem_context() {
    struct fuse_context* context = fuse_get_context();
    return (struct virtual_blockdev_fs*) context->private_data;
}

static int fs_getattr(const char *path, struct stat *stbuf)
{
    int res = 0;
    struct virtual_blockdev_fs* fs = get_filesystem_context();

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
    } else {
        auto block_device = fs->get_block_device(path);
        if (!block_device) {
            res = -ENOENT;
        } else {
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = block_device->size();
        }
    }

    return res;
}

static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
    struct virtual_blockdev_fs* fs = get_filesystem_context();

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
    struct virtual_blockdev_fs* fs = get_filesystem_context();

    if (!fs->block_device_exists(path)) {
        return -ENOENT;
    }

    return 0;
}

static int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    return fs_open(path, fi);
}

static int rw(const char *path, const void* buf, size_t size, off_t offset,
        std::function<size_t(virtual_block_device*, const void*, size_t, off_t)> operation) {
    struct virtual_blockdev_fs* fs = get_filesystem_context();
    auto block_device = fs->get_block_device(path);

    if (!block_device) {
        return -ENOENT;
    }

    size_t len = block_device->size();
    if (offset < len) {
        if (offset + size > len) {
            size = len - offset;
        }
        size = operation(block_device, buf, size, offset);
    } else {
        size = 0;
    }

    return size;
}

static int fs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    return rw(path, buf, size, offset, [] (auto block_device, const void *buf, size_t size, off_t offset) {
        return block_device->read((char *)buf, size, offset);
    });
}

static int fs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    return rw(path, buf, size, offset, [] (auto block_device, const void *buf, size_t size, off_t offset) {
        return block_device->write((const char *)buf, size, offset);
    });
}

static struct fuse_operations fs_oper;
static struct virtual_blockdev_fs fs;

int main(int argc, char *argv[])
{
    fs.add_memory_based_block_device("/virtual_block_device");

    fs_oper.getattr = fs_getattr;
    fs_oper.readdir = fs_readdir;
    fs_oper.open = fs_open;
    fs_oper.create = fs_create;
    fs_oper.read = fs_read;
    fs_oper.write = fs_write;

    return fuse_main(argc, argv, &fs_oper, (void*) &fs);
}
