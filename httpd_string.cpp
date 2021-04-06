// c++ 11
#include <list>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <memory>
// linux
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <semaphore.h>
#include <sys/socket.h>

using namespace std;

const int STDIN = 0;
const int STDOUT = 1;
const int STDERR = 2;
const string SERVER_STRING = "Server: wlj's httpd/0.1.0\r\n";
const int WORKER_THREAD_NUM = 12;
const int MAX_EPOLL_EVENT = 3072;

int epollfd = 0;
int listenfd = 0;
pthread_t accept_thread_id;
pthread_t worker_thread_id[WORKER_THREAD_NUM];
pthread_cond_t accept_cond;
pthread_mutex_t accept_mutex;

pthread_mutex_t client_mutex;
pthread_cond_t client_cond;
list<int> clients;

void *accept_request(int client);
void bad_request(int client);
void cat(int client, ifstream& resourse);
void cannot_execute(int client);
void error_die(const string& sc);
void execute_cgi(int client, string& path, string& method, string& query_string);
int get_line(int client, string& buf);
void headers(int client, const string& filename);
void not_found(int client);
void serve_file(int client, string& filename);
bool startup(const char* port);
void unimplemented(int client);
void clear_buffer(int client);
void send_data(int client, const string& buf);

/**
 * 处理来自客户端的请求
 * @param {void*} arg
 * @return {*}
 */
void *accept_request(int client)
{
    // cout << "Entering accept_request" << endl;
    string method;
    string url;
    string path;
    struct stat st;
    bool cgi = false;
    string query_string;

    // 获得HTTP请求的请求行
    string buf;
    get_line(client, buf);
    // 使用字符串流处理
    istringstream istrStream(buf);
    istrStream >> method >> url;
    
    // 请求是否为 GET 方法或 POST 方法
    if (method != "GET" && method != "POST")
    {
        unimplemented(client);
        return nullptr;
    }
    // 如果为 POST 方法，设cgi变量为true
    else if (method == "POST")
        cgi = true;
    // 如果为 GET 方法，查找是否有以“?”分割的参数
    else if (method == "GET")
    {
        if (url.find('?') != string::npos)
        {
            query_string += url.substr(url.find('?')+1);
            url.erase(url.find('?'));
            //string 无法使用 '\0' 进行分割
            //url[url.find('?')] = '\0';
        }
    }
    
    // 客户端期望请求的页面路径
    path = "htdocs" + url;
    // 补全默认文件名
    if (path.back() == '/')
        path += "index.html";
    // 如果文件不存在，将目前请求后续的内容全部读完
    if (stat(path.c_str(), &st) == -1)
    {
        clear_buffer(client);
        // 然后返回一个 404 状态代码
        not_found(client);
    }
    // 如果文件存在，判断请求是否为动态或静态
    else
    {
        // 如果url为目录，补全默认文件名
        if ((st.st_mode & S_IFMT) == S_IFDIR)
            path += "/index.html";
        // 如果url为可执行文件，则为动态内容请求
        if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH))
            cgi = true;
        // 静态内容请求
        if (!cgi)
            serve_file(client, path);
        // 动态内容请求，转交给CGI程序
        else
            execute_cgi(client, path, method, query_string);
    }

    //只实现为短连接请求，处理完事务后直接关闭
    // close(client);
    return nullptr;
}

/**
 * 如果请求有问题，返回400响应状态码
 * @param {int} client
 * @return {*}
 */
void bad_request(int client)
{
	send_data(client, "HTTP/1.0 400 BAD REQUEST\r\n");
	send_data(client, "Content-type: text/html\r\n");
	send_data(client, "\r\n");
	send_data(client, "<P>Your browser sent a bad request, ");
	send_data(client, "such as a POST without a Content-Length.\r\n");
}

/**
 * 读取文件的全部内容并发送至客户端
 * @param {int} client
 * @param {ifstream} &resourse
 * @return {*}
 */
