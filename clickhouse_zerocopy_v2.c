/*
 * ClickHouse Zero-Copy LD_PRELOAD Library v2 - io_uring Edition
 *
 * This library intercepts common file and socket I/O used by ClickHouse and
 * related benchmarks. It routes large transfers through io_uring and keeps a
 * synchronous fallback for correctness when the optimized path is unavailable.
 *
 * Usage:
 *   LD_PRELOAD=/path/to/libclickhouse_zerocopy_v2.so clickhouse-server
 *
 * Environment variables:
 *   ZEROCOPY_DEBUG=1         - Enable debug logging
 *   ZEROCOPY_THRESHOLD=N     - Minimum bytes to use io_uring (default: 8192)
 *   ZEROCOPY_QUEUE_DEPTH=N   - io_uring queue depth (default: 128)
 *   ZEROCOPY_ASYNC=1         - Hint io_uring to run requests asynchronously
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
#include <sys/sendfile.h>
#include <stdatomic.h>
#include <pthread.h>
#include <liburing.h>

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

/* Original function pointers */
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;
static ssize_t (*real_write)(int fd, const void *buf, size_t count) = NULL;
static ssize_t (*real_pread)(int fd, void *buf, size_t count, off_t offset) = NULL;
static ssize_t (*real_pwrite)(int fd, const void *buf, size_t count, off_t offset) = NULL;
static ssize_t (*real_send)(int sockfd, const void *buf, size_t len, int flags) = NULL;
static ssize_t (*real_recv)(int sockfd, void *buf, size_t len, int flags) = NULL;
static ssize_t (*real_sendto)(int sockfd, const void *buf, size_t len, int flags,
                              const struct sockaddr *dest_addr, socklen_t addrlen) = NULL;
static ssize_t (*real_recvfrom)(int sockfd, void *buf, size_t len, int flags,
                                struct sockaddr *src_addr, socklen_t *addrlen) = NULL;
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
static __thread struct io_uring *thread_ring = NULL;
static __thread int thread_ring_initialized = 0;
static pthread_key_t thread_ring_key;
static pthread_once_t thread_ring_key_once = PTHREAD_ONCE_INIT;

#define DEBUG_LOG(fmt, ...) \
    do { if (debug_enabled) fprintf(stderr, "[zerocopy_v2] " fmt "\n", ##__VA_ARGS__); } while (0)

static void destroy_thread_ring(void *ptr)
{
    struct io_uring *ring = ptr;

    if (!ring)
        return;

    io_uring_queue_exit(ring);
    free(ring);

    if (ring == thread_ring) {
        thread_ring = NULL;
        thread_ring_initialized = 0;
    }
}

static void make_thread_ring_key(void)
{
    pthread_key_create(&thread_ring_key, destroy_thread_ring);
}

static inline int is_socket(int fd)
{
    struct stat st;

    if (fstat(fd, &st) < 0)
        return 0;
    return S_ISSOCK(st.st_mode);
}

static inline int is_regular_file(int fd)
{
    struct stat st;

    if (fstat(fd, &st) < 0)
        return 0;
    return S_ISREG(st.st_mode);
}

static inline ssize_t normalize_cqe_result(int res)
{
    if (res < 0) {
        errno = -res;
        return -1;
    }
    return res;
}

static inline void maybe_mark_async(struct io_uring_sqe *sqe)
{
    if (!async_mode)
        return;
#ifdef IOSQE_ASYNC
    io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);
#else
    (void)sqe;
#endif
}

static struct io_uring *get_thread_ring(void)
{
    struct io_uring_params params;
    int ret;

    if (thread_ring_initialized && thread_ring)
        return thread_ring;

    thread_ring = calloc(1, sizeof(*thread_ring));
    if (!thread_ring) {
        DEBUG_LOG("Failed to allocate io_uring");
        return NULL;
    }

    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;
#ifdef IORING_SETUP_DEFER_TASKRUN
    if (async_mode)
        params.flags |= IORING_SETUP_DEFER_TASKRUN;
#endif

    ret = io_uring_queue_init_params(queue_depth, thread_ring, &params);
    if (ret < 0) {
        DEBUG_LOG("io_uring_queue_init failed: %s", strerror(-ret));
        free(thread_ring);
        thread_ring = NULL;
        return NULL;
    }

    pthread_once(&thread_ring_key_once, make_thread_ring_key);
    pthread_setspecific(thread_ring_key, thread_ring);

    thread_ring_initialized = 1;
    DEBUG_LOG("Initialized thread-local io_uring with depth %u", queue_depth);
    return thread_ring;
}

static struct io_uring_sqe *get_sqe_with_retry(struct io_uring *ring)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

    if (sqe)
        return sqe;

    if (io_uring_submit(ring) < 0)
        return NULL;

    return io_uring_get_sqe(ring);
}

