/* cpu_hog.c — Saturates one CPU core */
#include <stdio.h>
#include <signal.h>

static volatile int running = 1;
static void stop(int s) { (void)s; running = 0; }

int main(void) {
    signal(SIGTERM, stop);
    signal(SIGINT,  stop);
    printf("cpu_hog: burning CPU (PID %d)...\n", (int)getpid());
    volatile long x = 0;
    while (running) x++;
    printf("cpu_hog: stopped.\n");
    return 0;
}
