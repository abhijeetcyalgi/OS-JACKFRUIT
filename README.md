# OS Jackfruit Container Runtime

## Overview

OS Jackfruit is a lightweight Linux container runtime written in C. It combines a user-space supervisor with a Linux kernel monitoring module to demonstrate container isolation, multi-container lifecycle management, CLI-to-supervisor IPC, bounded-buffer logging, memory-limit enforcement, and scheduler experiments.

The runtime uses Linux namespaces and `chroot` to isolate container processes. A long-running supervisor manages container metadata and receives commands from short-lived CLI clients over a UNIX domain socket. Container output is captured through pipes, passed through a bounded producer-consumer queue, and written to per-container log files. A kernel module tracks registered container PIDs and enforces soft and hard memory limits.

## Team Information

- **ABHIJEET** - PES1UG24CS648
- **DHANUSH S SHEKHAR** - PES1UG24CS662

## Features

- Container isolation using UTS, PID, mount, network, and IPC namespaces
- Long-running supervisor process
- CLI client commands over UNIX domain socket IPC
- Multiple container tracking with metadata
- Per-container stdout/stderr capture
- Bounded-buffer logging with producer and consumer threads
- Kernel module at `/dev/container_monitor`
- PID registration through `ioctl`
- Soft memory-limit warning through `dmesg`
- Hard memory-limit enforcement with `SIGKILL`
- Manual stop classification as `manual_stop`
- Foreground `run` mode with final status reporting
- Scheduler experiment support with `--nice`
- Workload binaries: `memory_hog`, `cpu_hog`, and `io_pulse`

## Project Structure

```text
OSJACKFRUIT/
├── README.md
├── project-guide.md
├── boilerplate/
│   ├── engine.c              # Supervisor, CLI client, container launcher, logging pipeline
│   ├── monitor.c             # Linux kernel module for memory monitoring
│   ├── monitor_ioctl.h       # Shared ioctl request structure and command numbers
│   ├── memory_hog.c          # Memory-pressure workload
│   ├── cpu_hog.c             # CPU-bound scheduler workload
│   ├── io_pulse.c            # I/O-oriented scheduler workload
│   ├── environment-check.sh  # Ubuntu VM preflight checker
│   ├── Makefile              # Builds user programs and kernel module
│   ├── rootfs/               # Minimal root filesystem used by containers
│   ├── logs/                 # Generated per-container logs
│   └── screenshots/          # Demo screenshots
├── rootfs/
└── screenshots/
```

## Environment

Use an Ubuntu VM, not WSL.

Tested target environment:

- Ubuntu 22.04 / 24.04 VM
- Secure Boot disabled
- Kernel headers installed
- Root privileges available for namespaces, `chroot`, `/proc` mounting, and kernel module loading

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Optional preflight:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

## Build

From the runtime directory:

```bash
cd ~/Downloads/OS-Jackfruit-main/boilerplate
make clean
make
```

Expected generated files:

```text
engine
memory_hog
cpu_hog
io_pulse
monitor.ko
```

The kernel build may print warnings such as compiler-version mismatch or skipped BTF generation. These are usually non-fatal if `monitor.ko` is produced.

CI-safe user-space build:

```bash
make -C boilerplate ci
```

The `ci` target builds only user-space binaries and does not require `sudo`, kernel headers, module loading, rootfs setup, or a running supervisor.

## Kernel Module

Load the module:

```bash
sudo insmod monitor.ko
```

Successful `insmod` usually prints no output.

Verify the device:

```bash
ls /dev/container_monitor
```

Expected:

```text
/dev/container_monitor
```

View kernel monitor events:

```bash
sudo dmesg | grep container_monitor
```

Unload at the end:

```bash
sudo rmmod monitor
```

## Runtime Command Contract

Start the supervisor in one terminal:

```bash
sudo ./engine supervisor rootfs
```

Use a second terminal for client commands:

```bash
sudo ./engine start <id> <container-rootfs> <command> [args...] [--soft-mib N] [--hard-mib N] [--nice N]
sudo ./engine run   <id> <container-rootfs> <command> [args...] [--soft-mib N] [--hard-mib N] [--nice N]
sudo ./engine ps
sudo ./engine logs <id>
sudo ./engine stop <id>
sudo ./engine shutdown
```

