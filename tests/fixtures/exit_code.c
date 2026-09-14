/* Exits with the status given as its first argument. */
#include <stdlib.h>

int main(int argc, char **argv) {
    return argc > 1 ? atoi(argv[1]) : 0;
}