void cat(int client, ifstream &resourse)
{
    // cout << "Entering cat" << endl;
    char buf[128];
    while (!resourse.eof())
    {
        resourse.getline(buf, sizeof(buf));
        send(client, buf, strlen(buf), 0);
    }
}

/**
 * 通知客户端其所要求的CGI程序无法被执行
 * 返回 500 状态代码
 * @param {int} client
 * @return {*}
 */
void cannot_execute(int client)
{
	send_data(client, "HTTP/1.0 500 Internal Server Error\r\n");
	send_data(client, "Content-type: text/html\r\n");
	send_data(client, "\r\n");
	send_data(client, "<P>Error prohibited CGI execution.\r\n");
}

/**
 * 通过perror函数输出错误信息，并结束程序执行
 * @param {const string&} sc
 * @return {*}
 */
void error_die(const string& sc)
{
    // cout << "Entering error_die" << endl;
    perror(sc.c_str());
    exit(1);
}

/**
 * 执行一个CGI程序，根据请求的类型设定相关的环境变量
 * @param {int} client
 * @param {string&} path
 * @param {string&} method
 * @param {string} query_string
 * @return {*}
 */
void execute_cgi(int client, string& path, string& method, string& query_string)
{
    // cout << "Entering execute_cgi" << endl;
    auto sendData = [client](const char *buf){
        send(client, buf, strlen(buf), 0);
    };

    int cgi_output[2];
    int cgi_input[2];
    pid_t pid;
    int content_length = -1;
    
    // 如果请求为 GET ，将目前已有的数据全部读取并丢弃
    if (method == "GET")
    {
        clear_buffer(client);
    }
    // 如果请求为 POST，获取请求报文中的 Content-Length 参数
    else if (method == "POST")
    { 
        string buf;
        get_line(client, buf);
        while (!buf.empty())
        {
            if (buf.find("Content-Length:") != string::npos)
                content_length = stoi(buf.substr(15));
            get_line(client, buf);
        }
        if (content_length == -1)
        {
            bad_request(client);
            return;
        }
    }

    // 建立与子进程通信的管道
    if (pipe(cgi_output) < 0)
    {
        cannot_execute(client);
        return;
    }
    if (pipe(cgi_input) < 0)
    {
        cannot_execute(client);
        return;
    }

    // 如果建立进程失败，向客户端返回错误信息
    if ((pid = fork()) < 0)
    {
        cannot_execute(client);
        return;
    }
    sendData("HTTP/1.0 200 OK\r\n");
    // 子进程
    if (pid == 0)
    {
        

        // 由于 CGI 程序的输入输出是使用标准输入输出流的，所以需要重定向到与客户端相关联的已连接描述符
        // 将标准输入输出流进行重定向
        // 与父进程建立的匿名管道重绑定
        dup2(cgi_output[1], STDOUT);
        dup2(cgi_input[0], STDIN);
        // 关闭输出管道的读取端
        close(cgi_output[0]);
        // 关闭输入管道的写入端
        close(cgi_input[1]);
        // 设定相关的环境变量
        method = "REQUEST_METHOD=" + method;
        putenv(const_cast<char*>(method.c_str()));
        if (method == "GET") {
            query_string = "QUERY_STRING=" + query_string;
            putenv(const_cast<char*>(query_string.c_str()));
        }
        else {
            string length_env = "CONTENT_LENGTH=" + to_string(content_length);
            putenv(const_cast<char*>(length_env.c_str()));
        }
        // 执行 CGI 程序
        char *emptylist[] = {nullptr};
        execve(path.c_str(), emptylist, environ);
        exit(0);
    }
    // 父进程
    else
    {
        // 关闭输出管道的写入端
        close(cgi_output[1]);
        // 关闭输入管道的读取端
        close(cgi_input[0]);
        // 将客户端的请求发送给 CGI 程序
        if (method == "POST")
        {
            for (int i=0; i< content_length; i++)
            {
                char c;
                recv(client, &c, 1, 0);
                write(cgi_input[1], &c, 1);
            }
        }
        // 将 CGI 程序的回应发送给客户端
        char c;
        while (read(cgi_output[0], &c, 1) > 0)
            send(client, &c, 1, 0);

        // 关闭管道
        close(cgi_output[0]);
        close(cgi_input[1]);
        // 回收子进程
        waitpid(pid, nullptr, 0);
    }
}