Defaults:

- Soft limit: `40 MiB`
- Hard limit: `64 MiB`
- Default command when omitted: `/memory_hog`

Important: use a unique container ID for each new container in one supervisor session. The supervisor keeps metadata for exited containers, so old IDs cannot be reused until the supervisor restarts.

Command behavior:

- `start` returns after the supervisor accepts the request and records metadata.
- `run` blocks until the container exits and prints the final state and return code.
- `run` uses the same logging pipeline and log files as `start`.
- `ps` lists all containers known to the current supervisor session.
- `logs <id>` prints the persistent log file for one container.
- `stop <id>` sets `stop_requested`, terminates the container, and records `manual_stop`.
- `shutdown` is an extra helper command that cleanly stops the supervisor.

## Rootfs Copies

Each running container should use a separate writable rootfs copy. A simple clean copy can be created like this:

```bash
mkdir -p rootfs-c1
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs/memory_hog rootfs-c1/
mkdir -p rootfs-c1/proc
```

For CPU workload containers:

```bash
mkdir -p rootfs-cpu1
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs-cpu1/
cp cpu_hog rootfs-cpu1/
mkdir -p rootfs-cpu1/proc
```

Avoid copying a rootfs while its `/proc` is mounted. If needed:

```bash
sudo umount rootfs/proc 2>/dev/null
```

## Example: Hard-Limit Run

```bash
mkdir -p rootfs-hard
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs/memory_hog rootfs-hard/
mkdir -p rootfs-hard/proc

sudo ./engine run hard1 rootfs-hard /memory_hog --soft-mib 40 --hard-mib 64
```

Expected final output:

```text
OK running id=hard1 pid=... log=logs/hard1.log
DONE id=hard1 state=hard_limit_killed reason=hard_limit_or_sigkill rc=137
```

`rc=137` means the process was killed with `SIGKILL`:

```text
128 + 9 = 137
```

## Example: Manual Stop

```bash
mkdir -p rootfs-stop
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs/memory_hog rootfs-stop/
mkdir -p rootfs-stop/proc

sudo ./engine start stop1 rootfs-stop /memory_hog --soft-mib 2000 --hard-mib 3000
sudo ./engine stop stop1
sleep 1
sudo ./engine ps
```

Expected metadata:

```text
stop1 ... stopped ... manual_stop ... logs/stop1.log
```

The runtime sets an internal `stop_requested` flag before signaling the process, so the final reason is classified as `manual_stop` instead of `hard_limit_killed`.

## Example: Scheduler Experiment

```bash
mkdir -p rootfs-cpu1 rootfs-cpu2
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs-cpu1/
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs-cpu2/
cp cpu_hog rootfs-cpu1/
cp cpu_hog rootfs-cpu2/
mkdir -p rootfs-cpu1/proc rootfs-cpu2/proc

sudo ./engine start cpu1 rootfs-cpu1 /cpu_hog 20 --nice 0 --soft-mib 200 --hard-mib 300
sudo ./engine start cpu2 rootfs-cpu2 /cpu_hog 20 --nice 10 --soft-mib 200 --hard-mib 300
sudo ./engine ps
sudo ./engine logs cpu1
sudo ./engine logs cpu2
```

Expected log lines:

```text
cpu_hog alive elapsed=1 accumulator=...
cpu_hog alive elapsed=2 accumulator=...
cpu_hog done duration=20 accumulator=...
```

This demonstrates CPU-bound workloads running under different `nice` configurations.

## Scheduler Experiment Results

The scheduling demonstration used two CPU-bound containers running `cpu_hog` with different nice values:

```bash
sudo ./engine start c17 rootfs-c17 /cpu_hog 120 --nice 0 --soft-mib 200 --hard-mib 300
sudo ./engine start c18 rootfs-c18 /cpu_hog 120 --nice 10 --soft-mib 200 --hard-mib 300
sudo ./engine ps
```

