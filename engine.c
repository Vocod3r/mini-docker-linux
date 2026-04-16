/*
 * engine.c - Supervised Multi-Container Runtime (Complete Implementation)
 *
 * Architecture:
 *   - supervisor: long-running daemon, holds all container state in memory,
 *                 listens on a Unix domain socket, monitors memory via /proc
 *   - client:     any other command (run, ps, stats, stop, logs) connects
 *                 to the supervisor socket, sends a request, prints response
 *
 * This fixes "alpha not found" — state lives in the supervisor process,
 * not in a file that gets recreated each invocation.
 *
 * Workflow:
 *   sudo ./engine supervisor rootfs-base &
 *   sudo ./engine run   alpha rootfs-alpha memory_hog
 *   sudo ./engine ps
 *   sudo ./engine stats alpha
 *   sudo ./engine stop  alpha
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ── Constants ───────────────────────────────────────────────── */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 512          /* enlarged for stats output   */
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20) /* 40 MiB                      */
#define DEFAULT_HARD_LIMIT  (64UL << 20) /* 64 MiB                      */
#define MONITOR_INTERVAL_SEC 2           /* /proc poll period            */

/* ── Command kinds ───────────────────────────────────────────── */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP,
    CMD_STATS          /* NEW: per-container /proc memory stats */
} command_kind_t;

/* ── Container state ─────────────────────────────────────────── */
typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ── Data structures (from boilerplate, unchanged) ───────────── */
typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;                        /* 0=done  1=more-coming  <0=error */
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

typedef struct {
    int                server_fd;
    int                monitor_fd;      /* unused — we use /proc directly */
    int                should_stop;
    pthread_t          logger_thread;
    bounded_buffer_t   log_buffer;
    pthread_mutex_t    metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ── Pipe-reader thread arg ──────────────────────────────────── */
typedef struct {
    int              pipe_read_fd;
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} pipe_reader_args_t;

/* ── Forward declarations for /proc helpers (from monitor.c) ─── */
int  proc_read_mem(int pid, struct mem_data *out);
int  proc_process_alive(int pid);
void proc_print_stats(const struct mem_data *d);

/* ── Global supervisor context (for signal handler) ─────────── */
static supervisor_ctx_t g_ctx;

/* ═══════════════════════════════════════════════════════════════
 *  Utility
 * ═══════════════════════════════════════════════════════════════ */
static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static int parse_mib_flag(const char *flag, const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib = strtoul(value, &end, 10);
    if (errno || end == value || *end) {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                 char *argv[], int start)
{
    for (int i = start; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1],
                               &req->soft_limit_bytes)) return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1],
                               &req->hard_limit_bytes)) return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            req->nice_value = atoi(argv[i+1]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Bounded buffer  (producer-consumer, mutex + condvar)
 * ═══════════════════════════════════════════════════════════════ */
static int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    int rc = pthread_mutex_init(&b->mutex, NULL);
    if (rc) return rc;
    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc) { pthread_mutex_destroy(&b->mutex); return rc; }
    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/* Block until space is available, then insert item. */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* Block until an item is available, then remove it. */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0) {           /* shutting down and empty */
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Logging consumer thread
 *  Pops chunks from the bounded buffer and appends them to the
 *  per-container log file.
 * ═══════════════════════════════════════════════════════════════ */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strcmp(c->id, item.container_id) == 0) {
                int fd = open(c->log_path,
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd >= 0) {
                    write(fd, item.data, item.length);
                    close(fd);
                }
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
    return NULL;
}

