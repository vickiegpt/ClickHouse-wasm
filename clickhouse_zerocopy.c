/*
 * ClickHouse Zero-Copy LD_PRELOAD Library
 *
 * Intercepts read() and send() syscalls and replaces them with
 * splice() and sendfile() for zero-copy data transfer optimization.
 *
 * Usage:
 *   LD_PRELOAD=/path/to/libclickhouse_zerocopy.so clickhouse-server
 *
 * Environment variables:
 *   ZEROCOPY_DEBUG=1      - Enable debug logging
 *   ZEROCOPY_THRESHOLD=N  - Minimum bytes to use zero-copy (default: 4096)
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
#include <sys/sendfile.h>
#include <sys/uio.h>
#include <linux/fs.h>
#include <stdatomic.h>
#include <pthread.h>

/* Define SO_ZEROCOPY if not available (kernel 4.14+) */
#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

/* Original function pointers */
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;
static ssize_t (*real_write)(int fd, const void *buf, size_t count) = NULL;
static ssize_t (*real_send)(int sockfd, const void *buf, size_t len, int flags) = NULL;
static ssize_t (*real_recv)(int sockfd, void *buf, size_t len, int flags) = NULL;
static ssize_t (*real_sendto)(int sockfd, const void *buf, size_t len, int flags,
                              const struct sockaddr *dest_addr, socklen_t addrlen) = NULL;
static ssize_t (*real_recvfrom)(int sockfd, void *buf, size_t len, int flags,
                                struct sockaddr *src_addr, socklen_t *addrlen) = NULL;

/* Configuration */
static int debug_enabled = 0;
static size_t zerocopy_threshold = 4096;
static int initialized = 0;
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Statistics */
static atomic_ulong stat_read_calls = 0;
static atomic_ulong stat_send_calls = 0;
static atomic_ulong stat_splice_used = 0;
static atomic_ulong stat_sendfile_used = 0;
static atomic_ulong stat_splice_bytes = 0;
static atomic_ulong stat_sendfile_bytes = 0;

/* Per-thread pipe cache for splice operations */
#define MAX_CACHED_PIPES 16
static __thread int pipe_cache[MAX_CACHED_PIPES][2];
static __thread int pipe_cache_count = 0;

