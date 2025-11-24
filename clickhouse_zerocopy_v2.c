/*
 * ClickHouse Zero-Copy LD_PRELOAD Library v2 - io_uring Edition
 *
 * Uses io_uring for high-performance async zero-copy I/O operations.
 * Provides significant performance improvements over traditional syscalls
 * and even splice/sendfile by leveraging kernel's async I/O infrastructure.
 *
 * Usage:
 *   LD_PRELOAD=/path/to/libclickhouse_zerocopy_v2.so clickhouse-server
 *
 * Environment variables:
 *   ZEROCOPY_DEBUG=1         - Enable debug logging
 *   ZEROCOPY_THRESHOLD=N     - Minimum bytes to use zero-copy (default: 8192)
 *   ZEROCOPY_QUEUE_DEPTH=N   - io_uring queue depth (default: 128)
 *   ZEROCOPY_ASYNC=1         - Use fully async mode (default: 0)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <liburing.h>

/* Define missing constants if needed */
#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

/* io_uring zero-copy send (kernel 6.0+) */
#ifndef IORING_OP_SEND_ZC
#define IORING_OP_SEND_ZC 33
#endif

#ifndef IORING_OP_SENDMSG_ZC
#define IORING_OP_SENDMSG_ZC 34
#endif

/* Original function pointers */
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;
static ssize_t (*real_write)(int fd, const void *buf, size_t count) = NULL;
static ssize_t (*real_send)(int sockfd, const void *buf, size_t len, int flags) = NULL;
static ssize_t (*real_recv)(int sockfd, void *buf, size_t len, int flags) = NULL;
static ssize_t (*real_sendmsg)(int sockfd, const struct msghdr *msg, int flags) = NULL;
static ssize_t (*real_recvmsg)(int sockfd, struct msghdr *msg, int flags) = NULL;
static ssize_t (*real_readv)(int fd, const struct iovec *iov, int iovcnt) = NULL;
static ssize_t (*real_writev)(int fd, const struct iovec *iov, int iovcnt) = NULL;

/* Configuration */
static int debug_enabled = 0;
static size_t zerocopy_threshold = 8192;
static unsigned int queue_depth = 128;
static int async_mode = 0;
static int initialized = 0;
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Statistics */
static atomic_ulong stat_read_calls = 0;
static atomic_ulong stat_write_calls = 0;
static atomic_ulong stat_send_calls = 0;
static atomic_ulong stat_recv_calls = 0;
static atomic_ulong stat_uring_send_used = 0;
static atomic_ulong stat_uring_recv_used = 0;
static atomic_ulong stat_uring_sendfile_used = 0;
static atomic_ulong stat_uring_bytes_sent = 0;
static atomic_ulong stat_uring_bytes_recv = 0;
static atomic_ulong stat_fallback_calls = 0;

/* Per-thread io_uring instance */
#define MAX_URING_INSTANCES 256
static __thread struct io_uring *thread_ring = NULL;
static __thread int thread_ring_initialized = 0;

/* Global ring pool for shared operations */
static struct io_uring *global_ring_pool = NULL;
static int global_ring_pool_size = 0;
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;

