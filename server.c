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



#pragma pack(push, 1)


typedef struct 
{
	char name[101];  // 文件名最大长度不超过 100 字节
	uint32_t mode;   // 文件模式
	uint64_t size;   // 文件大小
	// ......        // 更多文件属性可以扩展	

} file_info_t;


#pragma pack(pop)



typedef struct
{
	int sock_conn;
	char ip[16];
	unsigned short port;
	time_t online_time;     // 上线时间
	char** send_file_list;  // 待发送的文件路径列表
	int send_file_cnt;      // 待发送的文件数量
	// char user_name[50];  // 用户名
	//......

} client_info_t;



// 确保数据完整发送
int send_all(int sockfd, const void *buf, size_t len) {
    size_t total = 0;
    ssize_t n;
    const char *ptr = (const char *)buf;
    while (total < len) {
        n = write(sockfd, ptr + total, len - total);
        if (n <= 0) {
            return -1;
        }
        total += n;
    }
    return 0;
}

// 全局变量
#define MAX_CONNECTIONS 100  // 最大并发连接数
int connection_count = 0;          // 当前连接数
pthread_mutex_t connection_mutex = PTHREAD_MUTEX_INITIALIZER;  // 连接数互斥锁

void* comm_thr(void* arg);
int send_file(int sock, const char* file_path);



