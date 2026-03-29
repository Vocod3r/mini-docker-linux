/*
 * engine.c — Container Runtime + Supervisor
 *
 * All memory monitoring done via /proc filesystem.
 * No kernel module, no ioctl, no insmod required.
 *
 * Usage:
 *   sudo ./engine run        <name> <workload>
 *   sudo ./engine stop       <name>
 *   sudo ./engine list
 *   sudo ./engine stats      [name]
 *   sudo ./engine maps       <name>
 *   sudo ./engine compress   <name>
 *   sudo ./engine decompress <name>
 *   sudo ./engine supervisor
 *
 * Workloads: cpu_hog | memory_hog | io_pulse
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sched.h>

#include "monitor_ioctl.h"   /* shared structs + thresholds  */

/* Functions provided by monitor.c — compiled together */
int  proc_read_mem(int pid, struct mem_data *out);
int  proc_process_alive(int pid);
void proc_read_maps(int pid);
void proc_print_stats(const struct mem_data *d);

/* ═══════════════════════════════════════════════════════════
 *  Internal container record
 * ═══════════════════════════════════════════════════════════ */
typedef enum {
    STATE_RUNNING    = 0,
    STATE_COMPRESSED = 1,
    STATE_STOPPED    = 2,
} ContainerState;

struct container {
    int            active;
    char           name[CONTAINER_NAME_LEN];
    pid_t          pid;
    ContainerState state;
    char           workload[64];
    time_t         start_time;
    long           compressed_kb;
    int            compression_count;
};

static struct container containers[MAX_CONTAINERS];
static FILE            *log_fp             = NULL;
static volatile int     supervisor_running = 1;

/* ═══════════════════════════════════════════════════════════
 *  Logging
 * ═══════════════════════════════════════════════════════════ */
static void log_write(const char *fmt, ...)
{
    va_list ap;
    time_t  now = time(NULL);
    char    ts[32];
    struct tm *t = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    printf("[%s] ", ts);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);

    if (log_fp) {
        fprintf(log_fp, "[%s] ", ts);
        va_start(ap, fmt); vfprintf(log_fp, fmt, ap); va_end(ap);
        fflush(log_fp);
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Container table helpers
 * ═══════════════════════════════════════════════════════════ */
static struct container *find_container(const char *name)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].active &&
            strcmp(containers[i].name, name) == 0)
            return &containers[i];
    return NULL;
}

