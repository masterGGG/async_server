#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <arpa/inet.h>

#include "log.h"
#include "shmq.h"
#include "config.h"
#include "global.h"
#include "plugin.h"
#include "mem_pool.h"

#define VERSION "1.0"

int fork_conn();
int fork_work(uint32_t channel);

static void sigterm_handler(int sig)
{
    g_stop = 1;
}

static inline int rlimit_reset()
{
    // 上调打开文件数的限制
    struct rlimit rl = {0};

    /**
     * @brief getrlimit
     *RLIMIT_AS //进程的最大虚内存空间，字节为单位。
     RLIMIT_CORE //内核转存文件的最大长度。
     RLIMIT_CPU //最大允许的CPU使用时间，秒为单位。当进程达到软限制，内核将给其发送SIGXCPU信号，这一信号的默认行为是终止进程的执行。然而，可以捕捉信号，处理句柄可将控制返回给主程序。如果进程继续耗费CPU时间，核心会以每秒一次的频率给其发送SIGXCPU信号，直到达到硬限制，那时将给进程发送 SIGKILL信号终止其执行。
     RLIMIT_DATA //进程数据段的最大值。
     RLIMIT_FSIZE //进程可建立的文件的最大长度。如果进程试图超出这一限制时，核心会给其发送SIGXFSZ信号，默认情况下将终止进程的执行。
     RLIMIT_LOCKS //进程可建立的锁和租赁的最大值。
     RLIMIT_MEMLOCK //进程可锁定在内存中的最大数据量，字节为单位。
     RLIMIT_MSGQUEUE //进程可为POSIX消息队列分配的最大字节数。
     RLIMIT_NICE //进程可通过setpriority() 或 nice()调用设置的最大完美值。
     RLIMIT_NOFILE //指定比进程可打开的最大文件描述词大一的值，超出此值，将会产生EMFILE错误。
     RLIMIT_NPROC //用户可拥有的最大进程数。
     RLIMIT_RTPRIO //进程可通过sched_setscheduler 和 sched_setparam设置的最大实时优先级。
     RLIMIT_SIGPENDING //用户可拥有的最大挂起信号数。
     RLIMIT_STACK //最大的进程堆栈，以字节为单位。
     */
    if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
    {
        printf("ERROR: getrlimit.\n");
        return -1;
    }
    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0 )
    {
        printf("ERROR: setrlimit.\n");
        return -1;
    }

    // 允许产生CORE文件
    if (getrlimit(RLIMIT_CORE, &rl) != 0)
    {
        printf("ERROR: getrlimit.\n");
        return -1;
    }

    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_CORE, &rl) != 0) {
        printf("ERROR: setrlimit.\n");
        return -1;
    }

    return 0;
}

static void daemon_start()
{
    rlimit_reset();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;        //注册信号处理器
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    signal(SIGPIPE,SIG_IGN);

    sigset_t sset;
    sigemptyset(&sset);
    sigaddset(&sset, SIGBUS);
    sigaddset(&sset, SIGILL);
    sigaddset(&sset, SIGFPE);
    sigaddset(&sset, SIGSEGV);
    sigaddset(&sset, SIGCHLD);
    sigaddset(&sset, SIGABRT);
    sigprocmask(SIG_UNBLOCK, &sset, &sset);
    daemon(1, 1);
}

int check_single()
{
    int fd = -1;
    char buf[16] = {0};

    fd = open(config_get_strval("pid_file", "./pid"), O_RDWR|O_CREAT, 0644);
    if (fd < 0)
        BOOT_LOG(-1, "check single failed");

    struct flock fl;

    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = getpid();

    if (fcntl(fd, F_SETLK, &fl) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            close(fd);
            BOOT_LOG(-1, "service is running");
            return -2;
        }
        BOOT_LOG(-1, "service is running");
    }

    if (ftruncate(fd, 0) != 0)
        BOOT_LOG(-1, "check single failed");

    snprintf(buf, sizeof(buf), "%d", (int)getpid());
    if (write(fd, buf, strlen(buf)) == -1)
        BOOT_LOG(-1, "check single failed");

    return 0;
}