/* ── Pipe-reader thread: forwards child stdout/stderr to bounded buffer ── */
static void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *pra = (pipe_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    strncpy(item.container_id, pra->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(pra->pipe_read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(pra->buffer, &item);
    }

    close(pra->pipe_read_fd);
    free(pra);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 *  child_fn — runs inside the cloned container process
 *
 *  Context: new PID + mount + UTS namespaces.
 *  Responsible for: chroot, /proc mount, exec.
 * ═══════════════════════════════════════════════════════════════ */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout/stderr to logging pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* Container hostname = container ID */
    sethostname(cfg->id, strlen(cfg->id));

    /* Make mount namespace private */
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    /* Chroot into container rootfs if it exists */
    if (access(cfg->rootfs, F_OK) == 0) {
        char proc_path[PATH_MAX];
        snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
        mkdir(proc_path, 0555);
        mount("proc", proc_path, "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

        if (chdir(cfg->rootfs) == 0)
            chroot(".");
    } else {
        fprintf(stderr,
            "WARNING: rootfs '%s' not found — no chroot isolation\n",
            cfg->rootfs);
    }

    /* Apply nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Exec: try shell first, then direct exec */
    execl("/bin/sh", "sh", "-c", cfg->command, NULL);

    char *args[] = { cfg->command, NULL };
    execvp(cfg->command, args);

    fprintf(stderr, "exec '%s' failed: %s\n", cfg->command, strerror(errno));
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 *  Container list helpers (supervisor-side)
 * ═══════════════════════════════════════════════════════════════ */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                           const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}

static void add_container(supervisor_ctx_t *ctx, container_record_t *c)
{
    c->next         = ctx->containers;
    ctx->containers = c;
}

static void remove_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t **pp = &ctx->containers;
    while (*pp) {
        if (strcmp((*pp)->id, id) == 0) {
            container_record_t *tmp = *pp;
            *pp = tmp->next;
            free(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ── Send one response to client ─────────────────────────────── */
static void send_resp(int fd, int status, const char *fmt, ...)
{
    control_response_t r;
    va_list ap;
    r.status = status;
    va_start(ap, fmt);
    vsnprintf(r.message, sizeof(r.message), fmt, ap);
    va_end(ap);
    send(fd, &r, sizeof(r), MSG_NOSIGNAL);
}

/* ═══════════════════════════════════════════════════════════════
 *  Supervisor request handlers
 * ═══════════════════════════════════════════════════════════════ */

static void handle_start(supervisor_ctx_t *ctx,
                          control_request_t *req, int client_fd)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        send_resp(client_fd, -1, "Container '%s' already exists.",
                  req->container_id);
        return;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create log directory + path */
    mkdir(LOG_DIR, 0755);
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/%s.log",
             LOG_DIR, req->container_id);

    /* Pipe: child writes, supervisor reads and forwards to bounded buffer */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        send_resp(client_fd, -1, "pipe() failed: %s", strerror(errno));
        return;
    }

    /* Child config */
    child_config_t *cfg = calloc(1, sizeof(child_config_t));
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,       CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* Stack for clone() */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg); close(pipefd[0]); close(pipefd[1]);
        send_resp(client_fd, -1, "malloc() failed");
        return;
    }

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD,
                      cfg);

    close(pipefd[1]);   /* parent does not write to pipe */
    free(stack);
    free(cfg);

    if (pid < 0) {
        close(pipefd[0]);
        send_resp(client_fd, -1, "clone() failed: %s", strerror(errno));
        return;
    }

    /* Register container record */
    container_record_t *cr = calloc(1, sizeof(container_record_t));
    strncpy(cr->id,       req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cr->log_path, log_path,           PATH_MAX - 1);
    cr->host_pid          = pid;
    cr->started_at        = time(NULL);
    cr->state             = CONTAINER_RUNNING;
    cr->soft_limit_bytes  = req->soft_limit_bytes;
    cr->hard_limit_bytes  = req->hard_limit_bytes;

    pthread_mutex_lock(&ctx->metadata_lock);
    add_container(ctx, cr);
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Start pipe-reader thread to forward output to bounded buffer */
    pipe_reader_args_t *pra = malloc(sizeof(pipe_reader_args_t));
    pra->pipe_read_fd = pipefd[0];
    pra->buffer       = &ctx->log_buffer;
    strncpy(pra->container_id, req->container_id, CONTAINER_ID_LEN - 1);

    pthread_t t;
    pthread_create(&t, NULL, pipe_reader_thread, pra);
    pthread_detach(t);

    send_resp(client_fd, 0,
              "Container '%s' started  PID=%d", req->container_id, pid);
}

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    control_response_t r;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;

    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        send_resp(client_fd, 0, "(no containers)");
        return;
    }

    /* Send header */
    r.status = 1;
    snprintf(r.message, sizeof(r.message),
             "%-14s  %-8s  %-10s  %-8s  %s",
             "ID", "PID", "STATE", "RSS(KB)", "STARTED");
    send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);

    while (c) {
        struct mem_data md;
        memset(&md, 0, sizeof(md));
        proc_read_mem(c->host_pid, &md);

        char started[32];
        struct tm *tm = localtime(&c->started_at);
        strftime(started, sizeof(started), "%H:%M:%S", tm);

        r.status = c->next ? 1 : 0;
        snprintf(r.message, sizeof(r.message),
                 "%-14s  %-8d  %-10s  %-8ld  %s",
                 c->id, c->host_pid,
                 state_to_string(c->state),
                 md.rss_kb, started);
        send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void handle_stats(supervisor_ctx_t *ctx,
                          control_request_t *req, int client_fd)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        send_resp(client_fd, -1, "Container '%s' not found.",
                  req->container_id);
        return;
    }
    pid_t pid   = c->host_pid;
    unsigned long soft = c->soft_limit_bytes;
    unsigned long hard = c->hard_limit_bytes;
    container_state_t st = c->state;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Read live /proc stats */
    struct mem_data md;
    memset(&md, 0, sizeof(md));
    proc_read_mem(pid, &md);

    control_response_t r;

    /* Send each line as a separate response with status=1, last=0 */
    r.status = 1;
    snprintf(r.message, sizeof(r.message),
             "Container : %s", req->container_id);
    send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);

    snprintf(r.message, sizeof(r.message), "PID       : %d", pid);
    send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);

    snprintf(r.message, sizeof(r.message), "State     : %s",
             state_to_string(st));
    send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);

    snprintf(r.message, sizeof(r.message),
             "RSS       : %ld KB  (%.1f MB)",
             md.rss_kb, md.rss_kb / 1024.0);
    send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);

    snprintf(r.message, sizeof(r.message),
             "VM Size   : %ld KB  (%.1f MB)",
             md.vm_kb, md.vm_kb / 1024.0);
    send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);

    snprintf(r.message, sizeof(r.message),
             "Swap      : %ld KB", md.swap_kb);
    send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);

    snprintf(r.message, sizeof(r.message),
             "Soft Limit: %lu MB", soft >> 20);
    send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);

    r.status = 0;
    snprintf(r.message, sizeof(r.message),
             "Hard Limit: %lu MB", hard >> 20);
    send(client_fd, &r, sizeof(r), MSG_NOSIGNAL);
}