/**
 * 从描述符获得一行字符串
 * @param {int} client
 * @return {string}
 */
int get_line(int client, string& buf)
{
    buf.clear();
    char c = '\0';
    while(recv(client, &c, 1, 0) > 0 && c != '\n')
    {
        if(c == '\r')
        {
            if(recv(client, &c, 1, MSG_PEEK) > 0 && c == '\n')
            {
                recv(client, &c, 1, 0);
                break;
            }
        }
        else
        {
            buf.push_back(c);
        }
    }
    return buf.length();
}

/**
 * 输出服务器报头
 * @param {int} client
 * @param {const string&} filename
 * @return {*}
 */
void headers(int client, const string& filename)
{
	send_data(client, "HTTP/1.0 200 OK\r\n");
	send_data(client, SERVER_STRING);	
	send_data(client, "Content-Type: text/html\r\n");	
	send_data(client, "\r\n");
}

/**
 * 要求的页面不存在，返回 404 状态代码
 * @param {int} client
 * @return {*}
 */
void not_found(int client)
{
	send_data(client, "HTTP/1.0 404 NOT FOUND\r\n");
	send_data(client, SERVER_STRING);
	send_data(client, "Content-Type: text/html\r\n");
	send_data(client, "\r\n");
	send_data(client, "<HTML><TITLE>Not Found</TITLE>\r\n");
	send_data(client, "<BODY><P>The server could not fulfill\r\n");
	send_data(client, "your request because the resource specified\r\n");
	send_data(client, "is unavailable or nonexistent.\r\n");
	send_data(client, "</BODY></HTML>\r\n");
}

/**
 * 请求的是静态内容，读取文件并发送至客户端
 * @param {int} client
 * @param {string&} filename
 * @return {*}
 */
void serve_file(int client, string& filename)
{
    // cout << "Entering serve_file" << endl;
    ifstream resource;

    // 将客户端发送的剩余内容读取并丢弃
    // int numchars = get_line(client, buf);
    // while (numchars > 1 && buf.back() != '\0')
    // {
    //     cout << "numchars:" << numchars << " buf.length:" << buf.length() << endl;
    //     numchars = get_line(client, buf);
    // }
    clear_buffer(client);
    // 判断文件是否能打开
    resource.open(filename);
    if (!resource.is_open())
        not_found(client);
    else
    {
        // 如果可以，发送服务器报头以及html文件
        headers(client, filename);
        cat(client, resource);
    }
    resource.close();

}

/**
 * 进行套接字的初始化
 * @param {const char*} port
 * @return int
 */
bool startup(const char* port1)
{
    // 使用可重入的、与协议无关的流程建立监听套接字
    struct addrinfo hints, *listp, *p;
    int optval = 1;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;    //接受来自任何IP地址的请求
    hints.ai_flags = AI_NUMERICSERV;                //使用端口号，而不是服务号
    getaddrinfo(nullptr, port1, &hints, &listp);

    // 使用ipv4而不是ipv6
    p = listp;
    while (p->ai_next != nullptr && p->ai_family == PF_INET6)
        p = p->ai_next;
    if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
        error_die("socket");
    
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, (socklen_t)sizeof(int))) < 0)
        error_die("setsockopt reuseaddr failed");
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, (const void *)&optval, (socklen_t)sizeof(int))) < 0)
        error_die("setsockopt reuseport failed");

    if (bind(listenfd, p->ai_addr, p->ai_addrlen) < 0)
        error_die("setsockopt failed");
    
    // 使用getaddrinfo方法后，需要手动释放返回的链表
    freeaddrinfo(listp);
    
    if (listen(listenfd, 1024) < 0)
    {
        close(listenfd);
        error_die("listen");
    }

    epollfd = epoll_create(MAX_EPOLL_EVENT);
    epoll_event ev;
    ev.data.fd = listenfd;
    ev.events = EPOLLIN;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev);

    return true;
}

