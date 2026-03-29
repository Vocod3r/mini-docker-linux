/* io_pulse.c — Repeatedly writes and reads a temp file */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define BUF_SIZE   (1024 * 1024)   /* 1 MB per write */
#define IO_FILE    "/tmp/io_pulse_data"

static volatile int running = 1;
static void stop(int s) { (void)s; running = 0; }

int main(void) {
    signal(SIGTERM, stop);
    signal(SIGINT,  stop);

    char *buf = malloc(BUF_SIZE);
    if (!buf) { perror("malloc"); return 1; }
    memset(buf, 'X', BUF_SIZE);

    printf("io_pulse: PID %d — writing %d KB per cycle to %s\n",
           (int)getpid(), BUF_SIZE / 1024, IO_FILE);

    int cycle = 0;
    while (running) {
        FILE *fp = fopen(IO_FILE, "wb");
        if (fp) {
            fwrite(buf, 1, BUF_SIZE, fp);
            fclose(fp);
        }
        fp = fopen(IO_FILE, "rb");
        if (fp) {
            fread(buf, 1, BUF_SIZE, fp);
            fclose(fp);
        }
        cycle++;
        if (cycle % 10 == 0)
            printf("io_pulse: completed %d IO cycles\n", cycle);
        usleep(100000);   /* 100 ms between pulses */
    }

    free(buf);
    remove(IO_FILE);
    printf("io_pulse: exiting after %d cycles.\n", cycle);
    return 0;
}