static void handle_logs(supervisor_ctx_t *ctx,
                         control_request_t *req, int client_fd)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        send_resp(client_fd, -1, "Container '%s' not found.",
                  req->container_id);
        return;
    }
    char log_path[PATH_MAX];
    strncpy(log_path, c->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    FILE *fp = fopen(log_path, "r");
    if (!fp) {
        send_resp(client_fd, 0, "(no log yet for '%s' — container may still be starting)",
                  req->container_id);
        return;
    }

    /* Check if the log file is empty before reading */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    rewind(fp);

    if (file_size == 0) {
        fclose(fp);
        send_resp(client_fd, 0, "(log is empty — workload has not printed anything yet)");
        return;
    }

    /* Stream each line to the client */
    char line[CONTROL_MESSAGE_LEN];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';   /* strip newline */
        int more = !feof(fp);
        send_resp(client_fd, more ? 1 : 0, "%s", line);
    }

    /* Ensure the client always gets a final status=0 */
    if (!feof(fp))
        send_resp(client_fd, 0, "(end of log)");

    fclose(fp);
}

static void handle_stop(supervisor_ctx_t *ctx,
                         control_request_t *req, int client_fd)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        send_resp(client_fd, -1, "Container '%s' not found.",
                  req->container_id);
        return;
    }
    pid_t pid = c->host_pid;
    c->state  = CONTAINER_STOPPED;
    pthread_mutex_unlock(&ctx->metadata_lock);

    kill(pid, SIGTERM);
    usleep(500000);
    if (proc_process_alive(pid)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    remove_container(ctx, req->container_id);
    pthread_mutex_unlock(&ctx->metadata_lock);

    send_resp(client_fd, 0, "Container '%s' stopped.", req->container_id);
}

