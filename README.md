# 🐳 OS Jackfruit — Lightweight Container Runtime in C

A minimal multi-container runtime and supervisor built in C to explore core OS concepts like process isolation, resource monitoring, and adaptive memory management.

---

## 🚀 Overview

* 🧱 Containers via `fork`, namespaces, and `chroot`
* 📊 Memory monitoring using Linux `/proc`
* 🧠 Compression-inspired memory handling (`SIGSTOP` + `madvise`)
* 🔄 Persistent container state (`/tmp/engine_state`)
* ⚙️ Supervisor for automated resource control

---

## 📁 Structure

```text
├── engine.c        # Runtime + CLI + supervisor
├── monitor.c       # /proc-based helpers
├── monitor_ioctl.h # Shared structures
├── cpu_hog.c
├── memory_hog.c
├── io_pulse.c
```

---

## 🧪 Example Workflow

```bash
sudo ./engine run alpha memory_hog
sudo ./engine list
sudo ./engine stats alpha
sudo ./engine compress alpha
sudo ./engine decompress alpha
sudo ./engine stop alpha
```

---

## 🎯 Use Cases

* OS/system design learning
* Resource monitoring experiments
* Lightweight container execution

---
