#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    // TODO
    int fd_count = argc == 1 ? 1 : argc - 1;
    int fds[fd_count];
    if (argc == 1) {
        fds[0] = STDIN_FILENO;
        if (fds[0] < 0) {
            fprintf(stderr, "open stdin failed: %s\n", strerror(errno));
            exit(1);
        }
    } else {
        for (int i = 0; i < fd_count; i++) {
            fds[i] = open(argv[i + 1], O_RDONLY);
            if (fds[i] < 0) {
                fprintf(stderr, "open %s failed: %s\n", argv[i + 1],
                        strerror(errno));
                exit(1);
            }
        }
    }
    for (int i = 0; i < fd_count; i++) {
        int fd = fds[i];
        if (fd < 0) {
            fprintf(stderr, "open %s failed: %s\n", argv[1], strerror(errno));
            exit(1);
        }
        char buf[1024];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (write(STDOUT_FILENO, buf, n) != n) {
                fprintf(stderr, "write failed: %s\n", strerror(errno));
                exit(1);
            }
        }
        if (n < 0) {
            fprintf(stderr, "read %s failed: %s\n", argv[1], strerror(errno));
            exit(1);
        }
    }
    return 0;
}