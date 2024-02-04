#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    // TODO
    if (argc < 2) {
        fprintf(stderr, "fail: no argument!\n", argv[0]);
        exit(1);
    }
    if (mkdir(argv[1], 0) < 0) {
        fprintf(stderr, "mkdir %s failed\n", argv[1]);
        exit(1);
    }

    exit(0);
}