# OS Jackfruit Container Runtime

## Team Information

* **Abhijeet** — PES1UG24CS648
* **Dhanush S** — PES1UG24CS662

---

# Project Overview

OS Jackfruit is a lightweight container runtime with a kernel-space memory monitor.
It supports multi-container execution, memory limit enforcement, logging, and scheduling workloads.

Features implemented:

* Container runtime using Linux namespaces
* Kernel memory monitor (LKM)
* Soft memory limit warning
* Hard memory limit enforcement (kill)
* Multi-container supervision
* CLI-based execution
* Scheduling workload tests
* Clean teardown and logging

---

# Build Instructions

Navigate to boilerplate directory:

```bash
cd OS-Jackfruit/boilerplate
make
```

This builds:

* engine (user-space runtime)
* monitor.ko (kernel module)

---

# Load Kernel Module

```bash
sudo insmod monitor.ko
```

Verify device:

```bash
ls /dev/jackfruit_monitor
```

---

# Run Container

```bash
sudo ./engine ../rootfs-alpha
```

You will enter container shell:

```
/ #
```

---

# Run Memory Workload

Inside container:

```bash
./memory_hog
```

This will trigger:

* soft limit warning
* hard limit kill

---

# Multi Container Execution

Terminal 1:

```bash
sudo ./engine ../rootfs-alpha
```

Terminal 2:

```bash
sudo ./engine ../rootfs-beta
```

This demonstrates multi-container supervision.

---

# Metadata Tracking

Run:

```bash
ps aux | grep engine
```

This shows running container processes.

---

# Scheduling Experiment

Run CPU workload:

```bash
./cpu_hog
```

Observe CPU scheduling behavior.

---

# Kernel Logs

Check monitor logs:

```bash
sudo dmesg | grep jackfruit
```

---

# Stop Container

Inside container:

```bash
exit
```

---

# Unload Kernel Module

```bash
sudo rmmod monitor
```

---

# Workloads Included

* memory_hog.c — memory pressure generator
* cpu_hog.c — CPU scheduling workload

---

# Files Included

* engine.c — user runtime
* monitor.c — kernel memory monitor
* monitor_ioctl.h — ioctl interface
* Makefile — build system
* memory_hog.c — memory workload
* cpu_hog.c — CPU workload
* README.md — documentation

---

# Clean Build

```bash
make clean
make
```

---

# Environment

* Ubuntu 22.04 / 24.04 VM
* Linux Kernel 6.x
* GCC 12
* VirtualBox / VMware
