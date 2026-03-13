/*
 * Test program for ClickHouse Zero-Copy Library v2 (io_uring)
 *
 * Usage:
 *   LD_PRELOAD=./libclickhouse_zerocopy_v2.so ZEROCOPY_DEBUG=1 ./test_zerocopy_v2
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>

/* External functions from zerocopy_v2 library */
extern ssize_t zerocopy_uring_sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
extern int zerocopy_enable_socket(int sockfd);
extern void zerocopy_print_stats(void);

#define TEST_FILE "/tmp/zerocopy_v2_test.dat"
#define TEST_SIZE (50 * 1024 * 1024)  /* 50 MB */
#define TEST_PORT 29876

static double get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static ssize_t raw_write_all(int fd, const void *buf, size_t len)
{
    const char *ptr = buf;
    size_t written = 0;

    while (written < len) {
        ssize_t n = syscall(SYS_write, fd, ptr + written, len - written);
        if (n <= 0)
            return n;
        written += (size_t)n;
    }

    return (ssize_t)written;
}

static int create_tcp_pair(int fds[2])
{
    int listener = -1;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto fail;
    if (listen(listener, 1) < 0)
        goto fail;
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) < 0)
        goto fail;

    fds[0] = socket(AF_INET, SOCK_STREAM, 0);
    if (fds[0] < 0)
        goto fail;
    if (connect(fds[0], (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto fail_client;

    fds[1] = accept(listener, NULL, NULL);
    if (fds[1] < 0)
        goto fail_client;

    close(listener);
    return 0;

fail_client:
    close(fds[0]);
    fds[0] = -1;
fail:
    close(listener);
    return -1;
}

void create_test_file(void)
{
    printf("Creating test file: %s (%d MB)...\n", TEST_FILE, TEST_SIZE / (1024*1024));

    int fd = open(TEST_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    char buf[4096];
    memset(buf, 'A', sizeof(buf));

    for (size_t i = 0; i < TEST_SIZE / sizeof(buf); i++) {
        if (raw_write_all(fd, buf, sizeof(buf)) != sizeof(buf)) {
            perror("write");
            exit(1);
        }
    }

    close(fd);
    printf("✓ Test file created\n");
}

void test_large_send_recv(void)
{
    printf("\n=== Test 1: Large send/recv via io_uring ===\n");
    fflush(NULL);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    /* Enable zero-copy on socket */
    zerocopy_enable_socket(sv[0]);
    zerocopy_enable_socket(sv[1]);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: receive data */
        close(sv[0]);

        char *buf = malloc(16 * 1024 * 1024);
        size_t total = 0;
        double start = get_time_ms();

        while (total < TEST_SIZE) {
            ssize_t n = recv(sv[1], buf, 16 * 1024 * 1024, 0);
            if (n <= 0) break;
            total += n;
        }

        double elapsed = get_time_ms() - start;
        double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

        printf("Child received: %zu bytes in %.2f ms (%.2f MB/s)\n",
               total, elapsed, throughput);

        free(buf);
        close(sv[1]);
        fflush(stdout);
        _exit(0);
    }

    /* Parent: send data */
    close(sv[1]);

    char *buf = malloc(16 * 1024 * 1024);
    memset(buf, 'B', 16 * 1024 * 1024);

    size_t total = 0;
    double start = get_time_ms();

    while (total < TEST_SIZE) {
        size_t to_send = (TEST_SIZE - total) < (16 * 1024 * 1024) ?
                         (TEST_SIZE - total) : (16 * 1024 * 1024);
        ssize_t n = send(sv[0], buf, to_send, 0);
        if (n <= 0) {
            perror("send");
            break;
        }
        total += n;
    }

    double elapsed = get_time_ms() - start;
    double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("Parent sent: %zu bytes in %.2f ms (%.2f MB/s)\n",
           total, elapsed, throughput);

    free(buf);
    close(sv[0]);
    wait(NULL);
}

void test_file_to_socket(void)
{
    printf("\n=== Test 2: File to socket via io_uring ===\n");
    fflush(stdout);

    int sv[2] = {-1, -1};
    if (create_tcp_pair(sv) < 0) {
        perror("create_tcp_pair");
        return;
    }

    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        perror("open");
        close(sv[0]);
        close(sv[1]);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: receive data */
        close(sv[0]);
        close(fd);

        char buf[65536];
        size_t total = 0;
        double start = get_time_ms();

        while (1) {
            ssize_t n = read(sv[1], buf, sizeof(buf));
            if (n <= 0) break;
            total += n;
        }

        double elapsed = get_time_ms() - start;
        double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

        printf("Child received: %zu bytes in %.2f ms (%.2f MB/s)\n",
               total, elapsed, throughput);

        close(sv[1]);
        fflush(stdout);
        _exit(0);
    }

    /* Parent: send file via zerocopy_uring_sendfile */
    close(sv[1]);

    double start = get_time_ms();
    off_t offset = 0;
    ssize_t sent = zerocopy_uring_sendfile(sv[0], fd, &offset, TEST_SIZE);

    double elapsed = get_time_ms() - start;

    if (sent > 0) {
        double throughput = (sent / (1024.0 * 1024.0)) / (elapsed / 1000.0);
        printf("Parent sent: %zd bytes in %.2f ms (%.2f MB/s) via io_uring\n",
               sent, elapsed, throughput);
    } else {
        perror("zerocopy_uring_sendfile");
    }

    close(sv[0]);
    close(fd);
    wait(NULL);
}

void test_multiple_small_ops(void)
{
    printf("\n=== Test 3: Multiple small operations (should use regular syscalls) ===\n");
    fflush(NULL);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: receive */
        close(sv[0]);

        char buf[256];
        int count = 0;

        while (count < 1000) {
            ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
            if (n <= 0) break;
            count++;
        }

        printf("Child received %d small messages\n", count);
        close(sv[1]);
        fflush(stdout);
        _exit(0);
    }

    /* Parent: send many small messages */
    close(sv[1]);

    char buf[256];
    memset(buf, 'C', sizeof(buf));

    double start = get_time_ms();

    for (int i = 0; i < 1000; i++) {
        if (send(sv[0], buf, sizeof(buf), 0) <= 0) {
            perror("send");
            break;
        }
    }

    double elapsed = get_time_ms() - start;
    printf("Parent sent 1000 messages in %.2f ms\n", elapsed);

    close(sv[0]);
    wait(NULL);
}

