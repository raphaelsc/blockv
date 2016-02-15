#BlockV

##Introduction

blockv project is composed of blockv server and blockv FUSE (client).
blockv server is used to export a block device that can be accessed by blockv FUSE.

With blockv FUSE, it's easy to keep access to a large number of block devices that are exported by respective blockv servers.

This project is based on the concept of network block device. For further information:
https://en.wikipedia.org/wiki/Network_block_device

##Getting started

Compile full project with:
```
g++ --std=c++14 `pkg-config fuse --cflags --libs` blockv_fuse.cc -o blockv_fuse;
g++ --std=c++14 blockv_server.cc -o blockv_server -lpthread;
```

There is no makefile because I am lazy, but I will write one as soon as possible.


##Playing with network-based block device
At this section, you will learn how to export a file system to the outside world.


#### Server side

1) Compile project for server side:
```
g++ --std=c++14 blockv_server.cc -o blockv_server -lpthread;
```

2) Create a pseudo block device that will be exported to the network:
```
qemu-img create -f raw -o size=500M ./pseudo_block_device.raw;
```

3) Format pseudo block device with a file system:
```
mkfs.ext2 ./pseudo_block_device.raw;
mkdir ./mount_point;
sudo mount -t ext2 ./pseudo_block_device.raw ./mount_point;
sudo umount ./mount_point;

NOTE: copy all files you want to share to the folder ./mount_point.
```

4) Run blockv server with pseudo block device:
```
./blockv_server ./pseudo_block_device.raw;
```

Read-only mode (write request is disallowed):
```
./blockv_server ./pseudo_block_device.raw --read-only;
```


#### Client side

1) Compile project for client side:
```
g++ --std=c++14 blockv_server.cc -o blockv_server -lpthread;
```

2) Mount blockv:
```
mkdir ./blockv_mount_point;
./blockv_fuse -d ./blockv_mount_point -o allow_root;

NOTE: *allow_root* option requires adding *user_allow_other* to */etc/fuse.conf*
```

3) Create a network-based block device that points to the server above:
```
### Replace localhost and 22000 by server ip and port, respectively.
ln -s localhost:22000 ./blockv_mount_point/remote_block_device;
```

It's possible to see the ip and port associated with a remote block device, look:
```
$ ls -l ./blockv_mount_point/remote_block_device;
lr--r--r--. 1 root root 524288000 Dec 31  1969 ./blockv_mount_point/remote_block_device -> localhost:22000
```

4) Mount the network-based block device:
```
mkdir ./mount_point_for_remote_file_system;
sudo mount -t ext2 -o loop ./blockv_mount_point/remote_block_device ./mount_point_for_remote_file_system;
```

At this point, you can fully use the file system stored in the remote block device.


##Playing with memory-based block device

1) Mount blockv:
```
mkdir ./blockv_mount_point;
./blockv_fuse -d ./blockv_mount_point -o allow_root;

NOTE: *allow_root* option requires adding *user_allow_other* to */etc/fuse.conf*
```

2) Create a memory-based block device of 30MB:
```
truncate -s 30M ./blockv_mount_point/virtual_block_device;
```

3) Format the memory-based block device with ext2 file system:
```
mkfs.ext2 ./blockv_mount_point/virtual_block_device;
```
ext2 filesystem was chosen arbitrarily. Any other file system supported by Linux should work as fine.

4) Mount the ext2 filesystem with:
```
mkdir ./mount_point2;
sudo mount -t ext2 -o loop ./blockv_mount_point/virtual_block_device ./mount_point2;
```

At this point, you can fully use the file system stored in the memory-based block device.
