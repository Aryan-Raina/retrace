/* Prints the argument vector it was given, one entry per line. */
#include <stdio.h>

int main(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }
    return 0;
}
