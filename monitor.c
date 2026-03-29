/*
 * monitor.c — User-Space Process Memory Monitor
 *
 * Replaces the kernel module entirely.
 * Reads memory stats directly from the /proc filesystem:
 *
 *   /proc/<pid>/status  → VmRSS, VmSize, VmSwap, State
 *   /proc/<pid>/maps    → memory region breakdown
 *   /proc/<pid>/statm   → page-level stats
 *
 * This file provides:
 *   1. proc_read_mem()      — fill a mem_data struct for any PID
 *   2. proc_read_maps()     — print a container's memory map
 *   3. proc_process_alive() — check if a PID still exists
 *   4. monitor_run()        — standalone monitor loop (optional)
 *
 * Used directly by engine.c (no device file, no ioctl needed).
 *
 * Build: gcc -Wall -O2 -o monitor monitor.c
 * Run:   ./monitor <pid>        (watch one PID)
 *        ./monitor              (prints usage)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "monitor_ioctl.h"

/* ═══════════════════════════════════════════════════════════
 *  proc_read_mem()
 *
 *  Reads /proc/<pid>/status and fills in a mem_data struct.
 *
 *  Key fields from /proc/<pid>/status:
 *    State:   R (running), S (sleeping), T (stopped), Z (zombie)
 *    VmSize:  total virtual memory
 *    VmRSS:   resident set size (actual RAM used)
 *    RssFile: RSS from file mappings
 *    VmSwap:  memory in swap
 *
 *  Returns 0 on success, -1 if PID does not exist.
 * ═══════════════════════════════════════════════════════════ */
int proc_read_mem(int pid, struct mem_data *out)
{
    char path[64];
    char line[256];
    FILE *fp;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    fp = fopen(path, "r");
    if (!fp) {
        /* PID does not exist or we lack permission */
        return -1;
    }

    out->pid    = pid;
    out->rss_kb = 0;
    out->vm_kb  = 0;
    out->shared_kb = 0;
    out->swap_kb   = 0;
    out->state     = '?';

    while (fgets(line, sizeof(line), fp)) {
        /* State line: "State:\t R (running)" */
        if (strncmp(line, "State:", 6) == 0) {
            char *p = line + 6;
            while (*p && isspace((unsigned char)*p)) p++;
            out->state = *p;   /* R, S, D, Z, T, etc. */
        }
        /* VmSize: total virtual memory in kB */
        else if (strncmp(line, "VmSize:", 7) == 0) {
            sscanf(line + 7, "%ld", &out->vm_kb);
        }
        /* VmRSS: resident set size in kB */
        else if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &out->rss_kb);
        }
        /* RssFile: file-backed RSS in kB */
        else if (strncmp(line, "RssFile:", 8) == 0) {
            sscanf(line + 8, "%ld", &out->shared_kb);
        }
        /* VmSwap: swapped-out memory in kB */
        else if (strncmp(line, "VmSwap:", 7) == 0) {
            sscanf(line + 7, "%ld", &out->swap_kb);
        }
    }

    fclose(fp);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  proc_process_alive()
 *
 *  Returns 1 if the PID exists in /proc, 0 otherwise.
 * ═══════════════════════════════════════════════════════════ */
