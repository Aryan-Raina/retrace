/* Writes its pid to argv[1], waits for SIGUSR1, and reports that its own
   handler ran.

   This is the only fixture whose target survives a signal, so it is the only
   one that makes the supervisor's wait loop go round more than once: stop,
   forward the signal, keep running, wait again. */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t caught = 0;

static void on_usr1(int signum) {
    (void)signum;
    caught = 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return 2;
    }

    signal(SIGUSR1, on_usr1);

    /* Written only once the handler is installed, so the test cannot race
       ahead and signal us too early. */
    FILE *pidfile = fopen(argv[1], "w");
    if (pidfile == NULL) {
        return 2;
    }
    fprintf(pidfile, "%d\n", (int)getpid());
    fclose(pidfile);

    while (!caught) {
        pause();
    }

    printf("handler ran\n");
    return 0;
}
