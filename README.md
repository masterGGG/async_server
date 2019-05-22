[toc]

# async-server
后台通信网络框架，借鉴了reactor模式，并基于**epoll事件处理机制**实现请求的异步处理，并发量每秒可达20000+。   
```
graph TD
    subgraph 数据流通
        main-->|创建连接进程|conn
        main-->|创建工作进程|work

        conn-->|请求转发|work
        work-->|业务处理|conn
    end
```
在主进程中通过**信号机制**捕获各子进程的处理异常，并进行相应的重启，以防止服务上线后异常宕机导致服务不可用。  
conn进程和work进程之间的通信通过**管道pipe**和**mmap共享内存**实现，具体表现见conn和work进程分析。

# 架构
async-server中包含了3类进程， 2类reactor以及5种handler。
## 3类进程
- 主进程（负责初始化，以及对子进程的创建和管理）
- 连接进程（负责连接的建立，请求的收发）
- 工作进程（负责业务逻辑的处理）
## 2类reactor
每个子进程中都会保持一个reactor实例。其中分为**连接进程reactor**和**工作进程reactor**。
- 连接进程reactor（包含连接handler， 回复handler，套接字handler）
- 工作进程reactor（包含业务处理handler，后台服务handler）

## 5类handler
### 连接handler（专门处理连接事件，并创建接收handler实例，添加到reactor）
```
class c_tcp_accept : public c_handler
{
...
public:
    bool start(const char *ip, uint16_t port)  
    /*{ 
        listen(m_fd, 1024)      //监听
    }*/

public:
    virtual bool handle_input();
    /*{
        cli_fd = accept(m_fd, (sockaddr *)&ip, &len);  //建立连接
        c_tcp_socket *ts = new (std::nothrow) c_tcp_socket();
        ts->start(cli_fd, ip);
    }*/
    ....

private:
    int m_fd;
};
```
### 套接字handler（接收客户端数据，写入请求队列，通知业务handler消费）
```
class c_tcp_socket : public c_handler
{
public:
    void start(int fd, sockaddr_in &ip);
    /*{
         //在epoll中加入对应的连接描述符上的监听事件
        if (!g_reactor.add_handler(m_fd, this)) 
        ...
        ////通知每个工作进程有新的连接建立
        for (uint32_t i = 0; i < g_shm_queue_mgr.channel_num; ++i) // notify all.
            recv_push(i, &sb, (uint8_t *)(&m_ip.sin_addr.s_addr), true);
    }*/
public:
    virtual bool handle_input();
    virtual bool handle_output();
    virtual void send_pkg(const void * buf, uint32_t len);

private:
    int m_fd;
    int m_id;
    sockaddr_in m_ip;

    bool m_is_closed;
    uint32_t m_recv_pos;
    uint8_t *m_recv_buf;
    uint32_t m_recv_buf_size;

    uint32_t m_send_pos;
    std::list<buffer_t *> m_send_list;
    uint32_t m_send_list_size;
};
```
### 业务处理handler（消费请求队列数据，结果写入应答队列，通知回复handler消费）
```
class c_revent : public c_handler
{
...
public:
    bool start(uint32_t channel);

public:
    virtual bool handle_input();
    /*if (sb.type == PROTO_BLOCK) {   //请求包：业务逻辑处理
        g_link_flags[sb.fd] = sb.id;
        if (g_link_flags[sb.fd] != 0)
            g_plugin.proc_pkg_cli(sb.fd, (char *)buf, sb.len);
    } else if (sb.type == CLOSE_BLOCK) {    //断开连接包
        if (g_link_flags[sb.fd] != 0) {
            if (g_plugin.link_down_cli)
                g_plugin.link_down_cli(sb.fd);
            g_link_flags[sb.fd] = 0;
        }
    } else if (sb.type == LOGIN_BLOCK) {        //建立连接包处理
        g_link_flags[sb.fd] = sb.id;
        if (g_plugin.link_up_cli)
            g_plugin.link_up_cli(sb.fd, *(uint32_t *)buf);
    }*/
    virtual bool handle_output();

private:
    int m_fd;
    uint32_t m_channel;
};
```
### 回复handler（消费回复队列的消息，从reactor中找出对应的套接字handler回传应答）
```
class c_sevent : public c_handler
{
...
public:
    bool start(uint32_t channel);
public:
    virtual bool handle_input();
    /*
    c_sevent::handle_input() {     
        ...
        //conn监听的管道收到回复，调用具体连接类做回应
        handler->send_pkg(buf, sb.len);
    }
    c_tcp_linker::send_pkg(const void * buf, uint32_t len) {
        if (!m_send_list.empty()) {
            //发送队列非空，则需要将应答包加入发送队列
        } else 
            int n = send(m_fd, buf, len, 0);  //否则，直接回应给相应的客户端
    }
    */
    virtual bool handle_output();

private:
    int m_fd;
    uint32_t m_channel;
};

```
### 后台服务handler（业务handler如需要请求其他服务的数据，可创建后台handler，用于与后台服务通信）