static ssize_t submit_and_wait(struct io_uring *ring)
{
    struct io_uring_cqe *cqe;
    int ret;
    int res;

    ret = io_uring_submit(ring);
    if (ret <= 0) {
        if (ret < 0)
            errno = -ret;
        else
            errno = EIO;
        return -1;
    }

    ret = io_uring_wait_cqe(ring, &cqe);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }

    res = cqe->res;
    io_uring_cqe_seen(ring, cqe);
    return normalize_cqe_result(res);
}

static void init_zerocopy(void)
{
    pthread_mutex_lock(&init_mutex);

    if (initialized) {
        pthread_mutex_unlock(&init_mutex);
        return;
    }

    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    real_pread = dlsym(RTLD_NEXT, "pread");
    real_pwrite = dlsym(RTLD_NEXT, "pwrite");
    real_send = dlsym(RTLD_NEXT, "send");
    real_recv = dlsym(RTLD_NEXT, "recv");
    real_sendto = dlsym(RTLD_NEXT, "sendto");
    real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");
    real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
    real_recvmsg = dlsym(RTLD_NEXT, "recvmsg");
    real_readv = dlsym(RTLD_NEXT, "readv");
    real_writev = dlsym(RTLD_NEXT, "writev");

    if (!real_read || !real_write || !real_pread || !real_pwrite ||
        !real_send || !real_recv || !real_sendto || !real_recvfrom ||
        !real_sendmsg || !real_recvmsg || !real_readv || !real_writev) {
        fprintf(stderr, "[zerocopy_v2] ERROR: Failed to load original functions\n");
        abort();
    }

    if (getenv("ZEROCOPY_DEBUG") && atoi(getenv("ZEROCOPY_DEBUG")))
        debug_enabled = 1;

    if (getenv("ZEROCOPY_THRESHOLD"))
        zerocopy_threshold = (size_t)atol(getenv("ZEROCOPY_THRESHOLD"));

    if (getenv("ZEROCOPY_QUEUE_DEPTH")) {
        unsigned int depth = (unsigned int)atoi(getenv("ZEROCOPY_QUEUE_DEPTH"));
        if (depth > 0 && depth <= 4096)
            queue_depth = depth;
    }

    if (getenv("ZEROCOPY_ASYNC") && atoi(getenv("ZEROCOPY_ASYNC")))
        async_mode = 1;

    pthread_once(&thread_ring_key_once, make_thread_ring_key);

    DEBUG_LOG("Initialized: threshold=%zu bytes, queue_depth=%u, async=%d",
              zerocopy_threshold, queue_depth, async_mode);

    initialized = 1;
    pthread_mutex_unlock(&init_mutex);
}

static inline void ensure_init(void)
{
    if (__builtin_expect(!initialized, 0))
        init_zerocopy();
}

static ssize_t uring_send(int sockfd, const void *buf, size_t len, int flags)
{
    struct io_uring *ring = get_thread_ring();
    struct io_uring_sqe *sqe;
    ssize_t res;

    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_send(sockfd, buf, len, flags);
    }

    sqe = get_sqe_with_retry(ring);
    if (!sqe) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_send(sockfd, buf, len, flags);
    }

    io_uring_prep_send(sqe, sockfd, buf, len, flags);
    maybe_mark_async(sqe);

    res = submit_and_wait(ring);
    if (res >= 0) {
        atomic_fetch_add(&stat_uring_send_used, 1);
        atomic_fetch_add(&stat_uring_bytes_sent, (unsigned long)res);
        DEBUG_LOG("uring_send: sent %zd bytes via io_uring", res);
    }

    return res;
}

static ssize_t uring_recv(int sockfd, void *buf, size_t len, int flags)
{
    struct io_uring *ring = get_thread_ring();
    struct io_uring_sqe *sqe;
    ssize_t res;

    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_recv(sockfd, buf, len, flags);
    }

    sqe = get_sqe_with_retry(ring);
    if (!sqe) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_recv(sockfd, buf, len, flags);
    }

    io_uring_prep_recv(sqe, sockfd, buf, len, flags);
    maybe_mark_async(sqe);

    res = submit_and_wait(ring);
    if (res >= 0) {
        atomic_fetch_add(&stat_uring_recv_used, 1);
        atomic_fetch_add(&stat_uring_bytes_recv, (unsigned long)res);
        DEBUG_LOG("uring_recv: received %zd bytes via io_uring", res);
    }

    return res;
}

