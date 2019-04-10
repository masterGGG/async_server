#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <netinet/in.h>

#include "log.h"
#include "shmq.h"
#include "global.h"
#include "plugin.h"
#include "shm_sevent.h"
#include "tcp_socket.h"

#include <sys/time.h>
c_sevent::c_sevent()
{
    m_fd = -1;
}

c_sevent::~c_sevent()
{
}

bool c_sevent::start(uint32_t channel)
{
    m_channel = channel;
    m_fd = g_shm_queue_mgr.send_queue[channel].pipe[0];

    if (!g_reactor.add_handler(m_fd, this)) {
        handle_fini();
        return false;
    }

    if (!g_reactor.handle_ctl(m_fd, EPOLL_CTL_ADD, EPOLLIN)) {
        handle_fini();
        return false;
    }

    return true;
}

bool c_sevent::handle_input()
{
	struct timeval tm,ntm,ltm;
	long s,us;
	gettimeofday(&ltm, NULL);
   	//ERROR_LOG("[%lds %ldus] xxxxx Channel:%d", tm.tv_sec, tm.tv_usec, m_channel);///
    pruge_pipe(m_fd);

    int count = 1024;
    while (count > 0) {
        struct shm_block_t sb;
        uint8_t *buf;
        if (send_pull(m_channel, &sb, &buf) != 0)
		{
#ifdef DEBUG_LIBCO
	gettimeofday(&ntm, NULL);
	s = ntm.tv_sec - ltm.tv_sec;
	us = ntm.tv_usec - ltm.tv_usec;
	if (us < 0)
	{
		s--;
		us = 1000000-us;
	}
   	ERROR_LOG("<5>[%lds %ldus] cost:[%lds%ldus] Count:%d Channel:%d", ltm.tv_sec, ltm.tv_usec, s, us, 1024-count, m_channel);///
#endif
            break;
		}

#ifdef DEBUG_LIBCO
	gettimeofday(&tm, NULL);
#endif
        --count;
        c_handler *handler = g_reactor.get_handler(sb.fd);
        if (!handler)
            continue;

        if (sb.type == CLOSE_BLOCK) {
            handler->shut();
        } else {
            handler->send_pkg(buf, sb.len);
#ifdef DEBUG_LIBCO
	gettimeofday(&ntm, NULL);
	s = ntm.tv_sec - tm.tv_sec;
	us = ntm.tv_usec - tm.tv_usec;
	if (us < 0)
	{
		s--;
		us = 1000000-us;
	}
   	DEBUG_LOG("<4>[%lds %ldus] cost:[%lds%ldus] Count:%d Channel:%d Response:%s", tm.tv_sec, tm.tv_usec, s, us, count, m_channel, buf+8);///
#endif
        }
    }

    return true;
}

bool c_sevent::handle_output()
{
    return false;
}

void c_sevent::handle_error()
{
    handle_fini();
}

void c_sevent::handle_fini()
{
    if (m_fd) {
        g_reactor.del_handler(m_fd);
        close(m_fd);
        m_fd = -1;
    }

    delete this; 
}
