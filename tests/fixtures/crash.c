/* Dies from a signal: SIGSEGV when passed "segv", SIGABRT otherwise. */
#include <signal.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "segv") == 0) {
        volatile int *nowhere = 0;
        *nowhere = 1;
    }
    raise(SIGABRT);
    return 0;
}