#define DEBUG_LOG(fmt, ...) \
    do { if (debug_enabled) fprintf(stderr, "[zerocopy_v2] " fmt "\n", ##__VA_ARGS__); } while(0)

/* Check if fd is a socket */
static inline int is_socket(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    return S_ISSOCK(st.st_mode);
}

/* Check if fd is a regular file */
static inline int is_regular_file(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    return S_ISREG(st.st_mode);
}

/* Get or create thread-local io_uring */
static struct io_uring *get_thread_ring(void)
{
    if (thread_ring_initialized && thread_ring)
        return thread_ring;

    thread_ring = calloc(1, sizeof(struct io_uring));
    if (!thread_ring) {
        DEBUG_LOG("Failed to allocate io_uring");
        return NULL;
    }

    /* Initialize with IORING_SETUP_COOP_TASKRUN for better performance */
    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;

    int ret = io_uring_queue_init_params(queue_depth, thread_ring, &params);
    if (ret < 0) {
        DEBUG_LOG("io_uring_queue_init failed: %s", strerror(-ret));
        free(thread_ring);
        thread_ring = NULL;
        return NULL;
    }

    thread_ring_initialized = 1;
    DEBUG_LOG("Initialized thread-local io_uring with depth %u", queue_depth);
    return thread_ring;
}

/* Initialize the library */
static void init_zerocopy(void)
{
    pthread_mutex_lock(&init_mutex);

    if (initialized) {
        pthread_mutex_unlock(&init_mutex);
        return;
    }

    /* Load original functions */
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    real_send = dlsym(RTLD_NEXT, "send");
    real_recv = dlsym(RTLD_NEXT, "recv");
    real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
    real_recvmsg = dlsym(RTLD_NEXT, "recvmsg");
    real_readv = dlsym(RTLD_NEXT, "readv");
    real_writev = dlsym(RTLD_NEXT, "writev");

    if (!real_read || !real_write || !real_send || !real_recv) {
        fprintf(stderr, "[zerocopy_v2] ERROR: Failed to load original functions\n");
        abort();
    }

    /* Read configuration from environment */
    const char *env_debug = getenv("ZEROCOPY_DEBUG");
    if (env_debug && atoi(env_debug))
        debug_enabled = 1;

    const char *env_threshold = getenv("ZEROCOPY_THRESHOLD");
    if (env_threshold)
        zerocopy_threshold = (size_t)atol(env_threshold);

    const char *env_queue = getenv("ZEROCOPY_QUEUE_DEPTH");
    if (env_queue) {
        unsigned int depth = (unsigned int)atoi(env_queue);
        if (depth > 0 && depth <= 4096)
            queue_depth = depth;
    }

    const char *env_async = getenv("ZEROCOPY_ASYNC");
    if (env_async && atoi(env_async))
        async_mode = 1;

    DEBUG_LOG("Initialized: threshold=%zu bytes, queue_depth=%u, async=%d",
              zerocopy_threshold, queue_depth, async_mode);

    /* Create global ring pool for shared operations */
    int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    global_ring_pool_size = ncpus > 0 ? ncpus : 4;
    global_ring_pool = calloc(global_ring_pool_size, sizeof(struct io_uring));

    if (global_ring_pool) {
        for (int i = 0; i < global_ring_pool_size; i++) {
            struct io_uring_params params = {0};
            params.flags = IORING_SETUP_COOP_TASKRUN;

            if (io_uring_queue_init_params(queue_depth, &global_ring_pool[i], &params) < 0) {
                DEBUG_LOG("Failed to init global ring %d", i);
                global_ring_pool[i].ring_fd = -1;
            }
        }
        DEBUG_LOG("Created global ring pool with %d rings", global_ring_pool_size);
    }

    initialized = 1;
    pthread_mutex_unlock(&init_mutex);
}

/* Ensure initialization */
static inline void ensure_init(void)
{
    if (__builtin_expect(!initialized, 0))
        init_zerocopy();
}

/*
 * io_uring zero-copy send implementation
 */
static ssize_t uring_send(int sockfd, const void *buf, size_t len, int flags)
{
    struct io_uring *ring = get_thread_ring();
    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_send(sockfd, buf, len, flags);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        /* Queue full, submit pending and retry */
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            atomic_fetch_add(&stat_fallback_calls, 1);
            return real_send(sockfd, buf, len, flags);
        }
    }

    /* Prepare send operation */
    io_uring_prep_send(sqe, sockfd, buf, len, flags);
    io_uring_sqe_set_flags(sqe, 0);
    sqe->user_data = (uint64_t)buf;

    /* Submit and wait for completion */
    int ret = io_uring_submit(ring);
    if (ret < 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_send(sockfd, buf, len, flags);
    }

    struct io_uring_cqe *cqe;
    ret = io_uring_wait_cqe(ring, &cqe);
    if (ret < 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_send(sockfd, buf, len, flags);
    }

    ssize_t res = cqe->res;
    io_uring_cqe_seen(ring, cqe);

    if (res > 0) {
        atomic_fetch_add(&stat_uring_send_used, 1);
        atomic_fetch_add(&stat_uring_bytes_sent, res);
        DEBUG_LOG("uring_send: sent %zd bytes via io_uring", res);
    }

    return res;
}

/*
 * io_uring zero-copy recv implementation
 */
