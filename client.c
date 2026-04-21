#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>

#define BUFFER_SIZE 1024

// 与服务端完全一致的结构体定义
#pragma pack(push, 1)
typedef struct {
    char name[101];
    uint32_t mode;
    uint64_t size;
} file_info_t;
#pragma pack(pop)

int recv_all(int sockfd, void *buf, size_t len) {
    size_t total = 0;
    ssize_t n;
    char *ptr = (char *)buf;
    while (total < len) {
        n = recv(sockfd, ptr + total, len - total, 0);
        if (n <= 0) {
            return -1;
        }
        total += n;
    }
    return 0;
}

int receive_file(int sockfd) {
    file_info_t info;

    // 1. 接收文件信息结构体
    if (recv_all(sockfd, &info, sizeof(info)) < 0) {
        fprintf(stderr, "Failed to receive file info: %s\n", strerror(errno));
        return -1;
    }

    // 2. 确保文件名以 '\0' 结尾
    char safe_name[102] = {0};
    strncpy(safe_name, info.name, sizeof(info.name));
    safe_name[sizeof(info.name)] = '\0';

    // 检查文件名是否为空或包含非法字符
    if (safe_name[0] == '\0') {
        fprintf(stderr, "Received empty filename\n");
        return -1;
    }

    // 检查文件名是否包含路径分隔符，防止路径遍历攻击
    if (strchr(safe_name, '/') != NULL || strchr(safe_name, '\\') != NULL) {
        fprintf(stderr, "Filename contains path separators: %s\n", safe_name);
        return -1;
    }

    printf("Receiving file: %s, size: %" PRIu64 " bytes, mode: %o\n",
           safe_name, info.size, info.mode);

    // 3. 创建本地文件
    int fd = open(safe_name, O_WRONLY | O_CREAT | O_TRUNC, info.mode);
    if (fd < 0) {
        fprintf(stderr, "Failed to open file %s: %s\n", safe_name, strerror(errno));
        return -1;
    }

    // 4. 接收文件内容
    char buffer[BUFFER_SIZE];
    uint64_t remaining = info.size;
    while (remaining > 0) {
        size_t to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        ssize_t n = recv(sockfd, buffer, to_read, 0);
        if (n <= 0) {
            fprintf(stderr, "Failed to receive file data: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        if (write(fd, buffer, n) != n) {
            fprintf(stderr, "Failed to write file data: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        remaining -= n;
    }

    if (close(fd) < 0) {
        fprintf(stderr, "Failed to close file %s: %s\n", safe_name, strerror(errno));
        return -1;
    }

    printf("File %s received successfully.\n", safe_name);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", argv[2]);
        exit(EXIT_FAILURE);
    }

    int sockfd;
    struct sockaddr_in servaddr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
      if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address: %s\n", server_ip);
      close(sockfd);
    exit(EXIT_FAILURE);
    }

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Connected to %s:%d\n", server_ip, port);

    // 接收文件，服务器会在发送完所有文件后关闭连接
    while (1) {
        if (receive_file(sockfd) < 0) {
            // 连接关闭是正常的退出条件
            if (errno == ECONNRESET || errno == EPIPE) {
                printf("Server closed connection. All files received.\n");
                break;
            }
            fprintf(stderr, "File transfer failed: %s\n", strerror(errno));
            close(sockfd);
            return EXIT_FAILURE;
        }
    }

    close(sockfd);
    printf("All files received successfully.\n");
    return 0;
}