Observed metadata from the demo showed CPU workload containers being tracked by the supervisor and completing normally:

```text
c17 ... /cpu_hog 120 ... exit_code_0 ... logs/c17.log
c18 ... /cpu_hog 120 ... exit_code_0 ... logs/c18.log
```

The log files contained progress lines such as:

```text
cpu_hog alive elapsed=1 accumulator=...
cpu_hog alive elapsed=2 accumulator=...
cpu_hog done duration=120 accumulator=...
```

The `--nice 0` workload used default priority. The `--nice 10` workload lowered its scheduling priority. Linux's Completely Fair Scheduler still allowed both processes to run, but the lower-priority workload is eligible for less CPU time when there is CPU contention. This project uses the runtime as an experiment platform rather than replacing the scheduler.

## Demo Screenshot Commands

The following commands reproduce the eight project-guide screenshots.

### 1. Multi-Container Supervision

```bash
mkdir -p rootfs-s1 rootfs-s2
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs/memory_hog rootfs-s1/
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs/memory_hog rootfs-s2/
mkdir -p rootfs-s1/proc rootfs-s2/proc

sudo ./engine start s1 rootfs-s1 /memory_hog --soft-mib 2000 --hard-mib 3000
sudo ./engine start s2 rootfs-s2 /memory_hog --soft-mib 2000 --hard-mib 3000
sudo ./engine ps
```

### 2. Metadata Tracking

```bash
sudo ./engine ps
```

The output includes:

```text
ID PID STATE SOFT HARD START ROOTFS COMMAND REASON LOG
```

### 3. Container Logging

```bash
sudo ./engine logs s1
```

Expected:

```text
allocation=1 chunk=8MB total=8MB
allocation=2 chunk=8MB total=16MB
```

This output comes from the per-container log file written by the producer-consumer logging pipeline.

### 3.2. Kernel Logging

```bash
sudo dmesg | grep container_monitor
```

Expected:

```text
[container_monitor] Module loaded
[container_monitor] Registered: ...
```

### 4. CLI and IPC

```bash
sudo ./engine ps
sudo ./engine logs s1
sudo ./engine stop s1
sleep 1
sudo ./engine ps
```

This shows that independent CLI commands reach the long-running supervisor through `/tmp/mini_runtime.sock`.

### 5. Soft-Limit Warning

```bash
mkdir -p rootfs-soft
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs/memory_hog rootfs-soft/
mkdir -p rootfs-soft/proc

sudo ./engine start soft1 rootfs-soft /memory_hog --soft-mib 40 --hard-mib 200
sleep 8
sudo dmesg | grep container_monitor
```

Expected:

```text
SOFT LIMIT EXCEEDED: soft1
```

### 6. Hard-Limit Enforcement

```bash
mkdir -p rootfs-hard
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs/memory_hog rootfs-hard/
mkdir -p rootfs-hard/proc

sudo ./engine run hard1 rootfs-hard /memory_hog --soft-mib 40 --hard-mib 64
sudo dmesg | grep container_monitor
```

Expected:

```text
DONE id=hard1 state=hard_limit_killed reason=hard_limit_or_sigkill rc=137
HARD LIMIT EXCEEDED: hard1
```

### 7. Scheduling Experiment

```bash
mkdir -p rootfs-cpu1 rootfs-cpu2
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs-cpu1/
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs-cpu2/
cp cpu_hog rootfs-cpu1/
cp cpu_hog rootfs-cpu2/
mkdir -p rootfs-cpu1/proc rootfs-cpu2/proc

sudo ./engine start cpu1 rootfs-cpu1 /cpu_hog 20 --nice 0 --soft-mib 200 --hard-mib 300
sudo ./engine start cpu2 rootfs-cpu2 /cpu_hog 20 --nice 10 --soft-mib 200 --hard-mib 300
sleep 3
sudo ./engine logs cpu1
sudo ./engine logs cpu2
sudo ./engine ps
```

### 8. Clean Teardown

```bash
sudo ./engine stop s2
sudo ./engine stop soft1
sudo ./engine stop cpu1
sudo ./engine stop cpu2
sleep 1

sudo ./engine shutdown
ps aux | grep defunct
sudo rmmod monitor
sudo dmesg | grep container_monitor
```

