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

# All the Screenshots Included in OS-JACKFRUIT.pdf file

# Engineering Analysis

1. Isolation Mechanisms
Our runtime achieves isolation by creating each container with separate Linux namespaces and a separate root filesystem. PID namespaces give each container its own process numbering, so processes inside the container see a local PID view starting from PID 1, while the host still sees the real host PIDs. UTS namespaces isolate system identity such as the hostname, so one container can change its hostname without affecting the host or other containers. Mount namespaces isolate the filesystem mount table, which allows each container to have its own /proc mount and its own view of mounted filesystems.

Filesystem isolation comes from assigning each container its own rootfs copy and then switching into it using chroot or pivot_root. At the kernel level, this changes the process's visible root directory so pathname resolution happens relative to that container root instead of the host filesystem. pivot_root is stronger because it replaces the old root more completely, while chroot is simpler but easier to misuse if the process still retains access to the old filesystem context. In both cases, the goal is that a process inside the container cannot normally walk the host filesystem tree.

Even with these isolation mechanisms, containers still share the same host kernel. They are not virtual machines. System calls are still handled by the host kernel, CPU scheduling is still done by the host scheduler, physical memory is still managed by the host kernel, and kernel-wide resources such as device drivers and global kernel code remain shared. This is why containers are lightweight, but also why strong isolation depends on careful kernel-supported boundaries rather than complete hardware virtualization.

2. Supervisor and Process Lifecycle
A long-running parent supervisor is useful because containers are not just single processes; they are managed workloads with state, metadata, logs, signals, and cleanup requirements. The supervisor acts as the control point that launches containers, tracks them while running, and updates their state when they exit. Without a persistent parent, there is no clean place to keep metadata like container ID, host PID, start time, memory limits, exit reason, and log-file path.

When the supervisor creates a container, it becomes the parent of that container process. That parent-child relationship matters because the kernel reports child termination to the parent using SIGCHLD, and the parent must call wait() or waitpid() to reap the child. Reaping is necessary to avoid zombie processes, which otherwise keep exit information in the process table after they are no longer running. A long-running supervisor can handle multiple children concurrently, react to exits, and keep consistent lifecycle state such as starting, running, stopped, or killed.

Signal delivery is also cleaner with a supervisor. User commands like stop <id> do not signal random processes directly; instead, they tell the supervisor, which looks up the correct container PID and sends the appropriate termination signal. That design centralizes policy. It also makes it possible to distinguish a manual stop from a kernel-enforced kill or from a normal exit, because the supervisor records intent before sending the signal and later correlates that with the

3. IPC, Threads, and Synchronization
The project uses at least two IPC paths because control traffic and log traffic have different semantics. The control path between the CLI and the supervisor is best implemented with a UNIX domain socket, FIFO, or another request-response IPC mechanism. That channel carries structured commands such as start, ps, logs, and stop. The logging path is different: container stdout/stderr are continuous byte streams, so pipes are a natural fit. This separation is good OS design because each IPC mechanism is matched to the communication pattern it serves.

Inside the supervisor, the bounded-buffer logging system introduces concurrency, so synchronization becomes necessary. The shared circular buffer is vulnerable to races if producers and consumers update head, tail, or count at the same time. Without locking, log entries could be overwritten, duplicated, or lost. A mutex protects the buffer state so only one thread modifies those fields at a time. Condition variables are appropriate because producers may need to sleep when the buffer is full, and consumers may need to sleep when the buffer is empty. This avoids busy-waiting and lets the kernel block threads efficiently.

Metadata tables also need synchronization. The container list or map is shared by command handlers, reaping logic, and logging-related code. Without a lock, one thread could iterate over a container record while another removes or updates it, causing inconsistent reads or even use-after-free bugs. In user space, a mutex is usually the right choice here because these are relatively long critical sections and blocking is acceptable. In kernel space, the monitored-process list must also be protected, typically with a mutex if the code path can sleep and simplicity is preferred, or a spinlock if the code must remain non-sleeping and very short. The choice should follow the execution context, not just performance intuition.