/**
 * 客户端请求的操作还未实现，返回 501 状态代码
 * @param {int} client
 * @return {*}
 */
void unimplemented(int client)
{
	send_data(client, "HTTP/1.0 501 Method Not Implemented\r\n");
    send_data(client, SERVER_STRING);
	send_data(client, "Content-Type: text/html\r\n");
	send_data(client, "\r\n");
	send_data(client, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
	send_data(client, "</TITLE></HEAD>\r\n");
	send_data(client, "<BODY><P>HTTP request method not supported.\r\n");
	send_data(client, "</BODY></HTML>\r\n");
};

void clear_buffer(int client)
{
    string s = "A";
    while(!s.empty())
        get_line(client, s);
}

void send_data(int client, const string& buf)
{
    send(client, buf.c_str(), buf.length(), 0);
}

/**
 * 将描述符设置为非阻塞模式
 * 返回旧的设置
 * @param {int} fd
 * @return {int}
 */
int set_nonblock(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

void release_client(int client)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, client, nullptr);
    close(client);
}

void* accept_thread(void *arg)
{
    while (1)
    {
        pthread_mutex_lock(&accept_mutex);
        pthread_cond_wait(&accept_cond, &accept_mutex);

        sockaddr_in clientaddr;
        socklen_t addrlen = sizeof(clientaddr);
        int clientfd = accept4(listenfd, (sockaddr*)&clientaddr, &addrlen, SOCK_NONBLOCK);

        pthread_mutex_unlock(&accept_mutex);

        set_nonblock(clientfd);
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = clientfd;
        epoll_ctl(epollfd, EPOLL_CTL_ADD, clientfd, &ev);
    }
    return nullptr;
}

void* worker_thread(void* arg)
{
    while (1)
    {
        pthread_mutex_lock(&client_mutex);
        while (clients.empty())
            pthread_cond_wait(&client_cond, &client_mutex);
        int clientfd = clients.front();
        clients.pop_front();
        pthread_mutex_unlock(&client_mutex);

        accept_request(clientfd);
        // cout << "accept: " << clientfd << endl;
        release_client(clientfd);
    }
    return nullptr;
}

int main(int argv, char *argc[])
{
    char port[] = "51213";

    // 信号处理
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);    
    
    // 信号量、互斥锁初始化
    pthread_cond_init(&accept_cond, nullptr);
    pthread_cond_init(&client_cond, nullptr);

    pthread_mutex_init(&accept_mutex, nullptr);
    pthread_mutex_init(&client_mutex, nullptr);

    // socket、epoll初始化
    startup(port);
    cout << "httpd running on port " << port << endl;

    pthread_create(&accept_thread_id, nullptr, accept_thread, nullptr);
    for (int i=0; i<WORKER_THREAD_NUM; i++)
    {
        pthread_create(&worker_thread_id[i], nullptr, worker_thread, nullptr);
    }

    while (1)
    {
        epoll_event ev[MAX_EPOLL_EVENT];
        int n = epoll_wait(epollfd, ev, MAX_EPOLL_EVENT, 10);
        n = min(n, MAX_EPOLL_EVENT);
        for (int i=0; i<n; i++)
        {
            if (ev[i].data.fd == listenfd)
                pthread_cond_signal(&accept_cond);
            else
            {
                pthread_mutex_lock(&client_mutex);
                clients.push_back(ev[i].data.fd);
                pthread_mutex_unlock(&client_mutex);

                pthread_cond_signal(&client_cond);
            }
        }
    }

    return 0;
}
