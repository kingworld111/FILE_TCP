#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <sys/time.h>
#include <stdint.h>

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

typedef struct
{
	int sock_conn;
	char ip[16];
	unsigned short port;
	time_t online_time;
	char **send_file_list;
	int send_file_cnt;
} client_info_t;

static volatile int running = 1;

void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

int send_all(int sockfd, const void *buf, size_t len)
{
	size_t total = 0;
	ssize_t n;
	const char *ptr = (const char *)buf;
	while (total < len)
	{
		n = write(sockfd, ptr + total, len - total);
		if (n <= 0)
		{
			return -1;
		}
		total += n;
	}
	return 0;
}

#define MAX_CONNECTIONS 100
int connection_count = 0;
pthread_mutex_t connection_mutex = PTHREAD_MUTEX_INITIALIZER;

void *comm_thr(void *arg);
int send_file(int sock, const char *file_path);

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	if (argc == 1)
	{
		fprintf(stderr, "Usage: %s file1 [...]\n", argv[0]);
		return 1;
	}

	for (int i = 1; i < argc; i++)
	{
		if (-1 == access(argv[i], R_OK))
		{
			fprintf(stderr, "文件 %s 不存在或不可读！\n", argv[i]);
			return 1;
		}

		struct stat st;
		if (lstat(argv[i], &st) == -1)
		{
			perror("lstat fail");
			return 1;
		}

		if (!S_ISREG(st.st_mode))
		{
			fprintf(stderr, "%s 不是普通文件！\n", argv[i]);
			return 1;
		}

		if (strlen(argv[i]) >= PATH_MAX)
		{
			fprintf(stderr, "文件路径 %s 过长！\n", argv[i]);
			return 1;
		}

		if (strstr(argv[i], "../") != NULL || strstr(argv[i], "..\\") != NULL)
		{
			fprintf(stderr, "文件路径 %s 包含非法字符！\n", argv[i]);
			return 1;
		}
	}

	int sock_listen = socket(AF_INET, SOCK_STREAM, 0);

	if (-1 == sock_listen)
	{
		perror("socket fail");
		exit(1);
	}

	int val = 1;
	setsockopt(sock_listen, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

	struct sockaddr_in myaddr;
	myaddr.sin_family = AF_INET;
	myaddr.sin_addr.s_addr = INADDR_ANY;
	myaddr.sin_port = htons(6666);

	if (-1 == bind(sock_listen, (struct sockaddr *)&myaddr, sizeof(myaddr)))
	{
		perror("bind fail");
		exit(1);
	}

	if (-1 == listen(sock_listen, 5))
	{
		perror("listen fail");
		exit(1);
	}

	int flags = fcntl(sock_listen, F_GETFL, 0);
	if (flags == -1)
	{
		perror("fcntl F_GETFL fail");
		exit(1);
	}
	if (fcntl(sock_listen, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		perror("fcntl F_SETFL fail");
		exit(1);
	}

	int sock_conn;
	pthread_t tid;
	client_info_t *pci = NULL;
	struct sockaddr_in client_addr;
	socklen_t addr_len = sizeof(client_addr);

	struct timeval tv;
	tv.tv_sec = 10;
	tv.tv_usec = 0;

	fd_set read_set;
	int max_fd;

	printf("Server started, listening on port 6666\n");
	while (running)
	{
		pthread_mutex_lock(&connection_mutex);
		if (connection_count >= MAX_CONNECTIONS)
		{
			pthread_mutex_unlock(&connection_mutex);
			sleep(1);
			continue;
		}
		pthread_mutex_unlock(&connection_mutex);

		FD_ZERO(&read_set);
		FD_SET(sock_listen, &read_set);
		max_fd = sock_listen + 1;

		tv.tv_sec = 5;
		tv.tv_usec = 0;

		int ret = select(max_fd, &read_set, NULL, NULL, &tv);
		if (ret < 0)
		{
			if (errno == EINTR)
				continue;
			perror("select fail");
			continue;
		}
		else if (ret == 0)
		{
			continue;
		}

		if (FD_ISSET(sock_listen, &read_set))
		{
			sock_conn = accept(sock_listen, (struct sockaddr *)&client_addr, &addr_len);

			if (-1 == sock_conn)
			{
				if (errno == EWOULDBLOCK || errno == EAGAIN)
				{
					continue;
				}
				perror("accept fail");
				continue;
			}

			pthread_mutex_lock(&connection_mutex);
			connection_count++;
			pthread_mutex_unlock(&connection_mutex);

			pci = malloc(sizeof(client_info_t));

			if (NULL == pci)
			{
				perror("malloc fail");
				close(sock_conn);
				pthread_mutex_lock(&connection_mutex);
				connection_count--;
				pthread_mutex_unlock(&connection_mutex);
				continue;
			}

			pci->sock_conn = sock_conn;
			strncpy(pci->ip, inet_ntoa(client_addr.sin_addr), sizeof(pci->ip) - 1);
			pci->ip[sizeof(pci->ip) - 1] = '\0';
			pci->port = ntohs(client_addr.sin_port);
			pci->online_time = time(NULL);
			pci->send_file_list = argv + 1;
			pci->send_file_cnt = argc - 1;

			printf("Client connected: %s:%hu\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
			if (pthread_create(&tid, NULL, comm_thr, pci))
			{
				perror("pthread_create fail");

				free(pci);
				close(sock_conn);
				pthread_mutex_lock(&connection_mutex);
				connection_count--;
				pthread_mutex_unlock(&connection_mutex);
				continue;
			}

			struct timeval recv_tv;
			recv_tv.tv_sec = 10;
			recv_tv.tv_usec = 0;
			setsockopt(sock_conn, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
		}
	}

	close(sock_listen);
	printf("\nServer shutting down...\n");

	return 0;
}

void *comm_thr(void *arg)
{
	client_info_t *pci = (client_info_t *)arg;
	int i, err_code = 0;

	pthread_detach(pthread_self());

	printf("\n客户端(%s:%hu)上线，待发送 %d 个文件\n", pci->ip, pci->port, pci->send_file_cnt);

	// 先发送文件数量
	uint32_t file_count = htonl(pci->send_file_cnt);
	if (send_all(pci->sock_conn, &file_count, sizeof(file_count)) < 0)
	{
		printf("发送文件数量失败\n");
		close(pci->sock_conn);
		pthread_mutex_lock(&connection_mutex);
		connection_count--;
		pthread_mutex_unlock(&connection_mutex);
		free(pci);
		return NULL;
	}

	for (i = 0; i < pci->send_file_cnt; i++)
	{
		if ((err_code = send_file(pci->sock_conn, pci->send_file_list[i])) != 0)
		{
			printf("\n向客户端(%s:%hu)发送 %s 文件失败(Error code: %d)！\n", pci->ip, pci->port, pci->send_file_list[i], err_code);
			break;
		}
		else
		{
			printf("\n向客户端(%s:%hu)发送 %s 文件成功！\n", pci->ip, pci->port, pci->send_file_list[i]);
		}
	}

	close(pci->sock_conn);

	pthread_mutex_lock(&connection_mutex);
	connection_count--;
	pthread_mutex_unlock(&connection_mutex);

	printf("\n客户端(%s:%hu)下线！\n", pci->ip, pci->port);

	free(pci);

	return NULL;
}

int recv_all(int sockfd, void *buf, size_t len)
{
	size_t total = 0;
	ssize_t n;
	char *ptr = (char *)buf;
	while (total < len)
	{
		n = recv(sockfd, ptr + total, len - total, 0);
		if (n < 0)
		{
			if (errno == EWOULDBLOCK || errno == EAGAIN)
			{
				usleep(10000);
				continue;
			}
			return -1;
		}
		if (n == 0)
		{
			return -1;
		}
		total += n;
	}
	return 0;
}

int send_file(int sock, const char *file_path)
{
	file_info_t fi = {0};
	const char *file_name = NULL;
	struct stat st;
	int fd, ret;
	char buff[1024];
	uint64_t send_cnt = 0;
	uint64_t offset = 0;

	if (lstat(file_path, &st) == -1)
	{
		perror("lstat fail");
		return 1;
	}

	fi.mode = st.st_mode;
	fi.size = st.st_size;

	file_name = strrchr(file_path, '/');

	if (file_name == NULL)
		file_name = file_path;
	else
		file_name++;

	strncpy(fi.name, file_name, sizeof(fi.name) - 1);

	if (send_all(sock, &fi, sizeof(fi)) < 0)
	{
		fprintf(stderr, "send file attribute fail: %s\n", strerror(errno));
		return 2;
	}

	resume_request_t req;
	if (recv_all(sock, &req, sizeof(req)) < 0)
	{
		fprintf(stderr, "recv resume request fail: %s\n", strerror(errno));
		return 3;
	}

	offset = req.offset;

	fd = open(file_path, O_RDONLY);

	if (fd == -1)
	{
		perror("open fail");
		return 4;
	}

	if (offset > 0)
	{
		if (lseek(fd, offset, SEEK_SET) < 0)
		{
			perror("lseek fail");
			close(fd);
			return 5;
		}
	}

	uint64_t total_to_send = fi.size - offset;
	printf("Sending: %s (%llu bytes)\n", file_name, (unsigned long long)total_to_send);

	const char *spinners = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";
	int spin_idx = 0;

	while ((ret = read(fd, buff, sizeof(buff))) > 0)
	{
		if (send_all(sock, buff, ret) < 0)
		{
			fprintf(stderr, "send_all failed: %s\n", strerror(errno));
			break;
		}
		send_cnt += ret;

		if (total_to_send > 0)
		{
			int progress = (int)(send_cnt * 100 / total_to_send);
			int filled = progress / 5;
			printf("\r  %c ", spinners[spin_idx % 10]);
			printf("[");
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
				   (unsigned long long)send_cnt,
				   (unsigned long long)total_to_send);
			fflush(stdout);
			spin_idx++;
		}
	}
	printf("\n");

	if (ret < 0)
	{
		perror("read fail");
	}

	close(fd);

	if (send_cnt != total_to_send)
	{
		fprintf(stderr, "send file data fail: sent %llu, expected %llu\n",
				(unsigned long long)send_cnt, (unsigned long long)total_to_send);
		return 6;
	}

	return 0;
}
