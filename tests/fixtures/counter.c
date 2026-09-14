/* Calls work() three times. Built -no-pie so the address `nm` reports for
   work() is the address it actually runs at, which is what you pass to
   --break. */
#include <stdio.h>

void work(int i) {
    printf("work %d\n", i);
    fflush(stdout);
}

int main(void) {
    for (int i = 0; i < 3; i++) {
        work(i);
    }
    return 0;
}
