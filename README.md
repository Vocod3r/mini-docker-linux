# mini-docker-linux

A lightweight Linux container runtime built from scratch in C — implementing process isolation, namespace separation, memory monitoring via `/proc`, and a supervisor daemon architecture. Built as a minimal Docker alternative without any external dependencies.

---

## What This Is

This project implements a working container runtime entirely in user-space C. It demonstrates the core concepts behind container engines like Docker:

- Process isolation using Linux namespaces (`CLONE_NEWNS`, `CLONE_NEWPID`, `CLONE_NEWUTS`)
- Filesystem isolation via `chroot` into an Alpine Linux rootfs
- A long-running supervisor daemon that manages container lifecycle
- Memory monitoring by reading `/proc/<pid>/status` directly
- Per-container logging via pipes and a bounded producer-consumer buffer
- Soft and hard memory limits with automatic enforcement

No kernel module is required. All monitoring is done through the `/proc` filesystem.

---

## Project Structure

```
.
├── engine.c          # Supervisor daemon + CLI client
├── monitor.c         # /proc-based memory monitor (user-space library)
├── monitor_ioctl.h   # Shared data structures and constants
├── cpu_hog.c         # CPU-bound test workload
├── memory_hog.c      # Memory-consuming test workload
├── io_pulse.c        # I/O-bound test workload
├── Makefile          # Build system
└── logs/             # Per-container log files (created at runtime)
```

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   CLI Commands                       │
│   run / ps / stats / logs / stop                    │
└────────────────────┬────────────────────────────────┘
                     │ Unix Domain Socket
                     │ /tmp/mini_runtime.sock
┌────────────────────▼────────────────────────────────┐
│              Supervisor Daemon                       │
│                                                     │
│  ┌─────────────┐   ┌──────────────┐                │
│  │  Container  │   │  Logging     │                │
│  │  Registry   │   │  Thread      │                │
│  │  (in-memory)│   │  (bounded    │                │
│  └──────┬──────┘   │   buffer)    │                │
│         │          └──────┬───────┘                │
│         │                 │                         │
│  ┌──────▼─────────────────▼───────┐                │
│  │     /proc Monitor Loop         │                │
│  │  reads VmRSS, VmSize, State    │                │
│  └────────────────────────────────┘                │
└─────────────────────────────────────────────────────┘
         │ clone() + namespaces + chroot
