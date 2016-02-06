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

struct fs {
    const char *block_device_name;
    void *block_device_content;
    size_t block_device_size;
};

static struct fs* get_fs() {
    struct fuse_context* context = fuse_get_context();
    return (struct fs*) context->private_data;
}

static int fs_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;
        struct fs* fs = get_fs();

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else if (strcmp(path, fs->block_device_name) == 0) {
		stbuf->st_mode = S_IFREG | 0777;
		stbuf->st_nlink = 1;
		stbuf->st_size = fs->block_device_size;
	} else
		res = -ENOENT;

	return res;
}

static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
        struct fs* fs = get_fs();

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, fs->block_device_name + 1, NULL, 0);

	return 0;
}

static int fs_open(const char *path, struct fuse_file_info *fi)
{
        struct fs* fs = get_fs();

	if (strcmp(path, fs->block_device_name) != 0)
		return -ENOENT;

	return 0;
}

static int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    return fs_open(path, fi);
}

static int fs_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	size_t len;
        struct fs* fs = get_fs();

	if(strcmp(path, fs->block_device_name) != 0)
		return -ENOENT;

	len = fs->block_device_size;
	if (offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, (const char *) fs->block_device_content + offset, size);
	} else
		size = 0;

	return size;
}

static int fs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
        struct fs* fs = get_fs();
        size_t len;

	if(strcmp(path, fs->block_device_name) != 0)
		return -ENOENT;

	len = fs->block_device_size;
	if (offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy((char *)fs->block_device_content + offset, buf, size);
	} else
		size = 0;

	return size;
}

static struct fuse_operations fs_oper;
static struct fs fs;

int main(int argc, char *argv[])
{
        fs.block_device_name = "/virtual_block_device";
        fs.block_device_content = new char[32*1024*1024];
        fs.block_device_size = 32*1024*1024;

        fs_oper.getattr = fs_getattr;
        fs_oper.readdir = fs_readdir;
        fs_oper.open = fs_open;
        fs_oper.create = fs_create;
        fs_oper.read = fs_read;
        fs_oper.write = fs_write;

	return fuse_main(argc, argv, &fs_oper, (void*) &fs);
}
