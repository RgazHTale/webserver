#define _XOPEN_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "threadpool.h"
#include "locker.h"
#include <signal.h>
#include "http_conn.h"

#define MAX_FD 65535           // 最大文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 监听的最大的事件数量

// 添加信号捕捉
void addsig(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// 添加文件描述符到epoll中
extern int addfd(int epollfd, int fd, bool one_shot);
// 从epoll中删除文件描述符
extern int removefd(int epollfd, int fd);
// 修改文件描述符
extern int modfd(int epollfd, int fd, int ev);

int main(int argc, char *argv[])
{
    if (argc <= 1)
    {
        printf("按照如下格式运行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    int port = atoi(argv[1]);

    // 对SIGPIPE信号进行处理
    addsig(SIGPIPE, SIG_IGN);

    // 创建线程池，初始化线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch (...)
    {
        exit(-1);
    }

    // 创建一个数组用于保存所有客户端信息
    http_conn *users = new http_conn[MAX_FD];

    // TCP协议流程
    // 1.创建套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd == -1)
    {
        perror("socket");
        exit(-1);
    }

    // 设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 2.绑定
    struct sockaddr_in address;
    address.sin_family = PF_INET;         // 地址族
    address.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0
    address.sin_port = htons(port);
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    if (ret == -1)
    {
        perror("bind");
        exit(-1);
    }

    // 3.监听
    ret = listen(listenfd, 5);
    if (ret == -1)
    {
        perror("listen");
        exit(-1);
    }

    // IO多路复用
    // 采用epoll技术

    // 创建epoll对象
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);

    // 将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);

    http_conn::m_epollfd = epollfd;

    while (true)
    {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((num < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < num; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                // 有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlen);
                if (connfd == -1)
                {
                    perror("accept");
                    exit(-1);
                }

                if (http_conn::m_user_count >= MAX_FD)
                {
                    // 目前连接数满了
                    // 给客户端回写一个信息：服务器内部正忙
                    close(connfd);
                    continue;
                }

                // 将新的客户的数据初始化，放到数组中
                users[connfd].init(connfd, client_address);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 对方异常断开或错误等事件发生了
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN)
            {
                if (users[sockfd].read())
                {
                    // 此时利用read一次性把所有数据都读完
                    // 然后把连接添加到线程池中
                    // 处理连接
                    // 这里仅仅是把这些数据全部读到了缓冲区中
                    /*
                    GET / HTTP/1.1
                    Host: www.baidu.com
                    User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:86.0) Gecko/20100101 Firefox/86.0
                    Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,/;q=0.8
                    Accept-Language: zh-CN,zh;q=0.8,zh-TW;q=0.7,zh-HK;q=0.5,en-US;q=0.3,en;q=0.2
                    Accept-Encoding: gzip, deflate, br
                    Connection: keep-alive
                    Cookie: BAIDUID=6729CB682DADC2CF738F533E35162D98:FG=1;
                    BIDUPSID=6729CB682DADC2CFE015A8099199557E; PSTM=1614320692; BD_UPN=13314752;
                    BDORZ=FFFB88E999055A3F8A630C64834BD6D0;
                    __yjs_duid=1_d05d52b14af4a339210722080a668ec21614320694782; BD_HOME=1;
                    H_PS_PSSID=33514_33257_33273_31660_33570_26350;
                    BA_HECTOR=8h2001alag0lag85nk1g3hcm60q
                    Upgrade-Insecure-Requests: 1
                    Cache-Control: max-age=0
                    */
                    // 接下来需要在线程池中做的是
                    // 解析这些数据
                    // 把这些数据打印到shell
                    // 生成响应，回复给客户端
                    pool->append(users + sockfd);
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                if (!users[sockfd].write())
                {
                    // 一次性把所有数据都写完
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;

    return 0;
}