int main(int argc, char** argv)
{
	signal(SIGPIPE, SIG_IGN);

	// 校验参数
	if(argc == 1)
	{
		fprintf(stderr, "Usage: %s file1 [...]\n", argv[0]);
		return 1;
	}

	for(int i = 1; i < argc; i++)
	{
		// 检查文件是否存在且可读
		if(-1 == access(argv[i], R_OK))
		{
			fprintf(stderr, "文件 %s 不存在或不可读！\n", argv[i]);
			return 1;
		}

		// 检查文件是否为普通文件（不是目录或其他特殊文件）
		struct stat st;
		if(lstat(argv[i], &st) == -1)
		{
			perror("lstat fail");
			return 1;
		}

		if(!S_ISREG(st.st_mode))
		{
			fprintf(stderr, "%s 不是普通文件！\n", argv[i]);
			return 1;
		}

		// 检查文件路径长度是否合理
		if(strlen(argv[i]) >= PATH_MAX)
		{
			fprintf(stderr, "文件路径 %s 过长！\n", argv[i]);
			return 1;
		}

		// 检查文件路径是否包含路径遍历攻击字符
		if(strstr(argv[i], "../") != NULL || strstr(argv[i], "..\\") != NULL)
		{
			fprintf(stderr, "文件路径 %s 包含非法字符！\n", argv[i]);
			return 1;
		}
	}

	// 第 1 步：创建监听套接字
	int sock_listen = socket(AF_INET, SOCK_STREAM, 0);

	if(-1 == sock_listen)
	{
		perror("socket fail");
		exit(1);
	}


	// 开启地址复用，以允许服务器快速重启
	int val = 1;
	setsockopt(sock_listen, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));


	// 第 2 步：绑定地址

	// 指定地址
	struct sockaddr_in myaddr;
	myaddr.sin_family = AF_INET;          // 指定地址家族(AF)为 Internet 地址家族
	myaddr.sin_addr.s_addr = INADDR_ANY;  // 指定 IP 地址为本机任意地址
	//myaddr.sin_addr.s_addr = inet_addr("172.16.251.96");  // 指定 IP 地址为本机的某个具体 IP 地址
	myaddr.sin_port = htons(9413);        // 指定端口号为 9413
	
	//printf("%hu\n", htons(6666));  // 2586

	// 绑定
	if(-1 == bind(sock_listen, (struct sockaddr*)&myaddr, sizeof(myaddr)))
	{
		perror("bind fail");
		exit(1);
	}


	// 第 3 步：监听
	if(-1 == listen(sock_listen, 5))
	{
		perror("listen fail");
		exit(1);
	}

	// 设置监听套接字为非阻塞模式
	int flags = fcntl(sock_listen, F_GETFL, 0);
	if(flags == -1)
	{
		perror("fcntl F_GETFL fail");
		exit(1);
	}
	if(fcntl(sock_listen, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		perror("fcntl F_SETFL fail");
		exit(1);
	}

	// 第 4 步：接收客户端连接请求
	
	int sock_conn;
	pthread_t tid;
	client_info_t* pci = NULL;
	struct sockaddr_in client_addr;
	socklen_t addr_len = sizeof(client_addr);

	struct timeval tv;
	tv.tv_sec = 10;
	tv.tv_usec = 0;

	fd_set read_set;
	int max_fd;


	printf("Server started, listening on port 9413\n");
	while(1)
	{
		printf("\nMain loop iteration\n");
		// 检查连接数是否达到上限
		pthread_mutex_lock(&connection_mutex);
		printf("Current connection count: %d\n", connection_count);
		if(connection_count >= MAX_CONNECTIONS)
		{
			pthread_mutex_unlock(&connection_mutex);
			printf("Connection limit reached. Waiting...\n");
			sleep(1);
			continue;
		}
		pthread_mutex_unlock(&connection_mutex);

		// 使用 select 实现 accept 超时
		FD_ZERO(&read_set);
		FD_SET(sock_listen, &read_set);
		max_fd = sock_listen + 1;

		tv.tv_sec = 5; // 5秒超时
		tv.tv_usec = 0;

		printf("Calling select()...\n");
		int ret = select(max_fd, &read_set, NULL, NULL, &tv);
		if(ret < 0)
		{
			perror("select fail");
			continue;
		}
		else if(ret == 0)
		{
			// 超时，继续循环
			printf("Select timeout\n");
			continue;
		}

		// 检查是否有连接请求
		if(FD_ISSET(sock_listen, &read_set))
		{
			printf("Connection request received\n");
			sock_conn = accept(sock_listen, (struct sockaddr*)&client_addr, &addr_len);

			if(-1 == sock_conn)
			{
				if(errno == EWOULDBLOCK || errno == EAGAIN)
				{
					// 非阻塞模式下没有连接请求，继续循环
					printf("EWOULDBLOCK or EAGAIN\n");
					continue;
				}
				perror("accept fail");
				continue;
			}

			printf("Connection accepted, socket: %d\n", sock_conn);
			// 增加连接数
			pthread_mutex_lock(&connection_mutex);
			connection_count++;
			pthread_mutex_unlock(&connection_mutex);

			pci = malloc(sizeof(client_info_t));

			if(NULL == pci)
			{
				perror("malloc fail");
				close(sock_conn);
				// 减少连接数
				pthread_mutex_lock(&connection_mutex);
				connection_count--;
				pthread_mutex_unlock(&connection_mutex);
				continue;
			}

			// 获取当前上线的客户端信息
			pci->sock_conn = sock_conn;
			strncpy(pci->ip, inet_ntoa(client_addr.sin_addr), sizeof(pci->ip) - 1);
			pci->ip[sizeof(pci->ip) - 1] = '\0'; // 确保字符串结束
			pci->port = ntohs(client_addr.sin_port);
			pci->online_time = time(NULL);
			pci->send_file_list = argv + 1;
			pci->send_file_cnt = argc - 1;
			
			printf("Creating thread for client %s:%hu\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
			if(pthread_create(&tid, NULL, comm_thr, pci))
			{
				perror("pthread_create fail");

				free(pci);
				close(sock_conn);
				// 减少连接数
				pthread_mutex_lock(&connection_mutex);
				connection_count--;
				pthread_mutex_unlock(&connection_mutex);
				continue;
			}
			printf("Thread created successfully for client %s:%hu\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

			// 设置接收超时
			setsockopt(sock_conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		}
	}

	// 第 7 步：关闭监听套接字
	close(sock_listen);

	return 0;
}



// 定义通信线程函数
void* comm_thr(void* arg)
{
	client_info_t* pci = (client_info_t*)arg;
	int i, err_code = 0;

	pthread_detach(pthread_self());

	printf("\n通信线程启动，处理客户端(%s:%hu)\n", pci->ip, pci->port);
	printf("待发送文件数量: %d\n", pci->send_file_cnt);
	for(i = 0; i < pci->send_file_cnt; i++)
	{
		printf("待发送文件[%d]: %s\n", i, pci->send_file_list[i]);
	}

	printf("\n客户端(%s:%hu)上线...\n", pci->ip, pci->port);

	// 第 5 步：收发数据
	for(i = 0; i < pci->send_file_cnt; i++)
	{
		if((err_code = send_file(pci->sock_conn, pci->send_file_list[i])) != 0)
		{
			printf("\n向客户端(%s:%hu)发送 %s 文件失败(Error code: %d)！\n", pci->ip, pci->port, pci->send_file_list[i], err_code);
			break;
		}
		else
		{
			printf("\n向客户端(%s:%hu)发送 %s 文件成功！\n", pci->ip, pci->port, pci->send_file_list[i]);
		}
	}

	// 第 6 步：断开连接（关闭连接套接字）
	close(pci->sock_conn);

	// 减少连接数
	pthread_mutex_lock(&connection_mutex);
	connection_count--;
	pthread_mutex_unlock(&connection_mutex);

	printf("\n客户端(%s:%hu)下线！\n", pci->ip, pci->port);

	free(pci);

	return NULL;
}



// 将指定文件发送给客户端
int send_file(int sock, const char* file_path)
{
	file_info_t fi = {""};
	const char* file_name = NULL;
	struct stat st;
	int fd, ret;
	char buff[1024];
	uint64_t send_cnt = 0;


	// 获取文件模式和大小
	if(lstat(file_path, &st) == -1)
	{
		perror("lstat fail");
		return 1;
	}

	fi.mode  = st.st_mode;
	fi.size  = st.st_size;

	printf("Sending file: %s, size: %llu bytes\n", file_path, (unsigned long long)fi.size);

	// 获取文件名（不含路径）
	file_name = strrchr(file_path, '/');

	if(file_name == NULL)
		file_name = file_path;
	else
		file_name++;

	strncpy(fi.name, file_name, sizeof(fi.name) - 1);

	// 发送文件属性信息
	if(send_all(sock, &fi, sizeof(fi)) < 0)
	{
		fprintf(stderr, "send file attribute fail: %s\n", strerror(errno));
		return 2;
	}

	// 发送文件数据内容
	printf("Attempting to open file: %s\n", file_path);
	fd = open(file_path, O_RDONLY);

	if(fd == -1)
	{
		perror("open fail");
		return 3;
	}

	printf("File opened successfully, descriptor: %d\n", fd);

	// 尝试读取文件
	ret = read(fd, buff, sizeof(buff));
	if(ret < 0)
	{
		perror("read fail");
	}
	else if(ret == 0)
	{
		printf("Read returned 0 bytes (end of file)\n");
	}
	else
	{
		printf("Read %d bytes from file\n", ret);
		if(send_all(sock, buff, ret) < 0)
		{
			fprintf(stderr, "send_all failed: %s\n", strerror(errno));
		}
		else
		{
			printf("Sent %d bytes to client\n", ret);
			send_cnt += ret;
		}
	}

	// 继续读取剩余数据
	while((ret = read(fd, buff, sizeof(buff))) > 0)
	{
		printf("Read %d bytes from file\n", ret);
		if(send_all(sock, buff, ret) < 0)
		{
			fprintf(stderr, "send_all failed: %s\n", strerror(errno));
			break;
		}
		printf("Sent %d bytes to client\n", ret);
		send_cnt += ret;
	}

	if(ret < 0)
	{
		perror("read fail");
	}
	else
	{
		printf("End of file reached, total read: %llu bytes\n", (unsigned long long)send_cnt);
	}

	if(close(fd) < 0)
	{
		perror("close fail");
	}
	else
	{
		printf("File closed successfully\n");
	}

	if(send_cnt != fi.size)
	{
		fprintf(stderr, "send file data fail: sent %llu, expected %llu\n", (unsigned long long)send_cnt, (unsigned long long)fi.size);
		return 4; 		
	}

	printf("File sent successfully, total: %llu bytes\n", (unsigned long long)send_cnt);
	return 0;  // 发送文件成功
}