┌────────▼──────────────────────────────┐
│          Container Process            │
│  (isolated PID + mount + UTS)         │
│  running: cpu_hog / memory_hog /      │
│           io_pulse / custom command   │
└───────────────────────────────────────┘
```

### How it works

1. `./engine supervisor` starts a long-running daemon that binds a Unix socket
2. All other commands (`run`, `ps`, `stats`, `logs`, `stop`) are clients that connect to this socket
3. The supervisor uses `clone()` to create isolated container processes
4. A pipe connects each container's stdout/stderr to a bounded buffer
5. A logging thread drains the buffer and writes to `logs/<id>.log`
6. Every 2 seconds the supervisor polls `/proc/<pid>/status` and enforces memory limits

---

## Requirements

- Ubuntu 22.04 or 24.04 (VM recommended, Secure Boot OFF)
- **Not WSL** — namespace isolation requires a real Linux kernel
- GCC, Make, pthreads

```bash
sudo apt update
sudo apt install -y build-essential gcc make
```

---

## Setup

### 1. Clone the repo

```bash
git clone https://github.com/Vocod3r/mini-docker-linux.git
cd mini-docker-linux
```

### 2. Build

```bash
make
```

### 3. Set up the root filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

> Do not commit `rootfs-*` directories to the repository.

---

## Usage

All commands require `sudo`.

### Start the supervisor (Terminal 1)

```bash
sudo ./engine supervisor rootfs-base
```

The supervisor runs in the foreground. Keep this terminal open. Use `&` to background it if needed.

### Run a container (Terminal 2)

```bash
sudo ./engine run alpha rootfs-alpha ./memory_hog
sudo ./engine run beta  rootfs-beta  ./cpu_hog
```

### List running containers

```bash
sudo ./engine ps
```

Output:
```
ID              PID       STATE       RSS(KB)   STARTED
----        --------  ----------  -------   --------
alpha           1234      running     45320     14:22:01
beta            1235      running      2048     14:22:05
```

### Memory stats for a container

```bash
sudo ./engine stats alpha
```

Output:
```
Container : alpha
PID       : 1234
State     : running
RSS       : 45320 KB  (44.3 MB)
VM Size   : 98304 KB  (96.0 MB)
Swap      : 0 KB
Soft Limit: 40 MB
Hard Limit: 64 MB
```

### View container logs

```bash
sudo ./engine logs alpha
```

### Stop a container

```bash
sudo ./engine stop alpha
```

### Custom memory limits

```bash
sudo ./engine run alpha rootfs-alpha ./memory_hog --soft-mib 50 --hard-mib 100
```

### Custom nice value (scheduling priority)

```bash
sudo ./engine run alpha rootfs-alpha ./cpu_hog --nice 10
```

---

## Workloads

| Binary | Description |
|---|---|
| `./memory_hog` | Allocates 10 MB every 2 seconds, touches every page |
| `./cpu_hog` | Saturates one CPU core with a busy loop |
| `./io_pulse` | Repeatedly writes and reads a 1 MB temp file |

---

## Memory Monitoring and Limits

The supervisor polls `/proc/<pid>/status` every 2 seconds and reads:

| Field | Source | Meaning |
|---|---|---|
| `VmRSS` | `/proc/<pid>/status` | Actual RAM in use |
| `VmSize` | `/proc/<pid>/status` | Total virtual memory |
| `VmSwap` | `/proc/<pid>/status` | Memory swapped to disk |
| `State` | `/proc/<pid>/status` | R/S/T/Z/D process state |

### Policy

| Condition | Action |
|---|---|
| RSS > soft limit (default 40 MB) | Log warning |
| RSS > hard limit (default 64 MB) | Send SIGKILL |
| Process no longer in `/proc` | Mark as exited, clean up |

---

## Logging Architecture

Container output flows through a pipeline:

```
container stdout/stderr
        │
        │ pipe
        ▼
pipe_reader_thread  →  bounded_buffer (16 slots)  →  logging_thread  →  logs/<id>.log
```

- The bounded buffer is a fixed-size circular queue protected by a mutex and two condition variables (`not_empty`, `not_full`)
- The logging thread blocks on `not_empty` and drains chunks to disk
- On shutdown, `bounded_buffer_begin_shutdown()` wakes all blocked threads cleanly

---

## Design Decisions

### Why Unix domain sockets for IPC?
Each CLI invocation is a fresh process — there is no shared memory between `./engine run` and `./engine ps`. A Unix socket lets the supervisor hold all state in memory and serve any number of clients without race conditions or state files.

### Why `/proc` instead of a kernel module?
Reading `/proc/<pid>/status` gives accurate RSS, VM, swap, and process state without requiring kernel headers, `insmod`, or Secure Boot disabled. It works on any Linux system including VMs and cloud instances.

### Why `clone()` instead of `fork() + exec()`?
`clone()` lets us pass namespace flags (`CLONE_NEWNS`, `CLONE_NEWPID`, `CLONE_NEWUTS`) directly at process creation, which is exactly what container runtimes like `runc` do. `fork()` would require a separate `unshare()` call inside the child.

### Why a bounded buffer for logging?
A bounded buffer decouples the container (fast producer) from disk writes (slow consumer). Without it, a slow disk or a log flood from one container could block the supervisor's main loop.

---

## Cleanup

```bash
# Stop all containers first
sudo ./engine stop alpha
sudo ./engine stop beta

# Then Ctrl-C the supervisor

# Clean build artefacts
make clean
```
#### made by Vipin V Lokesh (AM321) and Aariz Dudekula (AM330)

---

## Acknowledgements

Built as part of the OS-Jackfruit systems project. Inspired by the architecture of `runc`, Docker's containerd, and Linux's own `cgroups` + namespace subsystems.