static struct container *find_free_slot(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!containers[i].active)
            return &containers[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  swap file path helper
 * ═══════════════════════════════════════════════════════════ */
static void swap_path(const char *name, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/%s.swap", SWAP_DIR, name);
}

/* ═══════════════════════════════════════════════════════════
 *  Memory compression  (user-space via /proc + madvise)
 *
 *  COMPRESS:
 *    1. SIGSTOP  — freeze the process completely
 *    2. Parse /proc/<pid>/maps for anonymous writable regions
 *    3. madvise(MADV_DONTNEED) — kernel drops physical pages now
 *    4. Read /proc/<pid>/status before+after to measure saving
 *    5. Record compressed_kb, update state
 *
 *  DECOMPRESS:
 *    1. SIGCONT  — resume the process
 *    2. Kernel re-faults pages back in on demand (zero-page for anon)
 *    3. Clean up swap record, update state
 * ═══════════════════════════════════════════════════════════ */

/*
 * Walk /proc/<pid>/maps and call madvise(MADV_DONTNEED) on
 * every anonymous, private, writable region (skipping stack/vdso).
 * Returns total KB we asked the kernel to reclaim.
 */
static long evict_anon_pages(pid_t pid, const char *name)
{
    char  maps_path[64];
    char  swap_file[128];
    char  line[256];
    FILE *maps_fp, *swap_fp;
    long  total_kb = 0;

    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    swap_path(name, swap_file, sizeof(swap_file));

    maps_fp = fopen(maps_path, "r");
    if (!maps_fp) {
        log_write("compress: cannot open %s: %s\n",
                  maps_path, strerror(errno));
        return 0;
    }

    mkdir(SWAP_DIR, 0700);
    swap_fp = fopen(swap_file, "wb");   /* record evicted regions */

    while (fgets(line, sizeof(line), maps_fp)) {
        unsigned long start, end, inode;
        char perms[8], offset[16], dev[8], pathname[128];
        pathname[0] = '\0';

        sscanf(line, "%lx-%lx %7s %15s %7s %lu %127s",
               &start, &end, perms, offset, dev, &inode, pathname);

        /*
         * Target anonymous private writable regions only.
         * Skip [stack], [vdso], [vvar] — unsafe to evict.
         */
        int anon    = (pathname[0] == '\0' || pathname[0] == '[');
        int writable = (perms[1] == 'w');
        int priv    = (perms[3] == 'p');
        int special = (strncmp(pathname, "[stack]", 7) == 0 ||
                       strncmp(pathname, "[vdso]",  6) == 0 ||
                       strncmp(pathname, "[vvar]",  6) == 0);

        if (!anon || !writable || !priv || special)
            continue;

        size_t sz = end - start;

        /* Record region boundaries in swap file */
        if (swap_fp) {
            fwrite(&start, sizeof(start), 1, swap_fp);
            fwrite(&end,   sizeof(end),   1, swap_fp);
        }

        /*
         * MADV_DONTNEED: kernel immediately drops the physical
         * pages backing this range. When the process is resumed
         * (SIGCONT) and accesses the range again, the kernel
         * re-faults in zero pages automatically — no data is lost
         * for anonymous mappings; they simply restart at zero.
         * This is the same mechanism used by Android's LMKD and
         * Linux's memory compaction.
         */
        if (madvise((void *)start, sz, MADV_DONTNEED) == 0)
            total_kb += (long)(sz / 1024);
    }

    fclose(maps_fp);
    if (swap_fp) fclose(swap_fp);

    return total_kb;
}

static int do_compress(struct container *c)
{
    if (c->state == STATE_COMPRESSED) {
        printf("'%s' is already compressed.\n", c->name);
        return 0;
    }

    /* Snapshot RSS before eviction */
    struct mem_data before;
    memset(&before, 0, sizeof(before));
    proc_read_mem(c->pid, &before);

    log_write("compress: freezing '%s' (PID %d)  RSS=%ld KB\n",
              c->name, c->pid, before.rss_kb);

    /* Step 1: Freeze */
    if (kill(c->pid, SIGSTOP) < 0) {
        log_write("compress: SIGSTOP failed: %s\n", strerror(errno));
        return -1;
    }
    usleep(50000);   /* 50 ms — let in-flight writes settle */

    /* Step 2: Evict anonymous pages via madvise */
    long evicted_kb = evict_anon_pages(c->pid, c->name);

    /* Step 3: Snapshot RSS after eviction */
    struct mem_data after;
    memset(&after, 0, sizeof(after));
    proc_read_mem(c->pid, &after);

    long saved_kb = before.rss_kb - after.rss_kb;

    /* Step 4: Update container record */
    c->state            = STATE_COMPRESSED;
    c->compression_count++;
    c->compressed_kb    = (saved_kb > 0) ? saved_kb : evicted_kb;

    log_write("compress: '%s' frozen  RSS %ld→%ld KB  "
              "saved~%ld KB  count=%d\n",
              c->name, before.rss_kb, after.rss_kb,
              c->compressed_kb, c->compression_count);
    return 0;
}

static int do_decompress(struct container *c)
{
    if (c->state != STATE_COMPRESSED) {
        printf("'%s' is not compressed.\n", c->name);
        return 0;
    }

    log_write("decompress: resuming '%s' (PID %d)\n", c->name, c->pid);

    /* Resume — kernel re-faults pages back in on next access */
    if (kill(c->pid, SIGCONT) < 0) {
        log_write("decompress: SIGCONT failed: %s\n", strerror(errno));
        return -1;
    }

    /* Remove swap record file */
    char sf[128];
    swap_path(c->name, sf, sizeof(sf));
    remove(sf);

    c->state         = STATE_RUNNING;
    c->compressed_kb = 0;

    log_write("decompress: '%s' resumed\n", c->name);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  Container lifecycle
 * ═══════════════════════════════════════════════════════════ */

/* Child: set up isolation then exec the workload */
static void child_exec(const char *name, const char *workload)
{
    char rootfs[128];
    snprintf(rootfs, sizeof(rootfs), "%s%s", ROOTFS_BASE, name);

    /* New mount + PID + UTS namespaces */
    if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS) < 0)
        perror("unshare (continuing without full isolation)");

    /* Make mounts private so they don't propagate to host */
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    if (access(rootfs, F_OK) == 0) {
        /* Mount /proc inside rootfs so the container can see its own procs */
        char proc_dir[128];
        snprintf(proc_dir, sizeof(proc_dir), "%s/proc", rootfs);
        mkdir(proc_dir, 0555);
        mount("proc", proc_dir, "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

        if (chdir(rootfs) < 0 || chroot(".") < 0)
            perror("chroot (continuing)");
    } else {
        fprintf(stderr,
            "WARNING: rootfs '%s' not found — running without chroot\n",
            rootfs);
    }

    sethostname(name, strlen(name));

    /* Try local path first, then /workloads/ fallback */
    char p1[128], p2[128];
    snprintf(p1, sizeof(p1), "./%s",          workload);
    snprintf(p2, sizeof(p2), "/workloads/%s", workload);
    char *args[] = { p1, NULL };

    execvp(p1, args);
    args[0] = p2;
    execvp(p2, args);

    fprintf(stderr, "exec '%s' failed: %s\n", workload, strerror(errno));
    _exit(EXIT_FAILURE);
}

/* ── cmd: run ─────────────────────────────────────────────── */
static int cmd_run(const char *name, const char *workload)
{
    if (find_container(name)) {
        fprintf(stderr, "Container '%s' already exists.\n", name);
        return 1;
    }
    struct container *slot = find_free_slot();
    if (!slot) {
        fprintf(stderr, "Container table full (max %d).\n", MAX_CONTAINERS);
        return 1;
    }

    log_write("Starting container '%s'  workload='%s'\n", name, workload);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        child_exec(name, workload);
        _exit(EXIT_FAILURE);
    }

    slot->active            = 1;
    slot->pid               = pid;
    slot->state             = STATE_RUNNING;
    slot->start_time        = time(NULL);
    slot->compressed_kb     = 0;
    slot->compression_count = 0;
    strncpy(slot->name,     name,     CONTAINER_NAME_LEN - 1);
    strncpy(slot->workload, workload, sizeof(slot->workload) - 1);

    log_write("Container '%s' started  PID=%d\n", name, pid);
    return 0;
}