void test_readv_writev(void)
{
    printf("\n=== Test 4: readv/writev via io_uring ===\n");
    fflush(NULL);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    #define IOV_COUNT 16
    #define IOV_SIZE (1024 * 1024)

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: receive using readv */
        close(sv[0]);

        struct iovec iov[IOV_COUNT];
        for (int i = 0; i < IOV_COUNT; i++) {
            iov[i].iov_base = malloc(IOV_SIZE);
            iov[i].iov_len = IOV_SIZE;
        }

        double start = get_time_ms();
        ssize_t n = readv(sv[1], iov, IOV_COUNT);
        double elapsed = get_time_ms() - start;

        if (n > 0) {
            double throughput = (n / (1024.0 * 1024.0)) / (elapsed / 1000.0);
            printf("Child readv: %zd bytes in %.2f ms (%.2f MB/s)\n",
                   n, elapsed, throughput);
        } else if (n < 0) {
            perror("readv");
        } else {
            printf("Child readv returned 0 bytes\n");
        }

        for (int i = 0; i < IOV_COUNT; i++) {
            free(iov[i].iov_base);
        }

        close(sv[1]);
        fflush(stdout);
        _exit(0);
    }

    /* Parent: send using writev */
    close(sv[1]);

    struct iovec iov[IOV_COUNT];
    for (int i = 0; i < IOV_COUNT; i++) {
        iov[i].iov_base = malloc(IOV_SIZE);
        memset(iov[i].iov_base, 'D', IOV_SIZE);
        iov[i].iov_len = IOV_SIZE;
    }

    double start = get_time_ms();
    ssize_t n = writev(sv[0], iov, IOV_COUNT);
    double elapsed = get_time_ms() - start;

    if (n > 0) {
        double throughput = (n / (1024.0 * 1024.0)) / (elapsed / 1000.0);
        printf("Parent writev: %zd bytes in %.2f ms (%.2f MB/s)\n",
               n, elapsed, throughput);
    } else if (n < 0) {
        perror("writev");
    } else {
        printf("Parent writev returned 0 bytes\n");
    }

    for (int i = 0; i < IOV_COUNT; i++) {
        free(iov[i].iov_base);
    }

    close(sv[0]);
    wait(NULL);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("========================================\n");
    printf("ClickHouse Zero-Copy Library v2 Test\n");
    printf("io_uring Edition\n");
    printf("========================================\n\n");

    /* Check if LD_PRELOAD is active */
    const char *preload = getenv("LD_PRELOAD");
    if (!preload || !strstr(preload, "zerocopy")) {
        printf("⚠ WARNING: LD_PRELOAD not detected!\n");
        printf("Run with: LD_PRELOAD=./libclickhouse_zerocopy_v2.so %s\n\n", "test_zerocopy_v2");
    }

    /* Check configuration */
    const char *debug = getenv("ZEROCOPY_DEBUG");
    const char *threshold = getenv("ZEROCOPY_THRESHOLD");
    const char *queue_depth = getenv("ZEROCOPY_QUEUE_DEPTH");

    printf("Configuration:\n");
    printf("  Debug:       %s\n", debug ? debug : "0");
    printf("  Threshold:   %s bytes\n", threshold ? threshold : "8192");
    printf("  Queue depth: %s\n", queue_depth ? queue_depth : "128");
    printf("\n");

    create_test_file();

    test_large_send_recv();
    test_file_to_socket();
    test_multiple_small_ops();
    test_readv_writev();

    printf("\n=== Final Statistics ===\n");
    zerocopy_print_stats();

    /* Cleanup */
    unlink(TEST_FILE);

    printf("\n✓ All tests completed!\n");
    return 0;
}
