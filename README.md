[toc]

# async-server
asyn-server是后台通信的**多进程**底层框架，基于**epoll轮询**实现请求的异步处理，由于是**非阻塞网络io**,并发量每秒可达20000+。   
同时在主进程中通过**信号机制**捕获各子进程的处理异常，并进行相应的重启，以防止服务上线后异常当机卡死。  
conn进程和work进程之间的通信通过**管道pipe**和**共享内存**实现，具体表现见conn和work进程分析。
- main   （读取配置，初始化，开启日志文件，实现平滑重启）
产生一个主进程，主进程执行一系列的工作后会产生一个或者多个工作进程；
- conn  （建立连接和数据包收发）
- work （请求包的业务逻辑处理）
![网络通信图](https://github.com/masterGGG/async_server/raw/master/async_server-IO.PNG)

# Question
## 客户端连接和网络框架建立连接的过程
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
## 同一客户的多个请求怎么处理。(协商过协议包头格式，按包长接受数据包处理)
```
typedef struct {
    uint32_t pkg_len;       //数据包长度
    uint32_t seq_num;       //序列号
    uint16_t cmd_id;        //协议号
    uint32_t status_code;   //状态吗
    uint32_t user_id;       //用户账号
} __attribute__((packed)) proto_header;
```
## 可以优化的地方（conn进程挂掉会怎样）
### 考虑可以去掉共享内存和conn进程（同nginx）
![Markdown](http://i2.bvimg.com/683123/b0250618e3ae5541.png)
### 在主进程中负责绑定端口和listen操作，然后再fork出多个woker进程，这样每个work进程都可以去accept这个socket。当一个client连接到来时，所有accept的work进程都会受到通知，但只有一个进程可以accept成功，其它的则会accept失败。
Nginx提供了一把共享锁accept_mutex来保证同一时刻只有一个work进程在accept连接，从而解决惊群问题。（待网络的连接事件，当这个事件发生时，这些进程被同时唤醒，就是“惊群”。）。调用ngx_shmtx_trylock来尝试获取ngx_accept_mutex锁，如果获取了的话，在判断在上次循环中是否已经获取了锁，如果获取了，那么listening就已经在当前worker进程的epoll当中了，否则的话就调用ngx_enable_accept_events函数来讲listening加入到epoll当中，并要对变量ngx_accept_mutex_held赋值，表示已经获取了锁。如果没有获取到锁的话，还要判断上次是否已经获取了锁，如果上次获取了的话，那么还要调用ngx_disable_accept_events函数，将listening从epoll当中移除。
