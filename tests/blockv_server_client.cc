#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "blockv_protocol.hh"
#include <assert.h>
#include <iostream>
#include <errno.h>

int main(int argc,char **argv)
{
    int sockfd, ret;
    char sendline[100];
    char recvline[100];
    struct sockaddr_in servaddr;

    sockfd = socket(AF_INET,SOCK_STREAM,0);
    bzero(&servaddr,sizeof servaddr);

    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(22000);

    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));

    ret = connect(sockfd,(struct sockaddr *)&servaddr,sizeof(servaddr));
    if (ret == -1) {
        perror("connect");
        return -1;
    }

    ret = read(sockfd,recvline,100);
    assert(ret == blockv_server_info::serialized_size());

    blockv_server_info* server_info = (blockv_server_info*) recvline;
    blockv_server_info::to_host(*server_info);

    assert(server_info->is_valid());

    std::cout << "server info: size=" << server_info->device_size << ", ro=" << bool(server_info->read_only) << std::endl;

    blockv_read_request read_request_to_network = blockv_read_request::to_network(10, 0);

    bzero(recvline, sizeof(recvline));
    write(sockfd, (const void*)&read_request_to_network, read_request_to_network.serialized_size());
    ret = read(sockfd,recvline,100);
    std::cout << ret << std::endl;
    printf("%.*s\n", 10, recvline);

    printf("sizeof(blockv_write_request): %ld\n", sizeof(blockv_write_request));

    blockv_write_request* write_request = blockv_write_request::to_network("crazy", 5, 0);
    assert(write_request != nullptr);
    printf("serialized size: %ld\n", write_request->serialized_size());
    write(sockfd, (const void*)write_request, write_request->serialized_size());
    delete (char *) write_request;


    bzero(recvline, sizeof(recvline));
    write(sockfd, (const void*)&read_request_to_network, read_request_to_network.serialized_size());
    ret = read(sockfd,recvline,100);
    printf("\n%.*s\n", 10, recvline);

    blockv_request finish;
    finish.request = blockv_requests::FINISH;
    write(sockfd, (const void*)&finish, sizeof(blockv_request));

    sleep(1);

    return 0;
}