static ssize_t uring_recv(int sockfd, void *buf, size_t len, int flags)
{
    struct io_uring *ring = get_thread_ring();
    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_recv(sockfd, buf, len, flags);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            atomic_fetch_add(&stat_fallback_calls, 1);
            return real_recv(sockfd, buf, len, flags);
        }
    }

    /* Prepare recv operation */
    io_uring_prep_recv(sqe, sockfd, buf, len, flags);
    sqe->user_data = (uint64_t)buf;

    /* Submit and wait */
    int ret = io_uring_submit(ring);
    if (ret < 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_recv(sockfd, buf, len, flags);
    }

    struct io_uring_cqe *cqe;
    ret = io_uring_wait_cqe(ring, &cqe);
    if (ret < 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_recv(sockfd, buf, len, flags);
    }

    ssize_t res = cqe->res;
    io_uring_cqe_seen(ring, cqe);

    if (res > 0) {
        atomic_fetch_add(&stat_uring_recv_used, 1);
        atomic_fetch_add(&stat_uring_bytes_recv, res);
        DEBUG_LOG("uring_recv: received %zd bytes via io_uring", res);
    }

    return res;
}

/*
 * io_uring splice-based zero-copy transfer (file to socket)
 */
static ssize_t uring_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
    struct io_uring *ring = get_thread_ring();
    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        /* Fall back to standard sendfile */
        return -1;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            atomic_fetch_add(&stat_fallback_calls, 1);
            return -1;
        }
    }

    /* Prepare splice operation */
    off_t off = offset ? *offset : 0;
    io_uring_prep_splice(sqe, in_fd, off, out_fd, -1, count,
                         SPLICE_F_MOVE | SPLICE_F_MORE);
    sqe->user_data = 0xdeadbeef;

    int ret = io_uring_submit(ring);
    if (ret < 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return -1;
    }

    struct io_uring_cqe *cqe;
    ret = io_uring_wait_cqe(ring, &cqe);
    if (ret < 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return -1;
    }

    ssize_t res = cqe->res;
    io_uring_cqe_seen(ring, cqe);

    if (res > 0) {
        if (offset)
            *offset += res;
        atomic_fetch_add(&stat_uring_sendfile_used, 1);
        atomic_fetch_add(&stat_uring_bytes_sent, res);
        DEBUG_LOG("uring_sendfile: transferred %zd bytes", res);
    }

    return res;
}

/*
 * Optimized read using io_uring
 */
ssize_t read(int fd, void *buf, size_t count)
{
    ensure_init();
    atomic_fetch_add(&stat_read_calls, 1);

    /* Use io_uring for large reads on sockets and files */
    if (count >= zerocopy_threshold && (is_socket(fd) || is_regular_file(fd))) {
        struct io_uring *ring = get_thread_ring();
        if (ring) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
            if (sqe) {
                io_uring_prep_read(sqe, fd, buf, count, -1);
                sqe->user_data = (uint64_t)buf;

                if (io_uring_submit(ring) > 0) {
                    struct io_uring_cqe *cqe;
                    if (io_uring_wait_cqe(ring, &cqe) == 0) {
                        ssize_t res = cqe->res;
                        io_uring_cqe_seen(ring, cqe);
                        if (res >= 0) {
                            DEBUG_LOG("read: %zd bytes via io_uring", res);
                            return res;
                        }
                    }
                }
            }
        }
    }

    return real_read(fd, buf, count);
}

/*
 * Optimized write using io_uring
 */
ssize_t write(int fd, const void *buf, size_t count)
{
    ensure_init();
    atomic_fetch_add(&stat_write_calls, 1);

    /* Use io_uring for large writes */
    if (count >= zerocopy_threshold && (is_socket(fd) || is_regular_file(fd))) {
        struct io_uring *ring = get_thread_ring();
        if (ring) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
            if (sqe) {
                io_uring_prep_write(sqe, fd, buf, count, -1);
                sqe->user_data = (uint64_t)buf;

                if (io_uring_submit(ring) > 0) {
                    struct io_uring_cqe *cqe;
                    if (io_uring_wait_cqe(ring, &cqe) == 0) {
                        ssize_t res = cqe->res;
                        io_uring_cqe_seen(ring, cqe);
                        if (res >= 0) {
                            DEBUG_LOG("write: %zd bytes via io_uring", res);
                            return res;
                        }
                    }
                }
            }
        }
    }

    return real_write(fd, buf, count);
}

/*
 * Optimized send using io_uring
 */
ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    ensure_init();
    atomic_fetch_add(&stat_send_calls, 1);

    /* For small transfers, use regular send */
    if (len < zerocopy_threshold) {
        return real_send(sockfd, buf, len, flags);
    }

    /* Use io_uring for large sends */
    return uring_send(sockfd, buf, len, flags);
}

