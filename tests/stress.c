#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SNAP "/proc/proc_lens/snapshot"
#define CAP (8 * 1024 * 1024)

static char *read_all(int fd, size_t chunk) {
    char *buf = calloc(1, CAP);
    size_t used = 0;
    ssize_t n;
    assert(buf);
    while ((n = read(fd, buf + used, chunk < CAP - used - 1 ? chunk : CAP - used - 1)) > 0) {
        used += n;
        assert(used < CAP - 1);
    }
    assert(n == 0);
    return buf;
}

static unsigned long long check(const char *text) {
    unsigned long long gen;
    unsigned count, truncated;
    assert(sscanf(text, "# proc-lens v1 generation=%llu count=%u truncated=%u", &gen, &count, &truncated) == 3);
    assert(gen > 0 && count <= 256 && truncated <= 1);
    unsigned rows = 0;
    for (const char *p = text; *p; p++) if (*p == '\n') rows++;
    assert(rows == count + 2);
    return gen;
}

static void *reader(void *unused) {
    for (int i = 0; i < 100; i++) {
        int fd = open(SNAP, O_RDONLY);
        assert(fd >= 0);
        char *first = read_all(fd, i % 2 ? 17 : 4096);
        check(first);
        assert(lseek(fd, 0, SEEK_SET) == 0);
        char *again = read_all(fd, 113);
        assert(strcmp(first, again) == 0);
        free(first); free(again); close(fd);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "hold") == 0) {
        int fd = open(SNAP, O_RDONLY);
        assert(fd >= 0);
        puts("HOLD_READY"); fflush(stdout);
        for (int i = 0; i < 500; i++) {
            char buf[64];
            if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, buf, sizeof(buf)) < 0) {
                /* proc_reg_lseek returns EINVAL after proc_remove drains the PDE. */
                assert(errno == EIO || errno == ENOENT || errno == EINVAL);
                close(fd); puts("PASS: open descriptor during unload"); return 0;
            }
            usleep(10000);
        }
        return 1;
    }
    int fd = open(SNAP, O_RDONLY);
    assert(fd >= 0);
    char *first = read_all(fd, 1);
    unsigned long long first_gen = check(first);
    usleep(150000);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    char *same = read_all(fd, 17);
    assert(strcmp(first, same) == 0);
    free(first); free(same); close(fd);
    fd = open(SNAP, O_RDONLY);
    assert(fd >= 0);
    first = read_all(fd, 4096);
    assert(check(first) > first_gen);
    free(first); close(fd);

    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(setuid(65534) == 0);
        errno = 0; fd = open(SNAP, O_RDONLY);
        _exit(fd < 0 && (errno == EACCES || errno == EPERM) ? 0 : 1);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, reader, NULL) == 0);
    for (int i = 0; i < 200; i++) {
        child = fork(); assert(child >= 0);
        if (!child) execl("/bin/true", "true", NULL), _exit(127);
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    puts("PASS: stable partial reads/seek, advancing generations, permission check, 400 concurrent reads, 200 fork/exec/exit cycles");
    return 0;
}
