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
#include <stdint.h>

#define BUFFER_SIZE 1024

#pragma pack(push, 1)
typedef struct
{
    char name[101];
    uint32_t mode;
    uint64_t size;
} file_info_t;

typedef struct
{
    char name[101];
    uint64_t offset;
} resume_request_t;
#pragma pack(pop)

int recv_all(int sockfd, void *buf, size_t len)
{
    size_t total = 0;
    ssize_t n;
    char *ptr = (char *)buf;
    while (total < len)
    {
        n = recv(sockfd, ptr + total, len - total, 0);
        if (n <= 0)
        {
            return -1;
        }
        total += n;
    }
    return 0;
}

int send_all(int sockfd, const void *buf, size_t len)
{
    size_t total = 0;
    ssize_t n;
    const char *ptr = (const char *)buf;
    while (total < len)
    {
        n = send(sockfd, ptr + total, len - total, 0);
        if (n <= 0)
        {
            return -1;
        }
        total += n;
    }
    return 0;
}

int receive_file(int sockfd)
{
    file_info_t info;

    if (recv_all(sockfd, &info, sizeof(info)) < 0)
    {
        fprintf(stderr, "Failed to receive file info: %s\n", strerror(errno));
        return -1;
    }

    char safe_name[102] = {0};
    strncpy(safe_name, info.name, sizeof(safe_name) - 1);
    safe_name[sizeof(info.name)] = '\0';

    if (safe_name[0] == '\0')
    {
        fprintf(stderr, "Received empty filename\n");
        return -1;
    }

    if (strchr(safe_name, '/') != NULL || strchr(safe_name, '\\') != NULL)
    {
        fprintf(stderr, "Filename contains path separators: %s\n", safe_name);
        return -1;
    }

    printf("Receiving file: %s, size: %" PRIu64 " bytes\n",
           safe_name, info.size);

    struct stat st;
    uint64_t existing_size = 0;
    int fd;

    if (stat(safe_name, &st) == 0)
    {
        existing_size = st.st_size;

        if (existing_size >= info.size)
        {
            printf("File already complete, skipping transfer\n");
            resume_request_t req;
            strncpy(req.name, safe_name, sizeof(req.name));
            req.offset = info.size;
            send_all(sockfd, &req, sizeof(req));
            return 0;
        }

        fd = open(safe_name, O_WRONLY | O_APPEND, info.mode);
        if (fd < 0)
        {
            fprintf(stderr, "Failed to open file %s for appending: %s\n", safe_name, strerror(errno));
            return -1;
        }

        resume_request_t req;
        strncpy(req.name, safe_name, sizeof(req.name));
        req.offset = existing_size;

        if (send_all(sockfd, &req, sizeof(req)) < 0)
        {
            fprintf(stderr, "Failed to send resume request: %s\n", strerror(errno));
            close(fd);
            return -1;
        }

        printf("Resume from offset: %" PRIu64 "\n", existing_size);
    }
    else
    {
        fd = open(safe_name, O_WRONLY | O_CREAT | O_TRUNC, info.mode);
        if (fd < 0)
        {
            fprintf(stderr, "Failed to open file %s: %s\n", safe_name, strerror(errno));
            return -1;
        }

        resume_request_t req;
        strncpy(req.name, safe_name, sizeof(req.name));
        req.offset = 0;

        if (send_all(sockfd, &req, sizeof(req)) < 0)
        {
            fprintf(stderr, "Failed to send initial request: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }

    char buffer[BUFFER_SIZE];
    uint64_t remaining = info.size - existing_size;
    uint64_t total_received = 0;

    while (remaining > 0)
    {
        size_t to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        ssize_t n = recv(sockfd, buffer, to_read, 0);
        if (n <= 0)
        {
            fprintf(stderr, "Failed to receive file data: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        if (write(fd, buffer, n) != n)
        {
            fprintf(stderr, "Failed to write file data: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        remaining -= n;
        total_received += n;

        if (info.size > 0)
        {
            int progress = (int)((total_received + existing_size) * 100 / info.size);
            int filled = progress / 5;
            printf("\r  [");
            for (int i = 0; i < 20; i++)
            {
                if (i < filled)
                    printf("█");
                else if (i == filled)
                    printf("▓");
                else
                    printf("░");
            }
            printf("] %3d%% (%llu/%llu bytes)",
                   progress,
                   (unsigned long long)(total_received + existing_size),
                   (unsigned long long)info.size);
            fflush(stdout);
        }
    }
    printf("\n");

    if (close(fd) < 0)
    {
        fprintf(stderr, "Failed to close file %s: %s\n", safe_name, strerror(errno));
        return -1;
    }

    printf("File %s received successfully.\n", safe_name);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "Invalid port number: %s\n", argv[2]);
        exit(EXIT_FAILURE);
    }

    int sockfd;
    struct sockaddr_in servaddr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0)
    {
        fprintf(stderr, "Invalid IP address: %s\n", server_ip);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        perror("connect");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Connected to %s:%d\n", server_ip, port);

    uint32_t file_count;
    if (recv_all(sockfd, &file_count, sizeof(file_count)) < 0)
    {
        fprintf(stderr, "Failed to receive file count\n");
        close(sockfd);
        return EXIT_FAILURE;
    }
    file_count = ntohl(file_count);
    printf("Server will send %u files\n", file_count);

    for (uint32_t i = 0; i < file_count; i++)
    {
        printf("\n[File %u/%u] ", i + 1, file_count);
        if (receive_file(sockfd) < 0)
        {
            fprintf(stderr, "File transfer failed\n");
            close(sockfd);
            return EXIT_FAILURE;
        }
    }

    close(sockfd);
    printf("\nAll files received successfully.\n");
    return 0;
}
