#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

/*
 * monitor_ioctl.h
 * Shared data structures for the container runtime.
 *
 * No kernel module required — all memory stats are read from
 * the /proc filesystem (/proc/<pid>/status, /proc/<pid>/maps).
 *
 * Included by both engine.c and monitor.c (user-space only).
 */

#define MAX_CONTAINERS     16
#define CONTAINER_NAME_LEN 32

/* Memory stats for one container */
struct mem_data {
    int  pid;
    char name[CONTAINER_NAME_LEN];
    long rss_kb;             /* VmRSS  from /proc/<pid>/status   */
    long vm_kb;              /* VmSize from /proc/<pid>/status   */
    long shared_kb;          /* RssFile from /proc/<pid>/status  */
    long swap_kb;            /* VmSwap from /proc/<pid>/status   */
    long compressed_kb;      /* KB evicted via MADV_DONTNEED     */
    int  is_compressed;      /* 1 = frozen (SIGSTOP sent)        */
    int  compression_count;  /* total times compressed           */
    char state;              /* process state: R S D Z T         */
};

/* Snapshot of all containers */
struct all_containers_data {
    int            count;
    struct mem_data entries[MAX_CONTAINERS];
};

/* Memory pressure thresholds (in KB) */
#define MEM_THRESHOLD_COMPRESS_KB  (80  * 1024)   /* 80  MB → compress */
#define MEM_THRESHOLD_KILL_KB      (150 * 1024)   /* 150 MB → kill     */

/* Supervisor poll interval */
#define SUPERVISOR_INTERVAL_SEC 2

/* Where compressed page maps are stored */
#define SWAP_DIR "/tmp/container_swap"

/* Log file */
#define LOG_FILE "engine.log"

/* Rootfs prefix */
#define ROOTFS_BASE "rootfs-"

#endif /* MONITOR_IOCTL_H */