static ssize_t uring_file_read(int fd, void *buf, size_t count, off_t offset)
{
    struct io_uring *ring = get_thread_ring();
    struct io_uring_sqe *sqe;
    ssize_t res;

    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        if (offset >= 0)
            return real_pread(fd, buf, count, offset);
        return real_read(fd, buf, count);
    }

    sqe = get_sqe_with_retry(ring);
    if (!sqe) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        if (offset >= 0)
            return real_pread(fd, buf, count, offset);
        return real_read(fd, buf, count);
    }

    io_uring_prep_read(sqe, fd, buf, count, offset);
    maybe_mark_async(sqe);

    res = submit_and_wait(ring);
    if (res >= 0)
        DEBUG_LOG("uring_read: read %zd bytes via io_uring", res);

    return res;
}

static ssize_t uring_file_write(int fd, const void *buf, size_t count, off_t offset)
{
    struct io_uring *ring = get_thread_ring();
    struct io_uring_sqe *sqe;
    ssize_t res;

    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        if (offset >= 0)
            return real_pwrite(fd, buf, count, offset);
        return real_write(fd, buf, count);
    }

    sqe = get_sqe_with_retry(ring);
    if (!sqe) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        if (offset >= 0)
            return real_pwrite(fd, buf, count, offset);
        return real_write(fd, buf, count);
    }

    io_uring_prep_write(sqe, fd, buf, count, offset);
    maybe_mark_async(sqe);

    res = submit_and_wait(ring);
    if (res >= 0)
        DEBUG_LOG("uring_write: wrote %zd bytes via io_uring", res);

    return res;
}

static ssize_t uring_sendmsg_internal(int sockfd, const struct msghdr *msg, int flags,
                                      ssize_t (*fallback)(int, const struct msghdr *, int))
{
    struct io_uring *ring = get_thread_ring();
    struct io_uring_sqe *sqe;
    ssize_t res;

    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return fallback(sockfd, msg, flags);
    }

    sqe = get_sqe_with_retry(ring);
    if (!sqe) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return fallback(sockfd, msg, flags);
    }

    io_uring_prep_sendmsg(sqe, sockfd, msg, flags);
    maybe_mark_async(sqe);

    res = submit_and_wait(ring);
    if (res >= 0) {
        atomic_fetch_add(&stat_uring_send_used, 1);
        atomic_fetch_add(&stat_uring_bytes_sent, (unsigned long)res);
        DEBUG_LOG("uring_sendmsg: sent %zd bytes via io_uring", res);
    }

    return res;
}

static ssize_t uring_recvmsg_internal(int sockfd, struct msghdr *msg, int flags,
                                      ssize_t (*fallback)(int, struct msghdr *, int))
{
    struct io_uring *ring = get_thread_ring();
    struct io_uring_sqe *sqe;
    ssize_t res;

    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return fallback(sockfd, msg, flags);
    }

    sqe = get_sqe_with_retry(ring);
    if (!sqe) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return fallback(sockfd, msg, flags);
    }

    io_uring_prep_recvmsg(sqe, sockfd, msg, flags);
    maybe_mark_async(sqe);

    res = submit_and_wait(ring);
    if (res >= 0) {
        atomic_fetch_add(&stat_uring_recv_used, 1);
        atomic_fetch_add(&stat_uring_bytes_recv, (unsigned long)res);
        DEBUG_LOG("uring_recvmsg: received %zd bytes via io_uring", res);
    }

    return res;
}

static inline size_t iov_total_size(const struct iovec *iov, int iovcnt)
{
    size_t total = 0;

    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;

    return total;
}

static ssize_t uring_splice_once(struct io_uring *ring, int in_fd, off_t off_in,
                                 int out_fd, off_t off_out, size_t count,
                                 unsigned int flags)
{
    struct io_uring_sqe *sqe = get_sqe_with_retry(ring);

    if (!sqe) {
        errno = EAGAIN;
        return -1;
    }

    io_uring_prep_splice(sqe, in_fd, off_in, out_fd, off_out, count, flags);
    maybe_mark_async(sqe);
    return submit_and_wait(ring);
}