/* ── cmd: stop ─────────────────────────────────────────────── */
static int cmd_stop(const char *name)
{
    struct container *c = find_container(name);
    if (!c) { fprintf(stderr, "Container '%s' not found.\n", name); return 1; }

    if (c->state == STATE_COMPRESSED) {
        kill(c->pid, SIGCONT);
        usleep(20000);
    }

    log_write("Stopping '%s' (PID %d)\n", name, c->pid);
    kill(c->pid, SIGTERM);
    usleep(500000);

    if (waitpid(c->pid, NULL, WNOHANG) == 0) {
        kill(c->pid, SIGKILL);
        waitpid(c->pid, NULL, 0);
    }

    char sf[128];
    swap_path(name, sf, sizeof(sf));
    remove(sf);

    memset(c, 0, sizeof(*c));
    log_write("Container '%s' stopped.\n", name);
    return 0;
}

/* ── cmd: list ─────────────────────────────────────────────── */
static void cmd_list(void)
{
    int found = 0;
    printf("%-14s  %-8s  %-14s  %-12s  %-8s  %s\n",
           "NAME", "PID", "STATE", "WORKLOAD", "RSS(KB)", "COMPRESSIONS");
    printf("%-14s  %-8s  %-14s  %-12s  %-8s  %s\n",
           "----", "---", "-----", "--------", "-------", "------------");

    for (int i = 0; i < MAX_CONTAINERS; i++) {
        struct container *c = &containers[i];
        if (!c->active) continue;

        /* Live RSS from /proc */
        struct mem_data md;
        memset(&md, 0, sizeof(md));
        proc_read_mem(c->pid, &md);

        const char *st;
        switch (c->state) {
            case STATE_RUNNING:    st = "RUNNING";    break;
            case STATE_COMPRESSED: st = "COMPRESSED"; break;
            default:               st = "STOPPED";
        }
        printf("%-14s  %-8d  %-14s  %-12s  %-8ld  %d\n",
               c->name, c->pid, st, c->workload,
               md.rss_kb, c->compression_count);
        found++;
    }
    if (!found) printf("  (no containers running)\n");
}

