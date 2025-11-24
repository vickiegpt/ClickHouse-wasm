/*
 * Benchmark for bypass performance testing
 * Measures throughput with different I/O patterns
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define BENCH_SIZE (200 * 1024 * 1024)  /* 200 MB */
#define TEST_FILE "/tmp/bench_bypass.dat"

static double get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

void create_bench_file(void)
{
    int fd = open(TEST_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("create bench file");
        exit(1);
    }

    char buf[4096];
    memset(buf, 'X', sizeof(buf));

    for (size_t i = 0; i < BENCH_SIZE / sizeof(buf); i++) {
        write(fd, buf, sizeof(buf));
    }

    close(fd);
}

void bench_sequential_read(const char *name, size_t chunk_size)
{
    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return;
    }

    char *buf = malloc(chunk_size);
    size_t total = 0;
    double start = get_time_ms();

    while (total < BENCH_SIZE) {
        ssize_t n = read(fd, buf, chunk_size);
        if (n <= 0) break;
        total += n;
    }

    double elapsed = get_time_ms() - start;
    double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("  %s: %.2f MB/s (%.2f ms, %zu KB chunks)\n",
           name, throughput, elapsed, chunk_size / 1024);

    free(buf);
    close(fd);
}

void bench_random_read(const char *name)
{
    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return;
    }

    char *buf = malloc(1024 * 1024);
    size_t total = 0;
    double start = get_time_ms();

    /* Simulate random access */
    for (int i = 0; i < 100; i++) {
        off_t offset = (rand() % (BENCH_SIZE / (1024*1024))) * 1024 * 1024;
        ssize_t n = pread(fd, buf, 1024 * 1024, offset);
        if (n > 0)
            total += n;
    }

    double elapsed = get_time_ms() - start;
    double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("  %s: %.2f MB/s (%.2f ms, random access)\n",
           name, throughput, elapsed);

    free(buf);
    close(fd);
}

void bench_socket_throughput(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: receiver */
        close(sv[0]);

        char buf[65536];
        size_t total = 0;

        while (total < BENCH_SIZE) {
            ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
            if (n <= 0) break;
            total += n;
        }

        close(sv[1]);
        exit(0);
    }

    /* Parent: sender */
    close(sv[1]);

    char *buf = malloc(1024 * 1024);
    memset(buf, 'Y', 1024 * 1024);

    size_t total = 0;
    double start = get_time_ms();

    while (total < BENCH_SIZE) {
        ssize_t n = send(sv[0], buf, 1024 * 1024, 0);
        if (n <= 0) break;
        total += n;
    }

    double elapsed = get_time_ms() - start;
    double throughput = (total / (1024.0 * 1024.0)) / (elapsed / 1000.0);

    printf("  Socket transfer: %.2f MB/s (%.2f ms)\n", throughput, elapsed);

    free(buf);
    close(sv[0]);
    wait(NULL);
}

int main(void)
{
    const char *preload = getenv("LD_PRELOAD");

    if (preload) {
        if (strstr(preload, "zerocopy_v3"))
            printf("Mode: BPF Bypass + DMA (v3)\n");
        else if (strstr(preload, "zerocopy_v2"))
            printf("Mode: io_uring (v2)\n");
        else if (strstr(preload, "zerocopy"))
            printf("Mode: splice/sendfile (v1)\n");
        else
            printf("Mode: Unknown LD_PRELOAD\n");
    } else {
        printf("Mode: Standard syscalls\n");
    }

    printf("Benchmark size: %d MB\n\n", BENCH_SIZE / (1024 * 1024));

    /* Create test file if needed */
    if (access(TEST_FILE, F_OK) != 0) {
        printf("Creating benchmark file...\n");
        create_bench_file();
    }

    printf("Sequential Read Tests:\n");
    bench_sequential_read("  4KB chunks  ", 4 * 1024);
    bench_sequential_read(" 64KB chunks  ", 64 * 1024);
    bench_sequential_read("  1MB chunks  ", 1024 * 1024);
    bench_sequential_read("  4MB chunks  ", 4 * 1024 * 1024);

    printf("\nRandom Access Test:\n");
    bench_random_read("Random read");

    printf("\nSocket Test:\n");
    bench_socket_throughput();

    printf("\n");

    return 0;
}