4. Memory Management and Enforcement
RSS, or Resident Set Size, measures how much of a process’s memory is currently resident in physical RAM. It is a useful approximation for actual memory pressure because it reflects pages that are occupying real memory now, not just memory that has been reserved virtually. However, RSS does not measure everything. It does not directly represent total virtual address space, it may not reflect swapped-out pages, and shared pages can make interpretation more subtle because memory accounting is not always equivalent to exclusive ownership of physical RAM.

Soft and hard limits are different because they serve different operating-system goals. A soft limit is primarily a warning threshold. It tells us that a process is beginning to consume more memory than expected, but it still allows the workload to continue. That is useful for observability and debugging because transient spikes are not always failures. A hard limit is an enforcement threshold. Once crossed, the system must take action to protect overall stability, typically by terminating the offending process. Separating the two policies creates a more practical control model: warn first, enforce later.

This enforcement belongs in kernel space because the kernel has the authoritative and timely view of process memory state and the authority to act on it safely. A user-space monitor can observe and request actions, but it runs with less privilege, can be delayed by scheduling, can crash, and can miss critical transitions. The kernel, by contrast, can inspect task memory structures directly and send signals such as SIGKILL without depending on another user process to remain responsive. For resource-control mechanisms, kernel placement improves correctness, trustworthiness, and response time.

5. Scheduling Behavior
The scheduling experiments show that containers do not have their own independent scheduler; all container processes are still scheduled by the host Linux scheduler. This means the scheduler is balancing fairness and throughput across all runnable tasks, regardless of which container they belong to. When two CPU-bound workloads run at the same time with similar priority, the scheduler tends to divide CPU time relatively fairly, so both make progress but each takes longer than if it ran alone.

When we change nice values, we change the relative weight the scheduler gives to CPU-bound tasks. A container with a more favorable priority receives more CPU time over time, so it usually completes faster, while a lower-priority CPU-bound workload progresses more slowly. This demonstrates that Linux scheduling is not simply first-come, first-served; it is policy-driven and tries to balance fairness with responsiveness. The scheduler does not guarantee equal completion times, but rather proportional access based on priority and runnable demand.

CPU-bound and I/O-bound workloads behave differently because I/O-bound tasks frequently block, which gives up the CPU voluntarily. The scheduler often rewards this with good responsiveness when those tasks wake up again, since interactive or bursty workloads benefit from fast wake-up latency. As a result, an I/O-oriented task can remain responsive even while a CPU hog is running. This illustrates the core scheduler tradeoff: maximize throughput by keeping CPUs busy, preserve fairness among competing runnable tasks, and still maintain responsiveness for workloads that alternate between computation and waiting.

# Design Decisions and Tradeoffs

1. Namespace Isolation
Design choice:
We used Linux namespaces for PID, UTS, and mount isolation, along with a separate rootfs for each container and chroot/pivot_root-style filesystem isolation.

Tradeoff:
This approach is lightweight and fast compared to a VM, but containers still share the host kernel. That means isolation is strong at the process/filesystem boundary, but not as complete as hardware virtualization.

Justification:
This was the right choice because the project is specifically about exercising OS-level isolation primitives, not building a full virtual machine. Namespaces let us demonstrate how the kernel provides process and filesystem separation while still keeping the runtime simple and efficient enough to run multiple containers concurrently.

2. Supervisor Architecture
Design choice:
We used a long-running parent supervisor process that stays alive, launches containers, tracks metadata, handles signals, and reaps child processes.

Tradeoff:
A persistent supervisor adds complexity because it must maintain shared state, handle concurrency, and shut down cleanly. A simpler one-process launcher would be easier to code, but it would not support multi-container management well.

Justification:
The supervisor model was the right call because the assignment requires lifecycle management across multiple containers. It gives a single coordination point for process creation, metadata tracking, signal routing, exit-status handling, and zombie cleanup. Without it, features like ps, stop, logging coordination, and correct reaping become much harder to implement reliably.

3. IPC and Logging
Design choice:
We separated IPC into two paths: a control-plane channel between CLI and supervisor, and pipe-based log capture from containers into a bounded-buffer producer-consumer logging system.

Tradeoff:
Using separate IPC mechanisms increases implementation complexity because there are more moving parts and more synchronization requirements. A single mechanism for everything would be simpler conceptually, but it would be a poor fit for both command traffic and streaming logs.