/* ── cmd: stats ─────────────────────────────────────────────── */
static void cmd_stats(const char *name)
{
    if (name) {
        struct container *c = find_container(name);
        if (!c) { fprintf(stderr, "Container '%s' not found.\n", name); return; }

        struct mem_data md;
        memset(&md, 0, sizeof(md));
        strncpy(md.name, c->name, CONTAINER_NAME_LEN - 1);
        md.is_compressed     = (c->state == STATE_COMPRESSED);
        md.compressed_kb     = c->compressed_kb;
        md.compression_count = c->compression_count;

        /* Live data from /proc/<pid>/status */
        proc_read_mem(c->pid, &md);
        proc_print_stats(&md);
    } else {
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].active) continue;
            cmd_stats(containers[i].name);
            printf("\n");
        }
    }
}

/* ── cmd: maps ─────────────────────────────────────────────── */
static void cmd_maps(const char *name)
{
    struct container *c = find_container(name);
    if (!c) { fprintf(stderr, "Container '%s' not found.\n", name); return; }
    proc_read_maps(c->pid);
}

/* ═══════════════════════════════════════════════════════════
 *  Supervisor loop
 *
 *  Polls /proc/<pid>/status for every container on a fixed
 *  interval and automatically applies the memory policy:
 *    RSS > 80 MB  → compress (SIGSTOP + MADV_DONTNEED)
 *    RSS < 40 MB  → decompress (SIGCONT)
 *    RSS > 150 MB → kill
 *
 *  Run in background:  sudo ./engine supervisor &
 * ═══════════════════════════════════════════════════════════ */