int proc_process_alive(int pid)
{
    char path[64];
    struct stat st;
    snprintf(path, sizeof(path), "/proc/%d", pid);
    return (stat(path, &st) == 0) ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════
 *  proc_read_maps()
 *
 *  Reads /proc/<pid>/maps and prints a summary of memory
 *  regions. Useful for debugging and for the compression
 *  logic in engine.c which needs to find anonymous regions.
 *
 *  Each line from /proc/<pid>/maps looks like:
 *    address           perms offset  dev   inode  pathname
 *    7f3e4b000000-7f3e4c000000 rw-p 00000000 00:00 0
 * ═══════════════════════════════════════════════════════════ */
void proc_read_maps(int pid)
{
    char path[64];
    char line[512];
    FILE *fp;

    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "monitor: cannot open %s: %s\n",
                path, strerror(errno));
        return;
    }

    printf("\n── Memory map for PID %d ──\n", pid);
    printf("%-36s %-6s %-16s %s\n",
           "Address Range", "Perms", "Size", "Pathname");
    printf("%s\n", "------------------------------------------------------------");

    while (fgets(line, sizeof(line), fp)) {
        unsigned long start, end;
        char perms[8], offset[16], dev[8], pathname[256];
        unsigned long inode;
        pathname[0] = '\0';

        sscanf(line, "%lx-%lx %7s %15s %7s %lu %255s",
               &start, &end, perms, offset, dev, &inode, pathname);

        unsigned long size_kb = (end - start) / 1024;

        /* Label anonymous regions clearly */
        const char *label = pathname[0] ? pathname : "[anonymous]";

        printf("%016lx-%016lx  %-6s  %6lu KB  %s\n",
               start, end, perms, size_kb, label);
    }

    fclose(fp);
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════
 *  proc_print_stats()
 *
 *  Pretty-print a mem_data struct.
 * ═══════════════════════════════════════════════════════════ */
void proc_print_stats(const struct mem_data *d)
{
    const char *state_str;
    switch (d->state) {
        case 'R': state_str = "Running";   break;
        case 'S': state_str = "Sleeping";  break;
        case 'T': state_str = "Stopped";   break;
        case 'Z': state_str = "Zombie";    break;
        case 'D': state_str = "Disk wait"; break;
        default:  state_str = "Unknown";
    }

    printf("  PID          : %d\n",   d->pid);
    printf("  Name         : %s\n",   d->name[0] ? d->name : "(unknown)");
    printf("  State        : %c — %s\n", d->state, state_str);
    printf("  VM Size      : %ld KB  (%.1f MB)\n",
           d->vm_kb,  d->vm_kb  / 1024.0);
    printf("  RSS (RAM)    : %ld KB  (%.1f MB)\n",
           d->rss_kb, d->rss_kb / 1024.0);
    printf("  Shared       : %ld KB\n", d->shared_kb);
    printf("  Swap         : %ld KB\n", d->swap_kb);
    if (d->is_compressed) {
        printf("  Compressed   : YES  (~%ld KB evicted, %d times)\n",
               d->compressed_kb, d->compression_count);
    }
}

/* ═══════════════════════════════════════════════════════════
 *  monitor_run()
 *
 *  Standalone watch loop — polls /proc/<pid>/status every
 *  SUPERVISOR_INTERVAL_SEC seconds and prints stats.
 *  Used when running:  ./monitor <pid>
 * ═══════════════════════════════════════════════════════════ */
static volatile int keep_running = 1;
static void handle_sig(int s) { (void)s; keep_running = 0; }

void monitor_run(int pid)
{
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    printf("monitor: watching PID %d  (Ctrl-C to stop)\n\n", pid);

    while (keep_running) {
        struct mem_data d;
        memset(&d, 0, sizeof(d));

        if (proc_read_mem(pid, &d) < 0) {
            printf("monitor: PID %d no longer exists.\n", pid);
            break;
        }

        printf("── PID %d ──\n", pid);
        proc_print_stats(&d);
        printf("\n");

        sleep(SUPERVISOR_INTERVAL_SEC);
    }

    printf("monitor: exiting.\n");
}

/* ═══════════════════════════════════════════════════════════
 *  main() — standalone usage
 *
 *  ./monitor <pid>        watch a specific PID
 *  ./monitor              print usage
 * ═══════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s <pid>          Watch memory stats for a process\n"
            "\n"
            "Examples:\n"
            "  %s 1234\n"
            "\n"
            "All data is read from /proc/<pid>/status — no kernel\n"
            "module or root privileges required.\n",
            argv[0], argv[0]);
        return 1;
    }

    int pid = atoi(argv[1]);
    if (pid <= 0) {
        fprintf(stderr, "Invalid PID: %s\n", argv[1]);
        return 1;
    }

    /* Show map once, then watch */
    proc_read_maps(pid);
    monitor_run(pid);

    return 0;
}