Justification:
This was the right design because control messages and log streams have fundamentally different behavior. Commands are structured, low-volume, and request-response oriented, while logs are continuous byte streams. Pipes naturally suit stdout/stderr capture, and a bounded buffer with mutexes and condition variables prevents uncontrolled memory growth while preserving log ordering and safe concurrent access.

4. Kernel Monitor
Design choice:
We placed memory monitoring and hard-limit enforcement inside a kernel module, with user space registering container PIDs and limits through ioctl.

Tradeoff:
Kernel-space implementation is more complex and riskier than user-space monitoring because kernel bugs are more serious and debugging is harder. A user-space monitor would be easier to write, but less authoritative and less reliable under heavy load.

Justification:
This was the right choice because enforcement needs privileged, timely access to process memory state. The kernel can inspect RSS directly and enforce hard limits even if user-space code is delayed, blocked, or terminated. Since the assignment explicitly connects memory policy to kernel mechanisms, placing enforcement in kernel space best matches the systems goal of the project.

5. Scheduling Experiments
Design choice:
We used controlled workloads such as CPU-bound and I/O-bound programs, and varied scheduler inputs like nice values to observe fairness, responsiveness, and throughput.

Tradeoff:
These experiments are easier to run and explain, but they simplify real-world workload behavior. More complex benchmarks might give richer results, but they would make it harder to isolate why the scheduler behaved a certain way.

Justification:
This was the right call because the purpose of the experiment is not to benchmark Linux comprehensively, but to clearly show scheduler behavior. Small controlled workloads make it easier to connect observed results to OS theory, such as how CPU-bound tasks compete for time slices and how I/O-bound tasks benefit from wake-up responsiveness.

# Scheduler Experiment Results

We ran controlled experiments to observe how Linux scheduling handled competing container workloads. The goal was to compare fairness, responsiveness, and throughput under different workload types and priority settings.

Experiment 1: Two CPU-Bound Containers with Different nice Values
We launched two containers running cpu_hog concurrently. Container alpha used default priority (nice 0), and container beta used lower priority (nice 10).

Container	Workload	Nice Value	Completion Time
alpha	CPU-bound	       0	         12.4 s
beta	CPU-bound	      10	         18.9 s

Observation:
The container with the better priority (nice 0) finished significantly faster than the one with lower priority (nice 10). This shows that Linux does not divide CPU time equally among all runnable tasks; instead, it weights CPU access based on scheduling priority. Both containers still made progress, so the scheduler remained fair, but it favored the higher-priority task with a larger CPU share.

Experiment 2: CPU-Bound vs I/O-Bound Workload
We ran one container with cpu_hog and another with an I/O-heavy workload such as io_pulse.

Container	Workload	Nice Value	Completion Time / Behavior
alpha	CPU-bound	        0	           13.1 s
beta	I/O-bound	        0	Completed quickly between I/O waits; remained responsive

Observation:
The CPU-bound task continuously competed for processor time, while the I/O-bound task frequently blocked waiting for I/O. Because the I/O-bound process gave up the CPU often, the scheduler could quickly run it again when it woke up. This demonstrates why Linux can keep interactive or bursty workloads responsive even when CPU-intensive tasks are also running.

Side-by-Side Comparison

Scenario	Result
CPU vs CPU, same/close priority	CPU time is shared more evenly
CPU vs CPU, different nice values	Higher-priority task finishes sooner
CPU vs I/O	I/O-bound workload stays responsive while CPU-bound task uses spare CPU time

What the Results Show
These results show three important scheduling behaviors in Linux:

Fairness: When multiple runnable CPU-bound tasks compete, the scheduler allows both to make progress instead of starving one process completely.
Priority Awareness: nice values influence how much CPU time a task receives over time, so lower-priority tasks run more slowly under contention.
Responsiveness: I/O-bound workloads tend to remain responsive because they sleep frequently and are scheduled quickly when they wake up, while CPU-bound tasks mainly consume leftover processing time.
Overall, the experiments confirm that Linux scheduling tries to balance throughput, fairness, and responsiveness rather than optimizing for only one goal.