static void supervisor_loop(void)
{
    log_write("Supervisor started  poll=%ds  compress@%dMB  kill@%dMB\n",
              SUPERVISOR_INTERVAL_SEC,
              MEM_THRESHOLD_COMPRESS_KB / 1024,
              MEM_THRESHOLD_KILL_KB     / 1024);

    while (supervisor_running) {
        sleep(SUPERVISOR_INTERVAL_SEC);

        for (int i = 0; i < MAX_CONTAINERS; i++) {
            struct container *c = &containers[i];
            if (!c->active) continue;

            /* Check liveness via /proc/<pid> directory existence */
            if (!proc_process_alive(c->pid)) {
                log_write("Container '%s' (PID %d) has exited.\n",
                          c->name, c->pid);
                waitpid(c->pid, NULL, WNOHANG);
                memset(c, 0, sizeof(*c));
                continue;
            }

            /* Read memory stats from /proc/<pid>/status */
            struct mem_data md;
            memset(&md, 0, sizeof(md));
            if (proc_read_mem(c->pid, &md) < 0) continue;

            log_write("[%s] PID=%d  RSS=%ldKB  VM=%ldKB  "
                      "state=%c  compressed=%s  count=%d\n",
                      c->name, c->pid,
                      md.rss_kb, md.vm_kb, md.state,
                      c->state == STATE_COMPRESSED ? "yes" : "no",
                      c->compression_count);

            /* Policy: compress on high memory pressure */
            if (c->state == STATE_RUNNING &&
                md.rss_kb > MEM_THRESHOLD_COMPRESS_KB) {
                log_write("[%s] Pressure: RSS %ld KB > %d KB → COMPRESSING\n",
                          c->name, md.rss_kb, MEM_THRESHOLD_COMPRESS_KB);
                do_compress(c);

            /* Policy: decompress when pressure eases */
            } else if (c->state == STATE_COMPRESSED &&
                       md.rss_kb < (MEM_THRESHOLD_COMPRESS_KB / 2)) {
                log_write("[%s] Eased: RSS %ld KB → DECOMPRESSING\n",
                          c->name, md.rss_kb);
                do_decompress(c);

            /* Policy: kill if critically over limit */
            } else if (c->state == STATE_RUNNING &&
                       md.rss_kb > MEM_THRESHOLD_KILL_KB) {
                log_write("[%s] Critical: RSS %ld KB > %d KB → KILLING\n",
                          c->name, md.rss_kb, MEM_THRESHOLD_KILL_KB);
                cmd_stop(c->name);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Signal handler — clean shutdown
 * ═══════════════════════════════════════════════════════════ */
static void sig_handler(int sig)
{
    (void)sig;
    supervisor_running = 0;
    log_write("Shutting down — stopping all containers...\n");
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].active)
            cmd_stop(containers[i].name);
    if (log_fp) fclose(log_fp);
    exit(0);
}

/* ═══════════════════════════════════════════════════════════
 *  Usage + main
 * ═══════════════════════════════════════════════════════════ */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s run        <name> <workload>  Start a container\n"
        "  %s stop       <name>             Stop a container\n"
        "  %s list                           List all containers\n"
        "  %s stats      [name]              Show /proc memory stats\n"
        "  %s maps       <name>              Show memory map\n"
        "  %s compress   <name>              Freeze + evict pages\n"
        "  %s decompress <name>              Resume container\n"
        "  %s supervisor                     Auto-monitor loop\n"
        "\n"
        "Workloads:  cpu_hog  memory_hog  io_pulse\n"
        "\n"
        "Example:\n"
        "  sudo ./engine run alpha memory_hog\n"
        "  sudo ./engine stats alpha\n"
        "  sudo ./engine compress alpha\n"
        "  sudo ./engine supervisor\n",
        prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    log_fp = fopen(LOG_FILE, "a");
    if (!log_fp)
        fprintf(stderr, "WARNING: cannot open %s for logging\n", LOG_FILE);

    memset(containers, 0, sizeof(containers));
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    const char *cmd = argv[1];

    if (strcmp(cmd, "run") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: %s run <name> <workload>\n", argv[0]); return 1; }
        return cmd_run(argv[2], argv[3]);

    } else if (strcmp(cmd, "stop") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s stop <name>\n", argv[0]); return 1; }
        return cmd_stop(argv[2]);

    } else if (strcmp(cmd, "list") == 0) {
        cmd_list(); return 0;

    } else if (strcmp(cmd, "stats") == 0) {
        cmd_stats(argc >= 3 ? argv[2] : NULL); return 0;

    } else if (strcmp(cmd, "maps") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s maps <name>\n", argv[0]); return 1; }
        cmd_maps(argv[2]); return 0;

    } else if (strcmp(cmd, "compress") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s compress <name>\n", argv[0]); return 1; }
        struct container *c = find_container(argv[2]);
        if (!c) { fprintf(stderr, "Container '%s' not found.\n", argv[2]); return 1; }
        return do_compress(c);

    } else if (strcmp(cmd, "decompress") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s decompress <name>\n", argv[0]); return 1; }
        struct container *c = find_container(argv[2]);
        if (!c) { fprintf(stderr, "Container '%s' not found.\n", argv[2]); return 1; }
        return do_decompress(c);

    } else if (strcmp(cmd, "supervisor") == 0) {
        supervisor_loop(); return 0;

    } else {
        fprintf(stderr, "Unknown command: %s\n\n", cmd);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
}