/* ── Dispatch one client request ─────────────────────────────── */
static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    ssize_t n = recv(client_fd, &req, sizeof(req), 0);
    if (n != (ssize_t)sizeof(req)) {
        close(client_fd);
        return;
    }

    switch (req.kind) {
    case CMD_START:
    case CMD_RUN:
        handle_start(ctx, &req, client_fd);
        break;
    case CMD_PS:
        handle_ps(ctx, client_fd);
        break;
    case CMD_STATS:
        handle_stats(ctx, &req, client_fd);
        break;
    case CMD_LOGS:
        handle_logs(ctx, &req, client_fd);
        break;
    case CMD_STOP:
        handle_stop(ctx, &req, client_fd);
        break;
    default:
        send_resp(client_fd, -1, "Unknown command kind: %d", req.kind);
    }

    close(client_fd);
}

/* ── Periodic /proc memory check ─────────────────────────────── */
static void check_memory(supervisor_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c) {
        container_record_t *next = c->next;

        if (!proc_process_alive(c->host_pid)) {
            fprintf(stderr,
                "[supervisor] Container '%s' (PID %d) exited.\n",
                c->id, c->host_pid);
            waitpid(c->host_pid, NULL, WNOHANG);
            c->state = CONTAINER_EXITED;
            c = next;
            continue;
        }

        struct mem_data md;
        memset(&md, 0, sizeof(md));
        if (proc_read_mem(c->host_pid, &md) < 0) { c = next; continue; }

        if ((unsigned long)md.rss_kb * 1024 > c->hard_limit_bytes) {
            fprintf(stderr,
                "[supervisor] HARD LIMIT: '%s' RSS=%ldKB > %luMB → KILLING\n",
                c->id, md.rss_kb, c->hard_limit_bytes >> 20);
            kill(c->host_pid, SIGKILL);
            c->state = CONTAINER_KILLED;
        } else if ((unsigned long)md.rss_kb * 1024 > c->soft_limit_bytes) {
            fprintf(stderr,
                "[supervisor] SOFT LIMIT: '%s' RSS=%ldKB > %luMB\n",
                c->id, md.rss_kb, c->soft_limit_bytes >> 20);
        }
        c = next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

/* ── SIGCHLD handler ─────────────────────────────────────────── */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved;
}

/* ── SIGINT / SIGTERM handler ────────────────────────────────── */
static void shutdown_handler(int sig)
{
    (void)sig;
    g_ctx.should_stop = 1;
}

/* ═══════════════════════════════════════════════════════════════
 *  run_supervisor — long-running daemon
 * ═══════════════════════════════════════════════════════════════ */