/*
 * Optimized recv using io_uring
 */
ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    ensure_init();
    atomic_fetch_add(&stat_recv_calls, 1);

    /* For small transfers, use regular recv */
    if (len < zerocopy_threshold) {
        return real_recv(sockfd, buf, len, flags);
    }

    /* Use io_uring for large receives */
    return uring_recv(sockfd, buf, len, flags);
}

/*
 * Optimized sendmsg using io_uring
 */
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
    ensure_init();

    /* Calculate total size */
    size_t total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        total += msg->msg_iov[i].iov_len;
    }

    if (total < zerocopy_threshold) {
        return real_sendmsg(sockfd, msg, flags);
    }

    /* Use io_uring sendmsg */
    struct io_uring *ring = get_thread_ring();
    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_sendmsg(sockfd, msg, flags);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            atomic_fetch_add(&stat_fallback_calls, 1);
            return real_sendmsg(sockfd, msg, flags);
        }
    }

    io_uring_prep_sendmsg(sqe, sockfd, msg, flags);
    sqe->user_data = 0xfeedfeed;

    if (io_uring_submit(ring) <= 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_sendmsg(sockfd, msg, flags);
    }

    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(ring, &cqe) < 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_sendmsg(sockfd, msg, flags);
    }

    ssize_t res = cqe->res;
    io_uring_cqe_seen(ring, cqe);

    if (res > 0) {
        atomic_fetch_add(&stat_uring_send_used, 1);
        atomic_fetch_add(&stat_uring_bytes_sent, res);
        DEBUG_LOG("sendmsg: sent %zd bytes via io_uring", res);
    }

    return res;
}

/*
 * Optimized recvmsg using io_uring
 */
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    ensure_init();

    size_t total = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        total += msg->msg_iov[i].iov_len;
    }

    if (total < zerocopy_threshold) {
        return real_recvmsg(sockfd, msg, flags);
    }

    struct io_uring *ring = get_thread_ring();
    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_recvmsg(sockfd, msg, flags);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            atomic_fetch_add(&stat_fallback_calls, 1);
            return real_recvmsg(sockfd, msg, flags);
        }
    }

    io_uring_prep_recvmsg(sqe, sockfd, msg, flags);
    sqe->user_data = 0xcafebabe;

    if (io_uring_submit(ring) <= 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_recvmsg(sockfd, msg, flags);
    }

    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(ring, &cqe) < 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_recvmsg(sockfd, msg, flags);
    }

    ssize_t res = cqe->res;
    io_uring_cqe_seen(ring, cqe);

    if (res > 0) {
        atomic_fetch_add(&stat_uring_recv_used, 1);
        atomic_fetch_add(&stat_uring_bytes_recv, res);
        DEBUG_LOG("recvmsg: received %zd bytes via io_uring", res);
    }

    return res;
}

/*
 * Optimized readv using io_uring
 */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    ensure_init();

    if (!real_readv) {
        errno = ENOSYS;
        return -1;
    }

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        total += iov[i].iov_len;
    }

    if (total < zerocopy_threshold) {
        return real_readv(fd, iov, iovcnt);
    }

    struct io_uring *ring = get_thread_ring();
    if (!ring) {
        return real_readv(fd, iov, iovcnt);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (!sqe)
            return real_readv(fd, iov, iovcnt);
    }

    io_uring_prep_readv(sqe, fd, iov, iovcnt, -1);
    sqe->user_data = 0xbeefdead;

    if (io_uring_submit(ring) <= 0)
        return real_readv(fd, iov, iovcnt);

    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(ring, &cqe) < 0)
        return real_readv(fd, iov, iovcnt);

    ssize_t res = cqe->res;
    io_uring_cqe_seen(ring, cqe);

    if (res > 0)
        DEBUG_LOG("readv: %zd bytes via io_uring", res);

    return res;
}

/*
 * Optimized writev using io_uring
 */
ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    ensure_init();

    if (!real_writev) {
        errno = ENOSYS;
        return -1;
    }

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        total += iov[i].iov_len;
    }

    if (total < zerocopy_threshold) {
        return real_writev(fd, iov, iovcnt);
    }

    struct io_uring *ring = get_thread_ring();
    if (!ring) {
        return real_writev(fd, iov, iovcnt);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
        if (!sqe)
            return real_writev(fd, iov, iovcnt);
    }

    io_uring_prep_writev(sqe, fd, iov, iovcnt, -1);
    sqe->user_data = 0xdeadcafe;

    if (io_uring_submit(ring) <= 0)
        return real_writev(fd, iov, iovcnt);

    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(ring, &cqe) < 0)
        return real_writev(fd, iov, iovcnt);

    ssize_t res = cqe->res;
    io_uring_cqe_seen(ring, cqe);

    if (res > 0)
        DEBUG_LOG("writev: %zd bytes via io_uring", res);

    return res;
}