static ssize_t uring_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
    struct io_uring *ring = get_thread_ring();
    int pipefd[2] = {-1, -1};
    size_t remaining = count;
    ssize_t total = 0;
    off_t file_off = offset ? *offset : 0;

    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return sendfile(out_fd, in_fd, offset, count);
    }

    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return sendfile(out_fd, in_fd, offset, count);
    }

    while (remaining > 0) {
        size_t chunk = remaining > (1U << 20) ? (1U << 20) : remaining;
        ssize_t spliced_in;

        spliced_in = uring_splice_once(ring, in_fd, offset ? file_off : -1,
                                       pipefd[1], -1, chunk,
                                       SPLICE_F_MOVE | SPLICE_F_MORE);
        if (spliced_in <= 0) {
            if (spliced_in < 0 && total == 0) {
                int saved_errno = errno;
                close(pipefd[0]);
                close(pipefd[1]);
                atomic_fetch_add(&stat_fallback_calls, 1);
                errno = saved_errno;
                return sendfile(out_fd, in_fd, offset, count);
            }
            break;
        }

        if (offset)
            file_off += spliced_in;

        for (ssize_t sent = 0; sent < spliced_in;) {
            ssize_t spliced_out = uring_splice_once(ring, pipefd[0], -1, out_fd, -1,
                                                    (size_t)(spliced_in - sent),
                                                    SPLICE_F_MOVE | SPLICE_F_MORE);
            if (spliced_out <= 0) {
                close(pipefd[0]);
                close(pipefd[1]);
                if (offset)
                    *offset = file_off;
                return total > 0 ? total : spliced_out;
            }
            sent += spliced_out;
            total += spliced_out;
            remaining -= spliced_out;
        }
    }

    close(pipefd[0]);
    close(pipefd[1]);

    if (offset)
        *offset = file_off;

    if (total > 0) {
        atomic_fetch_add(&stat_uring_sendfile_used, 1);
        atomic_fetch_add(&stat_uring_bytes_sent, (unsigned long)total);
        DEBUG_LOG("uring_sendfile: transferred %zd bytes", total);
    }

    return total;
}

ssize_t read(int fd, void *buf, size_t count)
{
    ensure_init();
    atomic_fetch_add(&stat_read_calls, 1);

    if (count < zerocopy_threshold)
        return real_read(fd, buf, count);

    if (is_socket(fd))
        return uring_recv(fd, buf, count, 0);

    if (is_regular_file(fd))
        return uring_file_read(fd, buf, count, -1);

    return real_read(fd, buf, count);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    ensure_init();
    atomic_fetch_add(&stat_read_calls, 1);

    if (count < zerocopy_threshold || !is_regular_file(fd))
        return real_pread(fd, buf, count, offset);

    return uring_file_read(fd, buf, count, offset);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    ensure_init();
    atomic_fetch_add(&stat_write_calls, 1);

    if (count < zerocopy_threshold)
        return real_write(fd, buf, count);

    if (is_socket(fd))
        return uring_send(fd, buf, count, 0);

    if (is_regular_file(fd))
        return uring_file_write(fd, buf, count, -1);

    return real_write(fd, buf, count);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    ensure_init();
    atomic_fetch_add(&stat_write_calls, 1);

    if (count < zerocopy_threshold || !is_regular_file(fd))
        return real_pwrite(fd, buf, count, offset);

    return uring_file_write(fd, buf, count, offset);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    ensure_init();
    atomic_fetch_add(&stat_send_calls, 1);

    if (len < zerocopy_threshold)
        return real_send(sockfd, buf, len, flags);

    return uring_send(sockfd, buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen)
{
    struct iovec iov;
    struct msghdr msg;

    ensure_init();
    atomic_fetch_add(&stat_send_calls, 1);

    if (len < zerocopy_threshold)
        return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)buf;
    iov.iov_len = len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_name = (void *)dest_addr;
    msg.msg_namelen = addrlen;

    return uring_sendmsg_internal(sockfd, &msg, flags, real_sendmsg);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    ensure_init();
    atomic_fetch_add(&stat_recv_calls, 1);

    if (len < zerocopy_threshold)
        return real_recv(sockfd, buf, len, flags);

    return uring_recv(sockfd, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    struct iovec iov;
    struct msghdr msg;
    ssize_t res;

    ensure_init();
    atomic_fetch_add(&stat_recv_calls, 1);

    if (len < zerocopy_threshold)
        return real_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_name = src_addr;
    msg.msg_namelen = addrlen ? *addrlen : 0;

    res = uring_recvmsg_internal(sockfd, &msg, flags, real_recvmsg);
    if (res >= 0 && addrlen)
        *addrlen = msg.msg_namelen;
    return res;
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
    ensure_init();
    atomic_fetch_add(&stat_send_calls, 1);

    if (iov_total_size(msg->msg_iov, (int)msg->msg_iovlen) < zerocopy_threshold)
        return real_sendmsg(sockfd, msg, flags);

    return uring_sendmsg_internal(sockfd, msg, flags, real_sendmsg);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    ensure_init();
    atomic_fetch_add(&stat_recv_calls, 1);

    if (iov_total_size(msg->msg_iov, (int)msg->msg_iovlen) < zerocopy_threshold)
        return real_recvmsg(sockfd, msg, flags);

    return uring_recvmsg_internal(sockfd, msg, flags, real_recvmsg);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    struct io_uring *ring;
    struct io_uring_sqe *sqe;
    ssize_t res;

    ensure_init();
    atomic_fetch_add(&stat_read_calls, 1);

    if (iov_total_size(iov, iovcnt) < zerocopy_threshold)
        return real_readv(fd, iov, iovcnt);

    if (!is_regular_file(fd))
        return real_readv(fd, iov, iovcnt);

    ring = get_thread_ring();
    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_readv(fd, iov, iovcnt);
    }

    sqe = get_sqe_with_retry(ring);
    if (!sqe) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_readv(fd, iov, iovcnt);
    }

    io_uring_prep_readv(sqe, fd, iov, iovcnt, -1);
    maybe_mark_async(sqe);

    res = submit_and_wait(ring);
    if (res >= 0)
        DEBUG_LOG("readv: %zd bytes via io_uring", res);

    return res;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    struct io_uring *ring;
    struct io_uring_sqe *sqe;
    ssize_t res;

    ensure_init();
    atomic_fetch_add(&stat_write_calls, 1);

    if (iov_total_size(iov, iovcnt) < zerocopy_threshold)
        return real_writev(fd, iov, iovcnt);

    if (!is_regular_file(fd))
        return real_writev(fd, iov, iovcnt);

    ring = get_thread_ring();
    if (!ring) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_writev(fd, iov, iovcnt);
    }

    sqe = get_sqe_with_retry(ring);
    if (!sqe) {
        atomic_fetch_add(&stat_fallback_calls, 1);
        return real_writev(fd, iov, iovcnt);
    }

    io_uring_prep_writev(sqe, fd, iov, iovcnt, -1);
    maybe_mark_async(sqe);

    res = submit_and_wait(ring);
    if (res >= 0)
        DEBUG_LOG("writev: %zd bytes via io_uring", res);

    return res;
}