static int run_supervisor(const char *rootfs)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.server_fd  = -1;
    g_ctx.monitor_fd = -1;

    fprintf(stderr, "[supervisor] Starting  base-rootfs=%s\n", rootfs);

    /* Init mutex and bounded buffer */
    pthread_mutex_init(&g_ctx.metadata_lock, NULL);
    if (bounded_buffer_init(&g_ctx.log_buffer)) {
        perror("bounded_buffer_init");
        return 1;
    }

    /* Create Unix domain socket */
    unlink(CONTROL_PATH);
    g_ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(g_ctx.server_fd, 8) < 0) {
        perror("listen"); return 1;
    }

    /* Signals */
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT,  shutdown_handler);
    signal(SIGTERM, shutdown_handler);

    /* Start logging thread */
    pthread_create(&g_ctx.logger_thread, NULL, logging_thread, &g_ctx);

    fprintf(stderr,
        "[supervisor] Ready. Socket: %s\n"
        "[supervisor] Ctrl-C to stop.\n", CONTROL_PATH);

    /* Event loop: select with 2s timeout for memory monitoring */
    while (!g_ctx.should_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_ctx.server_fd, &rfds);

        struct timeval tv = { MONITOR_INTERVAL_SEC, 0 };
        int ret = select(g_ctx.server_fd + 1, &rfds, NULL, NULL, &tv);

        if (ret < 0 && errno == EINTR) continue;

        if (ret > 0 && FD_ISSET(g_ctx.server_fd, &rfds)) {
            int client_fd = accept(g_ctx.server_fd, NULL, NULL);
            if (client_fd >= 0)
                handle_client(&g_ctx, client_fd);
        }

        /* Periodic memory check */
        check_memory(&g_ctx);
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&g_ctx.metadata_lock);
    container_record_t *c = g_ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING)
            kill(c->host_pid, SIGTERM);
        c = c->next;
    }
    pthread_mutex_unlock(&g_ctx.metadata_lock);

    bounded_buffer_begin_shutdown(&g_ctx.log_buffer);
    pthread_join(g_ctx.logger_thread, NULL);
    bounded_buffer_destroy(&g_ctx.log_buffer);
    pthread_mutex_destroy(&g_ctx.metadata_lock);
    close(g_ctx.server_fd);
    unlink(CONTROL_PATH);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  send_control_request — client side
 *  Connects to supervisor socket, sends request, prints responses.
 * ═══════════════════════════════════════════════════════════════ */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
            "Cannot connect to supervisor at %s.\n"
            "Start it first:  sudo ./engine supervisor <rootfs> &\n",
            CONTROL_PATH);
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }

    /* Receive all response lines */
    control_response_t r;
    int rc = 0;
    while (recv(fd, &r, sizeof(r), 0) == (ssize_t)sizeof(r)) {
        printf("%s\n", r.message);
        if (r.status <= 0) {
            rc = (r.status < 0) ? 1 : 0;
            break;
        }
    }

    close(fd);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════
 *  CLI command builders
 * ═══════════════════════════════════════════════════════════════ */
static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s start <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
            argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s run <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
            argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* NEW: stats command */
static int cmd_stats(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stats <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STATS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ═══════════════════════════════════════════════════════════════
 *  Usage + main
 * ═══════════════════════════════════════════════════════════════ */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>                         Start supervisor daemon\n"
        "  %s run   <id> <rootfs> <command> [opts]            Start container\n"
        "  %s start <id> <rootfs> <command> [opts]            Start container\n"
        "  %s ps                                               List containers\n"
        "  %s stats <id>                                       Memory stats\n"
        "  %s logs  <id>                                       Show logs\n"
        "  %s stop  <id>                                       Stop container\n"
        "\n"
        "Options: --soft-mib N  --hard-mib N  --nice N\n"
        "\n"
        "Example workflow:\n"
        "  sudo ./engine supervisor rootfs-base &\n"
        "  sudo ./engine run alpha rootfs-alpha ./memory_hog\n"
        "  sudo ./engine ps\n"
        "  sudo ./engine stats alpha\n"
        "  sudo ./engine stop alpha\n",
        prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "list")  == 0) return cmd_ps();   /* alias */
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);
    if (strcmp(argv[1], "stats") == 0) return cmd_stats(argc, argv);

    usage(argv[0]);
    return 1;
}