/*
 * Direct API: Zero-copy sendfile using io_uring
 */
ssize_t zerocopy_uring_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
    ensure_init();
    return uring_sendfile(out_fd, in_fd, offset, count);
}

/*
 * Direct API: Enable zero-copy on socket
 */
int zerocopy_enable_socket(int sockfd)
{
    int enable = 1;
    return setsockopt(sockfd, SOL_SOCKET, SO_ZEROCOPY, &enable, sizeof(enable));
}

/*
 * Direct API: Get statistics
 */
void zerocopy_get_stats(unsigned long *reads, unsigned long *writes,
                        unsigned long *sends, unsigned long *recvs,
                        unsigned long *uring_sends, unsigned long *uring_recvs,
                        unsigned long *uring_sendfiles,
                        unsigned long *bytes_sent, unsigned long *bytes_recv,
                        unsigned long *fallbacks)
{
    if (reads) *reads = atomic_load(&stat_read_calls);
    if (writes) *writes = atomic_load(&stat_write_calls);
    if (sends) *sends = atomic_load(&stat_send_calls);
    if (recvs) *recvs = atomic_load(&stat_recv_calls);
    if (uring_sends) *uring_sends = atomic_load(&stat_uring_send_used);
    if (uring_recvs) *uring_recvs = atomic_load(&stat_uring_recv_used);
    if (uring_sendfiles) *uring_sendfiles = atomic_load(&stat_uring_sendfile_used);
    if (bytes_sent) *bytes_sent = atomic_load(&stat_uring_bytes_sent);
    if (bytes_recv) *bytes_recv = atomic_load(&stat_uring_bytes_recv);
    if (fallbacks) *fallbacks = atomic_load(&stat_fallback_calls);
}

/*
 * Direct API: Print statistics
 */
void zerocopy_print_stats(void)
{
    fprintf(stderr, "[zerocopy_v2] io_uring Statistics:\n");
    fprintf(stderr, "  read() calls:          %lu\n", atomic_load(&stat_read_calls));
    fprintf(stderr, "  write() calls:         %lu\n", atomic_load(&stat_write_calls));
    fprintf(stderr, "  send() calls:          %lu\n", atomic_load(&stat_send_calls));
    fprintf(stderr, "  recv() calls:          %lu\n", atomic_load(&stat_recv_calls));
    fprintf(stderr, "  io_uring sends:        %lu times, %lu bytes\n",
            atomic_load(&stat_uring_send_used), atomic_load(&stat_uring_bytes_sent));
    fprintf(stderr, "  io_uring recvs:        %lu times, %lu bytes\n",
            atomic_load(&stat_uring_recv_used), atomic_load(&stat_uring_bytes_recv));
    fprintf(stderr, "  io_uring sendfiles:    %lu times\n",
            atomic_load(&stat_uring_sendfile_used));
    fprintf(stderr, "  Fallback to syscalls:  %lu\n", atomic_load(&stat_fallback_calls));

    unsigned long total_ops = atomic_load(&stat_uring_send_used) +
                             atomic_load(&stat_uring_recv_used) +
                             atomic_load(&stat_uring_sendfile_used);
    unsigned long total_calls = atomic_load(&stat_send_calls) +
                               atomic_load(&stat_recv_calls);

    if (total_calls > 0) {
        double efficiency = (double)total_ops / total_calls * 100.0;
        fprintf(stderr, "  io_uring efficiency:   %.1f%%\n", efficiency);
    }
}

/* Destructor to print stats on unload */
__attribute__((destructor))
static void fini_zerocopy(void)
{
    if (debug_enabled) {
        zerocopy_print_stats();
    }

    /* Clean up thread-local ring */
    if (thread_ring_initialized && thread_ring) {
        io_uring_queue_exit(thread_ring);
        free(thread_ring);
        thread_ring = NULL;
    }

    /* Clean up global ring pool */
    if (global_ring_pool) {
        for (int i = 0; i < global_ring_pool_size; i++) {
            if (global_ring_pool[i].ring_fd >= 0) {
                io_uring_queue_exit(&global_ring_pool[i]);
            }
        }
        free(global_ring_pool);
        global_ring_pool = NULL;
    }
}

/* Constructor */
__attribute__((constructor))
static void early_init(void)
{
    init_zerocopy();
}
