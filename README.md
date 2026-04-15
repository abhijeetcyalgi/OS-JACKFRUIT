# OS Jackfruit Container Runtime

## 📌 Overview

This project implements a lightweight container runtime using Linux namespaces and a kernel monitoring module. It supports container creation, supervision, resource monitoring, and enforcement of memory limits.

---

## 🚀 Features

* **Container Isolation**

  * UTS, PID, Mount, Network, IPC namespaces
* **Supervisor Process**

  * Manages multiple containers
* **Kernel Monitoring Module**

  * Tracks container lifecycle
  * Logs events via `dmesg`
* **Memory Limits**

  * Soft limit warning
  * Hard limit enforcement (kills container)
* **CLI Interface**

  * Start, list, and stop containers
* **Stress Tools**

  * `memory_hog`, `cpu_hog`, `io_pulse`

---

## 📂 Project Structure

```
boilerplate/
│
├── engine.c            # Container runtime
├── monitor.c           # Kernel module
├── memory_hog.c        # Memory stress tool
├── cpu_hog.c           # CPU stress tool
├── io_pulse.c          # IO stress tool
├── Makefile
└── rootfs/             # Container filesystem
```

---

## ⚙️ Build Instructions

```bash
make clean
make
```

---

## 🔧 Load Kernel Module

```bash
sudo insmod monitor.ko
```

Check:

```bash
ls /dev/container_monitor
```

---

## 📦 Setup Root Filesystem

```bash
mkdir -p rootfs/bin rootfs/lib rootfs/lib64
cp /bin/sh rootfs/bin/

# Copy required libraries
ldd /bin/sh
cp /lib/x86_64-linux-gnu/libc.so.6 rootfs/lib/
cp /lib64/ld-linux-x86-64.so.2 rootfs/lib64/
```

Copy stress tool:

```bash
cp memory_hog rootfs/
chmod +x rootfs/memory_hog
```

---

## ▶️ Run Container

```bash
sudo ./engine start c1 rootfs
```

---

## 📊 View Logs

```bash
sudo dmesg | grep container_monitor
```

---

## 🧪 Demo Steps

### 1. Multi-container

```bash
sudo ./engine start c1 rootfs
sudo ./engine start c2 rootfs
```

---

### 2. Metadata

```bash
./engine ps
```

---

### 3. Logging

```bash
sudo dmesg | grep container_monitor
```

---

### 4. CLI + IPC

```bash
./engine supervisor
```

---

### 5. Soft Limit

Run container → observe:

```
SOFT LIMIT EXCEEDED
```

---

### 6. Hard Limit

Continue execution → observe:

```
HARD LIMIT EXCEEDED
Process exited
```

---

### 7. Scheduling

```bash
./cpu_hog &
./cpu_hog &
```

---

### 8. Clean Teardown

```bash
ps aux | grep defunct
sudo rmmod monitor
```

---

## 📸 Screenshot Requirements

| # | Requirement                 |
| - | --------------------------- |
| 1 | Multi-container supervision |
| 2 | Metadata tracking           |
| 3 | Logging                     |
| 4 | CLI & IPC                   |
| 5 | Soft-limit warning          |
| 6 | Hard-limit enforcement      |
| 7 | Scheduling experiment       |
| 8 | Clean teardown              |

---

## ⚠️ Notes

* Run container commands using `sudo`
* Kernel logs require root access
* Memory limits are enforced by kernel module
* No zombie processes should remain after execution

---

## 🎯 Conclusion

This project demonstrates:

* Container isolation using Linux namespaces
* Kernel-level monitoring and enforcement
* Process lifecycle management
* Resource control and scheduling behavior

---

## 👨‍💻 Authors

* **ABHIJEET** (SRN: PES1UG24CS648)
* **DHANUSH S** (SRN: PES1UG24CS662)

---