## 模块关系图
![网络通信图](https://github.com/masterGGG/async_server/raw/master/async_server-IO.PNG)

# Question
## 同一客户的多个请求怎么处理。(二进制数据报，协定包头格式，按包长接受数据包处理)
- 在**请求接收handler**的实例中有一块内存用于缓存接收到的数据，每次接收之后会检查内存中的数据是否有完整的包，有就写入请求队列，并通知**业务处理handler**进行相应的处理。
- 数据包分流这边可以自定义策略，这一块默认是用的轮询的方式分发数据包。  

**协议包头**
```
typedef struct {
    uint32_t pkg_len;       //数据包长度
    uint32_t seq_num;       //序列号
    uint16_t cmd_id;        //协议号
    uint32_t status_code;   //状态吗
    uint32_t user_id;       //用户账号
} __attribute__((packed)) proto_header;
```

## 客户端连接和网络框架建立连接的过程

```
sequenceDiagram
work->>work: Register Receive handler
conn->>conn: Register Send & Accept handler
client->>conn: Request connection
conn->>client: Notify accept handler
conn->>conn: Register Socket handler
client->>conn: Request something
conn->>conn: Notify Socket handler,save data to recv_queue
conn->>work: Notify Receive handler 
work->>work: Handle request,save response to send_queue
work->>conn: Notify Send handler
conn->>conn: Send handler search socket handler
conn->>client: Send response
```
### 端口监听，建立连接
```
c_tcp_accept::start() {
    listen(m_fd, 1024)      //监听  
}
c_tcp_accept::handle_input() {
    cli_fd = accept(m_fd, (sockaddr *)&ip, &len);  //建立连接
    c_tcp_socket *ts = new (std::nothrow) c_tcp_socket();
    ts->start(cli_fd, ip);
}
c_tcp_socket::start(int fd, sockaddr_in &ip) {  
    //在epoll中加入对应的连接描述符上的监听事件
    if (!g_reactor.add_handler(m_fd, this)) {
        handle_fini();
        return;
    }

    if (!g_reactor.handle_ctl(m_fd, EPOLL_CTL_ADD, EPOLLIN)) {
        handle_fini();
        return;
    }
    //通知每个工作进程有新的连接建立
    shm_block_t sb;
    sb.fd = m_fd;
    sb.id = m_id;
    sb.len = 4;
    sb.type = LOGIN_BLOCK;
    for (uint32_t i = 0; i < g_shm_queue_mgr.channel_num; ++i) // notify all.
        recv_push(i, &sb, (uint8_t *)(&m_ip.sin_addr.s_addr), true);
}
```
### 请求封装，通知work
```
c_tcp_socket::handle_input() {
    n = recv(m_fd, m_recv_buf + m_recv_pos, m_recv_buf_size - m_recv_pos, 0);
    shm_block_t sb;
    sb.fd = m_fd;
    sb.id = m_id;
    sb.len = len;
    sb.type = PROTO_BLOCK;
    if (recv_push(channel, &sb, m_recv_buf + pos, false) == 0) {}   //选择工作进程，将封装的请求包通知给worker
}
```
### 业务处理，回应封装
```
c_revent::handle_input() {
    if (sb.type == PROTO_BLOCK) {   //请求包：业务逻辑处理
        g_link_flags[sb.fd] = sb.id;
       // if (g_link_flags[sb.fd] != 0)
            g_plugin.proc_pkg_cli(sb.fd, (char *)buf, sb.len);
    } else if (sb.type == CLOSE_BLOCK) {    //断开连接包
        if (g_link_flags[sb.fd] != 0) {
            if (g_plugin.link_down_cli)
                g_plugin.link_down_cli(sb.fd);

            g_link_flags[sb.fd] = 0;
        }
    } else if (sb.type == LOGIN_BLOCK) {        //建立连接包处理
        g_link_flags[sb.fd] = sb.id;
        if (g_plugin.link_up_cli)
            g_plugin.link_up_cli(sb.fd, *(uint32_t *)buf);
    }
}
int net_send_cli(int fd, const void *buf, int len)   //业务层回调接口，用于将应答封装写回conn队列
{
    if ((uint32_t)fd >= g_max_connect || g_link_flags[fd] == 0 || len <= 0)
        return -1;

    struct shm_block_t sb;
    sb.fd = fd;
    sb.id = g_link_flags[fd];
    sb.len = len;
    sb.type = PROTO_BLOCK;
    send_push(g_work_channel, &sb, (const uint8_t *)buf, true);   //
    return len;
}

```
### 应答包返回
```
c_sevent::handle_input() {      //conn监听的管道收到回复，调用具体连接类做回应
    handler->send_pkg(buf, sb.len);
}
c_tcp_linker::send_pkg(const void * buf, uint32_t len) {
    if (!m_send_list.empty()) {
        //发送队列非空，则需要将应答包加入发送队列
    } else 
        int n = send(m_fd, buf, len, 0);  //否则，直接回应给相应的客户端
}
```
## work进程的工作方式（同上）
```
```

## 可以优化的地方（conn进程挂掉会怎样）
### 考虑可以去掉共享内存和conn进程（同nginx）

#### 在主进程中负责绑定端口和listen操作，然后再fork出多个woker进程，这样每个work进程都可以去accept这个socket。当一个client连接到来时，所有accept的work进程都会受到通知，但只有一个进程可以accept成功，其它的则会accept失败。
Nginx提供了一把共享锁accept_mutex来保证同一时刻只有一个work进程在accept连接，从而解决惊群问题。（待网络的连接事件，当这个事件发生时，这些进程被同时唤醒，就是“惊群”。）。调用ngx_shmtx_trylock来尝试获取ngx_accept_mutex锁，如果获取了的话，在判断在上次循环中是否已经获取了锁，如果获取了，那么listening就已经在当前worker进程的epoll当中了，否则的话就调用ngx_enable_accept_events函数来讲listening加入到epoll当中，并要对变量ngx_accept_mutex_held赋值，表示已经获取了锁。如果没有获取到锁的话，还要判断上次是否已经获取了锁，如果上次获取了的话，那么还要调用ngx_disable_accept_events函数，将listening从epoll当中移除。