#ifndef ASYNC_SERVER_SHMQ_HPP
#define ASYNC_SERVER_SHMQ_HPP

#include <stdint.h>
#include <list>

enum {
    CLOSE_BLOCK = 1,
	LOGIN_BLOCK,
    PROTO_BLOCK
};

/*请求回复队列的每个数据包的结构*/
struct shm_block_t {
    int fd;         //连接套接字
    uint32_t id;
    uint32_t len;
    uint8_t type;
    uint8_t buf[];  //数据流
};

//数据队列的头部结构
struct shm_head_t {
    volatile uint32_t inited;
    volatile uint32_t idle;
    volatile uint32_t w_pos;
    volatile uint32_t r_pos;
};

//数据队列的基础信息
struct shm_queue_t {
    uint8_t *base;              //共享内存的写入起始地址
    uint32_t size;              //共享队列的长度
    struct shm_head_t *head;    //
    uint8_t *pull_buf;
    uint32_t pull_len;
    std::list<shm_block_t *> push_list;
    int pipe[2];
};

struct shm_queue_mgr_t {
    uint32_t channel_num;
    struct shm_queue_t *recv_queue;
    struct shm_queue_t *send_queue;
};

extern struct shm_queue_mgr_t g_shm_queue_mgr;

bool shmq_init(uint32_t channel, uint32_t size);
void shmq_fini();

int recv_push(uint32_t channel, struct shm_block_t *sb, const uint8_t * buf, bool safe);
int recv_pull(uint32_t channel, struct shm_block_t *sb, uint8_t **buf);

int send_push(uint32_t channel, struct shm_block_t *sb, const uint8_t *buf, bool safe);
int send_pull(uint32_t channel, struct shm_block_t *sb, uint8_t **buf);

int pruge_recv(uint32_t channel);
int pruge_send(uint32_t channel);

void write_pipe(int fd);
void pruge_pipe(int fd);

#endif
