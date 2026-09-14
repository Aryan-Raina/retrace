/* Opens the file named by argv[1], reads it, and prints what it read.
   The syscall sequence is small and obvious: openat, read, write, close. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        return 2;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    char buffer[256];
    ssize_t got = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (got < 0) {
        perror("read");
        return 1;
    }

    buffer[got] = '\0';
    printf("read %zd bytes: %s", got, buffer);
    return 0;
}