ssize_t zerocopy_uring_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
    ensure_init();
    return uring_sendfile(out_fd, in_fd, offset, count);
}

int zerocopy_enable_socket(int sockfd)
{
    int enable = 1;
    return setsockopt(sockfd, SOL_SOCKET, SO_ZEROCOPY, &enable, sizeof(enable));
}

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

void zerocopy_print_stats(void)
{
    unsigned long total_optimized = atomic_load(&stat_uring_send_used) +
                                    atomic_load(&stat_uring_recv_used) +
                                    atomic_load(&stat_uring_sendfile_used);
    unsigned long total_path_calls = atomic_load(&stat_send_calls) +
                                     atomic_load(&stat_recv_calls);

    fprintf(stderr, "[zerocopy_v2] io_uring Statistics:\n");
    fprintf(stderr, "  read-like calls:       %lu\n", atomic_load(&stat_read_calls));
    fprintf(stderr, "  write-like calls:      %lu\n", atomic_load(&stat_write_calls));
    fprintf(stderr, "  send-path calls:       %lu\n", atomic_load(&stat_send_calls));
    fprintf(stderr, "  recv-path calls:       %lu\n", atomic_load(&stat_recv_calls));
    fprintf(stderr, "  io_uring sends:        %lu times, %lu bytes\n",
            atomic_load(&stat_uring_send_used), atomic_load(&stat_uring_bytes_sent));
    fprintf(stderr, "  io_uring recvs:        %lu times, %lu bytes\n",
            atomic_load(&stat_uring_recv_used), atomic_load(&stat_uring_bytes_recv));
    fprintf(stderr, "  io_uring sendfiles:    %lu times\n",
            atomic_load(&stat_uring_sendfile_used));
    fprintf(stderr, "  Fallback to syscalls:  %lu\n", atomic_load(&stat_fallback_calls));

    if (total_path_calls > 0) {
        double efficiency = (double)total_optimized / total_path_calls * 100.0;
        fprintf(stderr, "  io_uring efficiency:   %.1f%%\n", efficiency);
    }
}

__attribute__((destructor))
static void fini_zerocopy(void)
{
    if (debug_enabled)
        zerocopy_print_stats();

    if (thread_ring_initialized && thread_ring) {
        pthread_setspecific(thread_ring_key, NULL);
        destroy_thread_ring(thread_ring);
    }
}

__attribute__((constructor))
static void early_init(void)
{
    init_zerocopy();
}