int main(int argc, char* argv[])
{
    //全局变量，定义在global.h里，记录的是可执行程序的名字
    arg_start = argv[0];
    //
    arg_end = argv[argc-1] + strlen(argv[argc - 1]) + 1;
    env_start = environ[0];             //当前进程环境变量的起始地址

    if (argc != 2)
        exit(-1);

    daemon_start();

    INFO_LOG ("async_server %s, report bugs to <cliu.hust@hotmail.com>", VERSION);

    /**
     * @brief load_config_file 
     * 定义在config.cpp中，从配置文件读取配置到内存，用map<string, string>存储
     * @param argv[1]   配置文件路径
     */
    load_config_file(argv[1]);

    /**
     * @brief check_single
     * 将pid写入文件
     */
    check_single();

    if (config_get_strval("proc_name", NULL))
    {
        /**
         * @brief set_title 
         * 定义于global.cpp 145行，设置进程的名字
         * @param "%s-MAIN"
         * @param "proc_name", NULL
         */
        set_title("%s-MAIN", config_get_strval("proc_name", NULL));
    }
    else
    {
        set_title("%s-MAIN", arg_start);
    }

    load_work_file(config_get_strval("work_conf", ""));

    if (-1 == log_init(config_get_strval("log_dir", "./"), (log_lvl_t)config_get_intval("log_level", 8),
                       config_get_intval("log_size", 33554432),
                       config_get_intval("log_maxfiles", 100),
                       "main_")) {
        BOOT_LOG(-1, "log init");
    }

    init_warning_system();

    plugin_load(config_get_strval("plugin_file", ""));


    shmq_init(g_work_confs.size(), config_get_intval("shmq_size", 8388608));

    int max_connect = config_get_intval("max_connect", 10000);
    if (max_connect <= 4096)
        max_connect = 4096;

    g_max_connect = max_connect;

    int max_pkg_len = config_get_intval("max_pkg_len", 16384);
    if (max_pkg_len <= 4096)
        max_pkg_len = 4096;

    g_max_pkg_len = max_pkg_len;


    const char * bind_ip = config_get_strval("bind_ip", NULL);
    if (NULL == bind_ip)
    {
        BOOT_LOG(-1, "unspecified bind_ip");
        return -1;
    }

    if (0 != get_ip_by_name(bind_ip, g_bind_ip))
    {
        strncpy(g_bind_ip, bind_ip, 16);
    }


    if (g_plugin.plugin_init) 
    {
        if (g_plugin.plugin_init(PROC_MAIN) != 0)
            BOOT_LOG(-1, "plugin_init init failed PROC_MAIN");
    }


    for (uint32_t i = 0; i < g_work_confs.size(); ++i)
        g_work_confs[i].pid = fork_work(i);

    pid_t conn_pid = fork_conn();

    while (!g_stop) {
        int status = 0;
        pid_t p = waitpid(-1, &status, 0);
        if(-1 == p)
            continue;

        if (WEXITSTATUS(status) == 10) {
            if (p == conn_pid) {
                conn_pid = -1;
            } else {
                for (uint32_t i = 0; i < g_work_confs.size(); ++i) {
                    if (g_work_confs[i].pid == p) {
                        g_work_confs[i].pid = -1;
                        break;
                    }
                }
            }
        } else {
            if (p == conn_pid) {
                conn_pid = fork_conn();
                send_warning_msg("conn core", 0, 0, 0, get_bind_ip());
                ERROR_LOG("conn core");
            } else {
                for (uint32_t i = 0; i < g_work_confs.size(); ++i) {
                    if (g_work_confs[i].pid == p) {
                        ERROR_LOG("work core, id: %u, pid: %d", g_work_confs[i].id, p);
                        g_work_confs[i].pid = fork_work(i);
                        send_warning_msg("work core", g_work_confs[i].id, 0, 0, get_bind_ip());
                        break;
                    }
                }
            }
        }
    }

    for (uint32_t i = 0; i < g_work_confs.size(); ++i) {
        if (g_work_confs[i].pid != -1)
            kill(g_work_confs[i].pid, SIGTERM);
    }

    if (conn_pid != -1)
        kill(conn_pid, SIGTERM);

    while (true) {
        int ret = 0;
        for (uint32_t i = 0; i < g_work_confs.size(); ++i) {
            if (g_work_confs[i].pid != -1)
                ret = 1;
        }

        if (ret || conn_pid != -1) {
            int status = 0;
            pid_t p = waitpid(-1, &status, 0);
            if(-1 == p)
                continue;

            if (p == conn_pid) {
                conn_pid = -1;
            } else {
                for (uint32_t i = 0; i < g_work_confs.size(); ++i) {
                    if (g_work_confs[i].pid == p) {
                        g_work_confs[i].pid = -1;
                        break;
                    }
                }
            }
        } else {
            break;
        }
    }

    if (g_plugin.plugin_fini)
        g_plugin.plugin_fini(PROC_MAIN);
}