Expected:

```text
[container_monitor] Module unloaded
```

The `grep defunct` command itself may appear. There should be no real `engine`, `memory_hog`, or `cpu_hog` zombie processes.

## Implementation Details

### User-Space Runtime

[engine.c](boilerplate/engine.c) acts as both supervisor and CLI client:

- `supervisor` creates `/tmp/mini_runtime.sock` and waits for commands.
- `start` and `run` are short-lived clients that send requests to the supervisor.
- The supervisor launches containers with `clone()`.
- Containers use `CLONE_NEWUTS`, `CLONE_NEWPID`, `CLONE_NEWNS`, `CLONE_NEWNET`, and `CLONE_NEWIPC`.
- The child performs `chroot()`, `chdir("/")`, mounts `/proc`, and then `execv()`s the requested command.
- A reaper thread uses `waitpid()` to collect exited children and update metadata.

### Logging Pipeline

Each container has stdout and stderr redirected to pipes. Producer threads read from these pipes and push log chunks into a bounded queue protected by a mutex and condition variables. A consumer thread drains the queue and writes to `logs/<id>.log`.

This prevents direct terminal writes from containers and provides persistent per-container logs.

Synchronization choices:

- The log queue is protected by a `pthread_mutex_t`.
- Producers wait on `log_not_full` when the bounded queue is full.
- The consumer waits on `log_not_empty` when there is no log data.
- Metadata is protected by a separate `containers_mutex`, so container state updates do not race with `ps`, `stop`, or the reaper thread.
- The queue is flushed before foreground `run` reports completion so abrupt container exits do not lose buffered output.

Without these locks and condition variables, concurrent producer threads could corrupt queue indexes, overwrite entries, or race with the consumer while it writes log files. Separating the metadata lock from the log lock also avoids unnecessary blocking between `ps` and high-volume container output.

### Kernel Monitor

[monitor.c](boilerplate/monitor.c) creates `/dev/container_monitor` and accepts:

- `MONITOR_REGISTER`
- `MONITOR_UNREGISTER`

Registered PIDs are stored in a mutex-protected kernel linked list. A periodic timer checks each process RSS:

- If RSS exceeds the soft limit, a warning is logged once.
- If RSS exceeds the hard limit, `SIGKILL` is sent.
- Exited or stale entries are removed.

### Workloads

- `memory_hog`: allocates and touches memory in chunks to grow RSS.
- `cpu_hog`: burns CPU and prints progress once per second.
- `io_pulse`: writes and flushes small bursts to a file.

## Engineering Analysis

### 1. Isolation Mechanisms

The runtime uses Linux namespaces to give each container a separate view of selected kernel resources. `CLONE_NEWPID` gives the child a new PID namespace, so processes inside the container see their own PID tree. `CLONE_NEWUTS` isolates hostname changes. `CLONE_NEWNS` creates a separate mount namespace, allowing the container to mount `/proc` without changing the host mount table. `CLONE_NEWNET` and `CLONE_NEWIPC` isolate network and IPC resources.

Filesystem isolation is provided with `chroot()`, which changes the process's apparent root directory to the selected container rootfs. This is simpler than `pivot_root()` and is sufficient for the project demonstration, but it is not as strong as a production container runtime. All containers still share the same host kernel, CPU scheduler, physical memory, and kernel security boundary.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is useful because short-lived CLI commands cannot remember container state after they exit. The supervisor remains the parent of container processes, stores metadata, receives `SIGCHLD`, and reaps children with `waitpid()`. This prevents zombie processes and lets the runtime update final state, exit code, terminating signal, and reason.

The supervisor also centralizes lifecycle decisions. `start` records metadata and returns immediately, while `run` keeps the client connection open until the reaper thread observes final status. `stop` sets `stop_requested` before signaling the process, allowing the runtime to distinguish manual stops from kernel hard-limit kills.

### 3. IPC, Threads, and Synchronization

The project uses two IPC paths:

