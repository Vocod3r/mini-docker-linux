#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

/*
 * monitor_ioctl.h — shared types for engine.c and monitor.c
 * No kernel module required — memory stats come from /proc.
 */

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#define MONITOR_NAME_LEN   32
#define MAX_CONTAINERS     16
#define CONTAINER_NAME_LEN 32

/* Kept for boilerplate compatibility — not called in /proc mode */
struct monitor_request {
    pid_t         pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char          container_id[MONITOR_NAME_LEN];
};

#define MONITOR_MAGIC      'M'
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

/* Memory stats populated by proc_read_mem() */
struct mem_data {
    int  pid;
    char name[CONTAINER_NAME_LEN];
    long rss_kb;
    long vm_kb;
    long shared_kb;
    long swap_kb;
    long compressed_kb;
    int  is_compressed;
    int  compression_count;
    char state;
};

/* Memory pressure thresholds */
#define MEM_THRESHOLD_COMPRESS_KB  (80  * 1024)
#define MEM_THRESHOLD_KILL_KB      (150 * 1024)
#define SUPERVISOR_INTERVAL_SEC    2
#define SWAP_DIR                   "/tmp/container_swap"
#define LOG_FILE                   "engine.log"
#define ROOTFS_BASE                "rootfs-"

#endif /* MONITOR_IOCTL_H */