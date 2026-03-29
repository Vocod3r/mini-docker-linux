/* memory_hog.c — Gradually allocates and touches memory */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define CHUNK_MB    10
#define SLEEP_SEC    2
#define MAX_ALLOC_MB 200

static volatile int running = 1;
static void stop(int s) { (void)s; running = 0; }

int main(void) {
    signal(SIGTERM, stop);
    signal(SIGINT,  stop);

    printf("memory_hog: PID %d — allocating %d MB in %d MB chunks\n",
           (int)getpid(), MAX_ALLOC_MB, CHUNK_MB);

    char *ptrs[MAX_ALLOC_MB / CHUNK_MB];
    int   count = 0;
    int   limit = MAX_ALLOC_MB / CHUNK_MB;

    while (running && count < limit) {
        size_t sz = CHUNK_MB * 1024 * 1024;
        ptrs[count] = malloc(sz);
        if (!ptrs[count]) { perror("malloc"); break; }
        /* Touch every page so it becomes resident */
        memset(ptrs[count], 0xAB, sz);
        count++;
        printf("memory_hog: allocated %d MB total\n", count * CHUNK_MB);
        sleep(SLEEP_SEC);
    }

    printf("memory_hog: holding %d MB — waiting for signal\n",
           count * CHUNK_MB);
    while (running) sleep(1);

    for (int i = 0; i < count; i++) free(ptrs[i]);
    printf("memory_hog: freed memory, exiting.\n");
    return 0;
}