- Control IPC: CLI client to supervisor through a UNIX domain socket at `/tmp/mini_runtime.sock`.
- Logging IPC: container stdout/stderr to supervisor through file-descriptor pipes.

The socket path separates user commands from container output. The logging path uses producer threads to read pipes and a consumer thread to write logs. A bounded queue sits between them to absorb bursts while preventing unbounded memory growth.

Race conditions exist around shared metadata and the logging queue. Metadata updates can race with `ps`, `stop`, and reaper updates, so `containers_mutex` protects the container table. Log producers can race with each other and with the consumer, so the queue uses a mutex and condition variables. These primitives avoid lost log chunks, corrupted queue positions, and deadlock when the queue becomes full or empty.

### 4. Memory Management and Enforcement

RSS means resident set size: the amount of a process's memory currently resident in physical RAM. It does not include every virtual address the process has reserved, and it can be affected by shared pages. The `memory_hog` workload touches allocated memory with `memset()`, forcing pages to become resident so RSS grows.

The project uses two policies. A soft limit is advisory: the kernel module logs a warning once when RSS crosses the threshold. A hard limit is enforcing: the kernel module sends `SIGKILL` when RSS exceeds the hard threshold. Enforcement belongs in kernel space for this project because the kernel can inspect process memory accounting directly and kill the task even if user space is delayed, blocked, or untrusted.

### 5. Scheduling Behavior

The scheduler experiment runs CPU-bound workloads with different nice values. Nice values are inputs to Linux scheduling priority: a larger nice value means the process is less favored. Linux still tries to preserve fairness and responsiveness, so lower-priority processes are not necessarily starved, but they receive less CPU time under contention.

The `cpu_hog` logs show progress over time and normal completion. This demonstrates that the runtime can launch controlled workloads and record their behavior while the host scheduler manages CPU sharing.

## Design Decisions and Tradeoffs

| Subsystem | Design Choice | Tradeoff | Justification |
| --- | --- | --- | --- |
| Namespace isolation | Used `clone()` with PID, UTS, mount, network, and IPC namespaces plus `chroot()` | `chroot()` is simpler but weaker than `pivot_root()` | It clearly demonstrates isolation mechanisms required by the project while keeping the implementation understandable |
| Supervisor architecture | One long-running supervisor plus short-lived CLI clients | Supervisor metadata is in memory and resets on restart | Keeps lifecycle management centralized and allows proper reaping, logging, and state tracking |
| Control IPC | UNIX domain socket at `/tmp/mini_runtime.sock` | Only local clients can connect; message format is simple text | UNIX sockets are reliable, easy to demonstrate, and separate from logging pipes |
| Logging IPC | Pipes from container stdout/stderr into producer threads and a bounded queue | More code than direct terminal output | Satisfies bounded-buffer logging and prevents container output from bypassing the supervisor |
| Kernel monitor | Character device plus `ioctl` registration | Requires root privileges and kernel module support | Allows direct RSS checks and hard-limit enforcement in kernel space |
| Scheduling experiments | Used `cpu_hog` with different `--nice` values | Does not measure exact CPU share automatically | Shows observable scheduler behavior with simple commands and logs |

## Common Issues

### `sudo insmod monitor.ko` prints nothing

That means success. Check:

```bash
ls /dev/container_monitor
sudo dmesg | grep container_monitor
```

### `ERR container id already exists`

The supervisor keeps metadata for exited containers. Use a new ID or restart the supervisor.

### `ERR rootfs already used by a running container`

Each live container needs a unique rootfs directory.

### `cp -a rootfs rootfs-copy` prints `/proc` permission errors

`rootfs/proc` may still be mounted. Use a clean copy method:

```bash
mkdir -p rootfs-copy
cp -a rootfs/bin rootfs/lib rootfs/lib64 rootfs/memory_hog rootfs-copy/
mkdir -p rootfs-copy/proc
```

### `stop` says container is not running

The container may have already exited or been killed by the hard limit. Check:

```bash
sudo ./engine ps
```

## Authors

- **ABHIJEET** - PES1UG24CS648
- **DHANUSH S SHEKHAR** - PES1UG24CS662
