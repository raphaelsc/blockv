/*
 * Copyright (C) 2016 Raphael S. Carvalho
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <arpa/inet.h>

#define BLOCKV_MAGIC_VALUE 0xB0B0B0B0

struct blockv_server_info {
    uint32_t magic_value;
    uint32_t device_size;
    uint8_t read_only;

    blockv_server_info() = default;

    bool is_valid() const {
        return magic_value == BLOCKV_MAGIC_VALUE;
    }

    static size_t serialized_size() {
        return sizeof(magic_value) + sizeof(device_size) + sizeof(read_only);
    }

    static blockv_server_info to_network(uint32_t device_size, bool read_only) {
        blockv_server_info to;
        to.magic_value = htonl(BLOCKV_MAGIC_VALUE);
        to.device_size = htonl(device_size);
        to.read_only = uint8_t(read_only);
        return to;
    }

    static void to_host(blockv_server_info& server_info) {
        server_info.magic_value = ntohl(server_info.magic_value);
        server_info.device_size = ntohl(server_info.device_size);
    }
} __attribute__((packed));

enum blockv_requests : uint8_t {
    FIRST = 0xB0, // WARNING: don't change FIRST or LAST.
    READ = 0xB1,
    WRITE = 0xB2,
    FINISH = 0xB3,
    LAST = FINISH + 1,
};

struct blockv_read_request {
    uint8_t request;
    uint32_t size;
    uint32_t offset;

    blockv_read_request() = default;

    static size_t serialized_size() {
        return sizeof(request) + sizeof(size) + sizeof(offset);
    }

    static blockv_read_request to_network(uint32_t size, uint32_t offset) {
        blockv_read_request to;
        to.request = blockv_requests::READ;
        to.size = htonl(size);
        to.offset = htonl(offset);
        return to;
    }

    static void to_host(blockv_read_request& read_request) {
        read_request.size = ntohl(read_request.size);
        read_request.offset = ntohl(read_request.offset);
    }
} __attribute__((packed));

struct blockv_write_request {
    uint8_t request;
    uint32_t size;
    uint32_t offset;
    char buf[];

    blockv_write_request() = delete;

    static size_t serialized_size(uint32_t buf_size) {
        return sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) + buf_size;
    }

    size_t serialized_size() {
        return serialized_size(ntohl(size));
    }

    static blockv_write_request* to_network(const char *buf, uint32_t buf_size, uint32_t off) {
        size_t bytes_to_allocate = serialized_size(buf_size);
        blockv_write_request* to;

        try {
            to = (blockv_write_request*) new char[bytes_to_allocate];
        } catch (...) {
            return nullptr;
        }

        to->request = blockv_requests::WRITE;
        to->size = htonl(buf_size);
        to->offset = htonl(off);
        memcpy(to->buf, buf, buf_size);
        return to;
    }

    static void to_host(blockv_write_request& write_request) {
        write_request.size = ntohl(write_request.size);
        write_request.offset = ntohl(write_request.offset);
    }
} __attribute__((packed));

struct blockv_request {
    uint8_t request;

    bool is_valid() const {
        return request > blockv_requests::FIRST && request < blockv_requests::LAST;
    }
} __attribute__((packed));

#endif