#define DEBUG_LOG(fmt, ...) \
    do { if (debug_enabled) fprintf(stderr, "[zerocopy] " fmt "\n", ##__VA_ARGS__); } while(0)

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

/* Check if fd is a pipe */
static inline int is_pipe(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    return S_ISFIFO(st.st_mode);
}

/* Get or create a cached pipe for splice operations */
static int get_splice_pipe(int pipefd[2])
{
    if (pipe_cache_count > 0) {
        pipe_cache_count--;
        pipefd[0] = pipe_cache[pipe_cache_count][0];
        pipefd[1] = pipe_cache[pipe_cache_count][1];
        return 0;
    }

    if (pipe2(pipefd, O_NONBLOCK) < 0)
        return -1;

    /* Set pipe size for better performance */
    fcntl(pipefd[0], F_SETPIPE_SZ, 1048576);

    return 0;
}

/* Return pipe to cache or close it */
static void put_splice_pipe(int pipefd[2])
{
    if (pipe_cache_count < MAX_CACHED_PIPES) {
        pipe_cache[pipe_cache_count][0] = pipefd[0];
        pipe_cache[pipe_cache_count][1] = pipefd[1];
        pipe_cache_count++;
    } else {
        close(pipefd[0]);
        close(pipefd[1]);
    }
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
    real_sendto = dlsym(RTLD_NEXT, "sendto");
    real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");

    if (!real_read || !real_write || !real_send) {
        fprintf(stderr, "[zerocopy] ERROR: Failed to load original functions\n");
        abort();
    }

    /* Read configuration from environment */
    const char *env_debug = getenv("ZEROCOPY_DEBUG");
    if (env_debug && atoi(env_debug))
        debug_enabled = 1;

    const char *env_threshold = getenv("ZEROCOPY_THRESHOLD");
    if (env_threshold)
        zerocopy_threshold = (size_t)atol(env_threshold);

    DEBUG_LOG("Initialized: threshold=%zu bytes", zerocopy_threshold);

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
 * Optimized send using splice for pipe-backed buffers or MSG_ZEROCOPY.
 */
ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    ensure_init();
    atomic_fetch_add(&stat_send_calls, 1);

    /* For small transfers, use regular send */
    if (len < zerocopy_threshold) {
        return real_send(sockfd, buf, len, flags);
    }

    /*
     * Try MSG_ZEROCOPY for large sends (Linux 4.14+)
     * This avoids kernel copy for large buffers.
     * Note: Requires SO_ZEROCOPY socket option to be set.
     */
    int so_zerocopy = 0;
    socklen_t optlen = sizeof(so_zerocopy);

    if (getsockopt(sockfd, SOL_SOCKET, SO_ZEROCOPY, &so_zerocopy, &optlen) == 0
        && so_zerocopy) {
        ssize_t ret = real_send(sockfd, buf, len, flags | MSG_ZEROCOPY);
        if (ret >= 0) {
            DEBUG_LOG("send: used MSG_ZEROCOPY for %zd bytes", ret);
            return ret;
        }
        /* Fall through on error */
    }

    return real_send(sockfd, buf, len, flags);
}

/*
 * Optimized sendto
 */
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen)
{
    ensure_init();

    if (!dest_addr) {
        /* No destination address means this is like send() */
        return send(sockfd, buf, len, flags);
    }

    return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

/*
 * Optimized read - tracks file descriptors for potential splice optimization
 */
ssize_t read(int fd, void *buf, size_t count)
{
    ensure_init();
    atomic_fetch_add(&stat_read_calls, 1);

    return real_read(fd, buf, count);
}

/*
 * Optimized recv
 */
ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    ensure_init();

    return real_recv(sockfd, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    ensure_init();

    return real_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

/*
 * Optimized write - use splice for socket writes when possible
 */
ssize_t write(int fd, const void *buf, size_t count)
{
    ensure_init();

    if (count < zerocopy_threshold) {
        return real_write(fd, buf, count);
    }

    /* For sockets, try zerocopy send */
    if (is_socket(fd)) {
        return send(fd, buf, count, 0);
    }

    return real_write(fd, buf, count);
}

/*
 * Direct splice wrapper for use by ClickHouse if it detects this library
 */
ssize_t zerocopy_splice_to_socket(int in_fd, int out_sockfd, size_t len)
{
    ensure_init();

    ssize_t total = 0;
    int pipefd[2];

    if (get_splice_pipe(pipefd) < 0) {
        return -1;
    }

    while (len > 0) {
        /* Splice from input fd to pipe */
        ssize_t spliced = splice(in_fd, NULL, pipefd[1], NULL, len,
                                  SPLICE_F_MOVE | SPLICE_F_MORE);
        if (spliced <= 0) {
            if (spliced == 0)
                break;
            if (errno == EINTR)
                continue;
            put_splice_pipe(pipefd);
            return total > 0 ? total : -1;
        }

        /* Splice from pipe to socket */
        ssize_t sent = 0;
        while (sent < spliced) {
            ssize_t n = splice(pipefd[0], NULL, out_sockfd, NULL, spliced - sent,
                               SPLICE_F_MOVE | SPLICE_F_MORE);
            if (n <= 0) {
                if (n == 0)
                    break;
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Socket buffer full, wait */
                    continue;
                }
                put_splice_pipe(pipefd);
                return total > 0 ? total : -1;
            }
            sent += n;
        }

        total += sent;
        len -= spliced;

        atomic_fetch_add(&stat_splice_bytes, sent);
    }

    put_splice_pipe(pipefd);
    atomic_fetch_add(&stat_splice_used, 1);

    DEBUG_LOG("splice_to_socket: transferred %zd bytes", total);
    return total;
}

/*
 * Direct sendfile wrapper
 */
ssize_t zerocopy_sendfile(int out_sockfd, int in_fd, off_t *offset, size_t count)
{
    ensure_init();

    ssize_t total = 0;
    off_t off = offset ? *offset : 0;

    while (count > 0) {
        ssize_t sent = sendfile(out_sockfd, in_fd, &off, count);
        if (sent <= 0) {
            if (sent == 0)
                break;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (total > 0)
                    break;
                return -1;
            }
            return total > 0 ? total : -1;
        }

        total += sent;
        count -= sent;

        atomic_fetch_add(&stat_sendfile_bytes, sent);
    }

    if (offset)
        *offset = off;

    atomic_fetch_add(&stat_sendfile_used, 1);
    DEBUG_LOG("sendfile: transferred %zd bytes", total);

    return total;
}

/*
 * Copy data from one socket to another using splice (zero-copy)
 * Useful for proxy scenarios
 */
ssize_t zerocopy_socket_to_socket(int in_sockfd, int out_sockfd, size_t len)
{
    ensure_init();

    ssize_t total = 0;
    int pipefd[2];

    if (get_splice_pipe(pipefd) < 0) {
        return -1;
    }

    while (len > 0) {
        /* Splice from input socket to pipe */
        ssize_t spliced = splice(in_sockfd, NULL, pipefd[1], NULL, len,
                                  SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
        if (spliced <= 0) {
            if (spliced == 0)
                break;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (total > 0)
                    break;
                put_splice_pipe(pipefd);
                errno = EAGAIN;
                return -1;
            }
            put_splice_pipe(pipefd);
            return total > 0 ? total : -1;
        }

        /* Splice from pipe to output socket */
        ssize_t sent = 0;
        while (sent < spliced) {
            ssize_t n = splice(pipefd[0], NULL, out_sockfd, NULL, spliced - sent,
                               SPLICE_F_MOVE | SPLICE_F_MORE);
            if (n <= 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                put_splice_pipe(pipefd);
                return total > 0 ? total : -1;
            }
            sent += n;
        }

        total += sent;
        len -= spliced;
    }

    put_splice_pipe(pipefd);
    atomic_fetch_add(&stat_splice_used, 1);
    atomic_fetch_add(&stat_splice_bytes, total);

    DEBUG_LOG("socket_to_socket: transferred %zd bytes via splice", total);
    return total;
}

/*
 * Enable SO_ZEROCOPY on a socket for optimized sends
 */
int zerocopy_enable_socket(int sockfd)
{
    int enable = 1;
    return setsockopt(sockfd, SOL_SOCKET, SO_ZEROCOPY, &enable, sizeof(enable));
}

/*
 * Get statistics
 */
void zerocopy_get_stats(unsigned long *read_calls, unsigned long *send_calls,
                        unsigned long *splice_used, unsigned long *sendfile_used,
                        unsigned long *splice_bytes, unsigned long *sendfile_bytes)
{
    if (read_calls) *read_calls = atomic_load(&stat_read_calls);
    if (send_calls) *send_calls = atomic_load(&stat_send_calls);
    if (splice_used) *splice_used = atomic_load(&stat_splice_used);
    if (sendfile_used) *sendfile_used = atomic_load(&stat_sendfile_used);
    if (splice_bytes) *splice_bytes = atomic_load(&stat_splice_bytes);
    if (sendfile_bytes) *sendfile_bytes = atomic_load(&stat_sendfile_bytes);
}

/*
 * Print statistics to stderr
 */
void zerocopy_print_stats(void)
{
    fprintf(stderr, "[zerocopy] Statistics:\n");
    fprintf(stderr, "  read() calls:     %lu\n", atomic_load(&stat_read_calls));
    fprintf(stderr, "  send() calls:     %lu\n", atomic_load(&stat_send_calls));
    fprintf(stderr, "  splice() used:    %lu times, %lu bytes\n",
            atomic_load(&stat_splice_used), atomic_load(&stat_splice_bytes));
    fprintf(stderr, "  sendfile() used:  %lu times, %lu bytes\n",
            atomic_load(&stat_sendfile_used), atomic_load(&stat_sendfile_bytes));
}

/* Destructor to print stats on unload */
__attribute__((destructor))
static void fini_zerocopy(void)
{
    if (debug_enabled) {
        zerocopy_print_stats();
    }

    /* Clean up cached pipes */
    while (pipe_cache_count > 0) {
        pipe_cache_count--;
        close(pipe_cache[pipe_cache_count][0]);
        close(pipe_cache[pipe_cache_count][1]);
    }
}

/* Constructor */
__attribute__((constructor))
static void early_init(void)
{
    init_zerocopy();
}
