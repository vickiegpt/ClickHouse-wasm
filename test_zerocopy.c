/*
 * Test program for ClickHouse Zero-Copy Library
 *
 * Usage:
 *   LD_PRELOAD=./libclickhouse_zerocopy.so ZEROCOPY_DEBUG=1 ./test_zerocopy
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <errno.h>

/* External functions from zerocopy library */
extern ssize_t zerocopy_sendfile(int out_sockfd, int in_fd, off_t *offset, size_t count);
extern ssize_t zerocopy_splice_to_socket(int in_fd, int out_sockfd, size_t len);
extern ssize_t zerocopy_socket_to_socket(int in_sockfd, int out_sockfd, size_t len);
extern int zerocopy_enable_socket(int sockfd);
extern void zerocopy_print_stats(void);

#define TEST_FILE "/tmp/zerocopy_test.dat"
#define TEST_SIZE (10 * 1024 * 1024)  /* 10 MB */
#define TEST_PORT 19876

void create_test_file(void)
{
    int fd = open(TEST_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    char buf[4096];
    memset(buf, 'A', sizeof(buf));

    for (size_t i = 0; i < TEST_SIZE / sizeof(buf); i++) {
        if (write(fd, buf, sizeof(buf)) != sizeof(buf)) {
            perror("write");
            exit(1);
        }
    }

    close(fd);
    printf("Created test file: %s (%d MB)\n", TEST_FILE, TEST_SIZE / (1024*1024));
}

void test_sendfile(void)
{
    printf("\n=== Test 1: sendfile() ===\n");

    /* Create socket pair */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    /* Open test file */
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

        char buf[4096];
        size_t total = 0;
        while (1) {
            ssize_t n = read(sv[1], buf, sizeof(buf));
            if (n <= 0) break;
            total += n;
        }

        printf("Child received: %zu bytes\n", total);
        close(sv[1]);
        exit(0);
    }

    /* Parent: send using sendfile */
    close(sv[1]);

    off_t offset = 0;
    ssize_t sent = zerocopy_sendfile(sv[0], fd, &offset, TEST_SIZE);

    printf("sendfile() sent: %zd bytes\n", sent);

    close(sv[0]);
    close(fd);

    wait(NULL);
}

void test_splice(void)
{
    printf("\n=== Test 2: splice() ===\n");

    /* Create pipe */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return;
    }

    /* Create socket pair */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    /* Open test file */
    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        perror("open");
        close(pipefd[0]);
        close(pipefd[1]);
        close(sv[0]);
        close(sv[1]);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: receive data */
        close(sv[0]);
        close(fd);
        close(pipefd[0]);
        close(pipefd[1]);

        char buf[4096];
        size_t total = 0;
        while (1) {
            ssize_t n = read(sv[1], buf, sizeof(buf));
            if (n <= 0) break;
            total += n;
        }

        printf("Child received: %zu bytes\n", total);
        close(sv[1]);
        exit(0);
    }

    /* Parent: send using splice */
    close(sv[1]);

    ssize_t sent = zerocopy_splice_to_socket(fd, sv[0], TEST_SIZE);

    printf("splice() sent: %zd bytes\n", sent);

    close(sv[0]);
    close(fd);
    close(pipefd[0]);
    close(pipefd[1]);

    wait(NULL);
}

void test_regular_io(void)
{
    printf("\n=== Test 3: Regular I/O (with LD_PRELOAD) ===\n");

    /* Create socket pair */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    /* Enable zerocopy on socket */
    zerocopy_enable_socket(sv[0]);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: receive data */
        close(sv[0]);

        char buf[4096];
        size_t total = 0;
        while (1) {
            ssize_t n = read(sv[1], buf, sizeof(buf));
            if (n <= 0) break;
            total += n;
        }

        printf("Child received: %zu bytes\n", total);
        close(sv[1]);
        exit(0);
    }

    /* Parent: send using regular send() */
    close(sv[1]);

    char buf[4096];
    memset(buf, 'B', sizeof(buf));

    size_t total = 0;
    for (int i = 0; i < 1000; i++) {
        ssize_t n = send(sv[0], buf, sizeof(buf), 0);
        if (n > 0)
            total += n;
    }

    printf("Regular send() sent: %zu bytes\n", total);

    close(sv[0]);
    wait(NULL);
}

int main(void)
{
    printf("ClickHouse Zero-Copy Library Test\n");
    printf("==================================\n");

    /* Check if LD_PRELOAD is active */
    const char *preload = getenv("LD_PRELOAD");
    if (!preload || !strstr(preload, "zerocopy")) {
        printf("WARNING: LD_PRELOAD not detected!\n");
        printf("Run with: LD_PRELOAD=./libclickhouse_zerocopy.so %s\n", "test_zerocopy");
    }

    create_test_file();

    test_sendfile();
    test_splice();
    test_regular_io();

    printf("\n=== Statistics ===\n");
    zerocopy_print_stats();

    /* Cleanup */
    unlink(TEST_FILE);

    printf("\nAll tests completed!\n");
    return 0;
}
