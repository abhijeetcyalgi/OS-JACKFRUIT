#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 64
#define MAX_ID_LEN 64
#define MAX_STATE_LEN 32
#define MAX_REASON_LEN 64
#define MAX_COMMAND_LEN 256
#define MAX_ROOTFS_LEN 256
#define DEFAULT_SOFT_MIB 40ULL
#define DEFAULT_HARD_MIB 64ULL
#define SOCKET_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define LOG_QUEUE_CAPACITY 128
#define LOG_CHUNK_SIZE 512
#define CLIENT_READ_SIZE 4096

struct child_config {
    char id[MAX_ID_LEN];
    char rootfs[MAX_ROOTFS_LEN];
    char command[MAX_COMMAND_LEN];
    int stdout_fd;
    int stderr_fd;
    int nice_value;
};

struct container {
    char id[MAX_ID_LEN];
    char rootfs[MAX_ROOTFS_LEN];
    char command[MAX_COMMAND_LEN];
    char state[MAX_STATE_LEN];
    char reason[MAX_REASON_LEN];
    char log_path[PATH_MAX];
    pid_t pid;
    time_t start_time;
    uint64_t soft_limit_bytes;
    uint64_t hard_limit_bytes;
    int exit_code;
    int term_signal;
    int stop_requested;
    int wait_client_fd;
    void *stack;
    pthread_t stdout_thread;
    pthread_t stderr_thread;
    int stdout_thread_started;
    int stderr_thread_started;
};

struct log_entry {
    char id[MAX_ID_LEN];
    char path[PATH_MAX];
    size_t len;
    char data[LOG_CHUNK_SIZE];
};

struct producer_arg {
    int fd;
    char id[MAX_ID_LEN];
    char path[PATH_MAX];
};

static struct container containers[MAX_CONTAINERS];
static int container_count;
static pthread_mutex_t containers_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct log_entry log_queue[LOG_QUEUE_CAPACITY];
static size_t log_head;
static size_t log_tail;
static size_t log_count;
static int log_shutdown;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t log_not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t log_empty = PTHREAD_COND_INITIALIZER;
static pthread_t log_consumer_thread;
static pthread_t reaper_thread;
static volatile sig_atomic_t supervisor_stop;
static int server_fd = -1;

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, dst_size, "%s", src);
}

static void fd_printf(int fd, const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len < 0)
        return;
    if ((size_t)len >= sizeof(buf))
        len = (int)sizeof(buf) - 1;
    if (write(fd, buf, (size_t)len) < 0)
        return;
}

static void write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
}

static uint64_t mib_to_bytes(unsigned long long mib)
{
    return mib * 1024ULL * 1024ULL;
}

static void usage(void)
{
    printf("Usage:\n");
    printf("  ./engine supervisor <base-rootfs>\n");
    printf("  ./engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n");
    printf("  ./engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n");
    printf("  ./engine ps\n");
    printf("  ./engine logs <id>\n");
    printf("  ./engine stop <id>\n");
}

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static int ensure_proc_dir(const char *rootfs)
{
    char proc_path[PATH_MAX];

    if (snprintf(proc_path, sizeof(proc_path), "%s/proc", rootfs) >= (int)sizeof(proc_path))
        return -1;
    if (mkdir(proc_path, 0555) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static int monitor_ioctl_request(unsigned int cmd, pid_t pid, const char *id,
                                 uint64_t soft_limit, uint64_t hard_limit)
{
    struct monitor_request req;
    int fd;
    int ret;

    memset(&req, 0, sizeof(req));
    req.pid = (int32_t)pid;
    safe_copy(req.container_id, sizeof(req.container_id), id);
    req.soft_limit_bytes = soft_limit;
    req.hard_limit_bytes = hard_limit;

    fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0)
        return -1;

    ret = ioctl(fd, cmd, &req);
    close(fd);
    return ret;
}

static int find_container_index_locked(const char *id)
{
    int i;

    for (i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, id) == 0)
            return i;
    }
    return -1;
}

static int find_container_by_pid_locked(pid_t pid)
{
    int i;

    for (i = 0; i < container_count; i++) {
        if (containers[i].pid == pid)
            return i;
    }
    return -1;
}

static int rootfs_in_use_locked(const char *rootfs)
{
    int i;

    for (i = 0; i < container_count; i++) {
        if (strcmp(containers[i].rootfs, rootfs) == 0 &&
            (strcmp(containers[i].state, "running") == 0 ||
             strcmp(containers[i].state, "starting") == 0))
            return 1;
    }
    return 0;
}

static void log_queue_push(const char *id, const char *path, const char *data, size_t len)
{
    size_t remaining = len;
    const char *cursor = data;

    while (remaining > 0) {
        size_t chunk = remaining > LOG_CHUNK_SIZE ? LOG_CHUNK_SIZE : remaining;

        pthread_mutex_lock(&log_mutex);
        while (log_count == LOG_QUEUE_CAPACITY && !log_shutdown)
            pthread_cond_wait(&log_not_full, &log_mutex);
        if (log_shutdown) {
            pthread_mutex_unlock(&log_mutex);
            return;
        }

        safe_copy(log_queue[log_tail].id, sizeof(log_queue[log_tail].id), id);
        safe_copy(log_queue[log_tail].path, sizeof(log_queue[log_tail].path), path);
        memcpy(log_queue[log_tail].data, cursor, chunk);
        log_queue[log_tail].len = chunk;
        log_tail = (log_tail + 1) % LOG_QUEUE_CAPACITY;
        log_count++;

        pthread_cond_signal(&log_not_empty);
        pthread_mutex_unlock(&log_mutex);

        cursor += chunk;
        remaining -= chunk;
    }
}

static void *log_producer_main(void *arg)
{
    struct producer_arg *producer = arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(producer->fd, buf, sizeof(buf))) > 0)
        log_queue_push(producer->id, producer->path, buf, (size_t)n);

    close(producer->fd);
    free(producer);
    return NULL;
}

static void *log_consumer_main(void *arg)
{
    (void)arg;

    for (;;) {
        struct log_entry entry;
        FILE *fp;

        pthread_mutex_lock(&log_mutex);
        while (log_count == 0 && !log_shutdown)
            pthread_cond_wait(&log_not_empty, &log_mutex);
        if (log_count == 0 && log_shutdown) {
            pthread_mutex_unlock(&log_mutex);
            break;
        }

        entry = log_queue[log_head];
        log_head = (log_head + 1) % LOG_QUEUE_CAPACITY;
        log_count--;
        pthread_cond_signal(&log_not_full);
        pthread_mutex_unlock(&log_mutex);

        fp = fopen(entry.path, "ab");
        if (!fp) {
            pthread_mutex_lock(&log_mutex);
            if (log_count == 0)
                pthread_cond_broadcast(&log_empty);
            pthread_mutex_unlock(&log_mutex);
            continue;
        }
        fwrite(entry.data, 1, entry.len, fp);
        fclose(fp);

        pthread_mutex_lock(&log_mutex);
        if (log_count == 0)
            pthread_cond_broadcast(&log_empty);
        pthread_mutex_unlock(&log_mutex);
    }

    return NULL;
}

static void wait_for_log_flush(void)
{
    pthread_mutex_lock(&log_mutex);
    while (log_count > 0)
        pthread_cond_wait(&log_empty, &log_mutex);
    pthread_mutex_unlock(&log_mutex);
}

static int start_producer_thread(pthread_t *thread, int fd, const char *id, const char *path)
{
    struct producer_arg *arg = calloc(1, sizeof(*arg));

    if (!arg)
        return -1;
    arg->fd = fd;
    safe_copy(arg->id, sizeof(arg->id), id);
    safe_copy(arg->path, sizeof(arg->path), path);

    if (pthread_create(thread, NULL, log_producer_main, arg) != 0) {
        close(fd);
        free(arg);
        return -1;
    }
    return 0;
}

static int split_command(char *command, char **argv, size_t argv_cap)
{
    size_t argc = 0;
    char *save = NULL;
    char *token = strtok_r(command, " \t", &save);

    while (token && argc + 1 < argv_cap) {
        argv[argc++] = token;
        token = strtok_r(NULL, " \t", &save);
    }
    argv[argc] = NULL;
    return argc > 0 ? 0 : -1;
}

static int container_main(void *arg)
{
    struct child_config *config = arg;
    char command_copy[MAX_COMMAND_LEN];
    char *child_argv[32];

    if (config->nice_value != 0 && nice(config->nice_value) == -1 && errno != 0)
        perror("nice");

    if (dup2(config->stdout_fd, STDOUT_FILENO) < 0)
        _exit(111);
    if (dup2(config->stderr_fd, STDERR_FILENO) < 0)
        _exit(111);
    close(config->stdout_fd);
    close(config->stderr_fd);

    if (sethostname(config->id, strlen(config->id)) < 0) {
        perror("sethostname");
        _exit(111);
    }
    if (chroot(config->rootfs) < 0) {
        perror("chroot");
        _exit(111);
    }
    if (chdir("/") < 0) {
        perror("chdir");
        _exit(111);
    }
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");

    safe_copy(command_copy, sizeof(command_copy), config->command);
    if (split_command(command_copy, child_argv, sizeof(child_argv) / sizeof(child_argv[0])) < 0)
        _exit(127);

    execv(child_argv[0], child_argv);
    perror("execv");
    _exit(127);
}

static int parse_limits(char **tokens, int count, int option_start, uint64_t *soft_bytes,
                        uint64_t *hard_bytes, int *nice_value)
{
    int i;

    *soft_bytes = mib_to_bytes(DEFAULT_SOFT_MIB);
    *hard_bytes = mib_to_bytes(DEFAULT_HARD_MIB);
    *nice_value = 0;

    for (i = option_start; i < count; i += 2) {
        char *end = NULL;
        long value;

        if (i + 1 >= count)
            return -1;
        value = strtol(tokens[i + 1], &end, 10);
        if (!end || *end != '\0')
            return -1;

        if (strcmp(tokens[i], "--soft-mib") == 0) {
            if (value <= 0)
                return -1;
            *soft_bytes = mib_to_bytes((unsigned long long)value);
        } else if (strcmp(tokens[i], "--hard-mib") == 0) {
            if (value <= 0)
                return -1;
            *hard_bytes = mib_to_bytes((unsigned long long)value);
        } else if (strcmp(tokens[i], "--nice") == 0) {
            if (value < -20 || value > 19)
                return -1;
            *nice_value = (int)value;
        } else {
            return -1;
        }
    }

    return *soft_bytes <= *hard_bytes ? 0 : -1;
}

static void join_command_tokens(char **tokens, int start, int end, char *out, size_t out_size)
{
    int i;
    size_t used = 0;

    if (out_size == 0)
        return;
    out[0] = '\0';

    for (i = start; i < end; i++) {
        int written = snprintf(out + used, out_size - used, "%s%s",
                               i == start ? "" : " ", tokens[i]);

        if (written < 0)
            break;
        if ((size_t)written >= out_size - used) {
            out[out_size - 1] = '\0';
            break;
        }
        used += (size_t)written;
    }
}

static int launch_container(int client_fd, char **tokens, int count, int wait_for_exit)
{
    struct child_config *config;
    struct container *container;
    char command[MAX_COMMAND_LEN];
    uint64_t soft_bytes;
    uint64_t hard_bytes;
    int nice_value;
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int flags;
    pid_t pid;
    void *stack;
    int idx;
    int option_start;

    if (count < 3) {
        fd_printf(client_fd, "ERR usage: %s <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", tokens[0]);
        return -1;
    }
    if (count == 3) {
        safe_copy(command, sizeof(command), "/memory_hog");
        option_start = 3;
    } else {
        option_start = 3;
        while (option_start < count && strncmp(tokens[option_start], "--", 2) != 0)
            option_start++;
        if (option_start == 3) {
            fd_printf(client_fd, "ERR missing command\n");
            return -1;
        }
        join_command_tokens(tokens, 3, option_start, command, sizeof(command));
    }
    if (parse_limits(tokens, count, option_start, &soft_bytes, &hard_bytes, &nice_value) < 0) {
        fd_printf(client_fd, "ERR invalid limits or options\n");
        return -1;
    }
    if (ensure_dir(LOG_DIR) < 0) {
        fd_printf(client_fd, "ERR cannot create logs directory: %s\n", strerror(errno));
        return -1;
    }
    if (ensure_proc_dir(tokens[2]) < 0) {
        fd_printf(client_fd, "ERR cannot create %s/proc: %s\n", tokens[2], strerror(errno));
        return -1;
    }

    pthread_mutex_lock(&containers_mutex);
    if (container_count >= MAX_CONTAINERS) {
        pthread_mutex_unlock(&containers_mutex);
        fd_printf(client_fd, "ERR container table full\n");
        return -1;
    }
    if (find_container_index_locked(tokens[1]) >= 0) {
        pthread_mutex_unlock(&containers_mutex);
        fd_printf(client_fd, "ERR container id already exists\n");
        return -1;
    }
    if (rootfs_in_use_locked(tokens[2])) {
        pthread_mutex_unlock(&containers_mutex);
        fd_printf(client_fd, "ERR rootfs already used by a running container\n");
        return -1;
    }
    idx = container_count++;
    container = &containers[idx];
    memset(container, 0, sizeof(*container));
    safe_copy(container->id, sizeof(container->id), tokens[1]);
    safe_copy(container->rootfs, sizeof(container->rootfs), tokens[2]);
    safe_copy(container->command, sizeof(container->command), command);
    safe_copy(container->state, sizeof(container->state), "starting");
    safe_copy(container->reason, sizeof(container->reason), "none");
    snprintf(container->log_path, sizeof(container->log_path), "%s/%s.log", LOG_DIR, tokens[1]);
    container->soft_limit_bytes = soft_bytes;
    container->hard_limit_bytes = hard_bytes;
    container->wait_client_fd = wait_for_exit ? client_fd : -1;
    pthread_mutex_unlock(&containers_mutex);

    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
        fd_printf(client_fd, "ERR pipe failed: %s\n", strerror(errno));
        goto fail_reserved;
    }

    stack = malloc(STACK_SIZE);
    config = calloc(1, sizeof(*config));
    if (!stack || !config) {
        fd_printf(client_fd, "ERR allocation failed\n");
        free(stack);
        free(config);
        goto fail_reserved;
    }

    safe_copy(config->id, sizeof(config->id), tokens[1]);
    safe_copy(config->rootfs, sizeof(config->rootfs), tokens[2]);
    safe_copy(config->command, sizeof(config->command), command);
    config->stdout_fd = out_pipe[1];
    config->stderr_fd = err_pipe[1];
    config->nice_value = nice_value;

    flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC;
    pid = clone(container_main, (char *)stack + STACK_SIZE, flags | SIGCHLD, config);
    if (pid < 0) {
        fd_printf(client_fd, "ERR clone failed: %s\n", strerror(errno));
        free(stack);
        free(config);
        goto fail_reserved;
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    free(config);

    pthread_mutex_lock(&containers_mutex);
    container = &containers[idx];
    container->pid = pid;
    container->start_time = time(NULL);
    container->stack = stack;
    safe_copy(container->state, sizeof(container->state), "running");
    if (start_producer_thread(&container->stdout_thread, out_pipe[0], container->id, container->log_path) == 0)
        container->stdout_thread_started = 1;
    if (start_producer_thread(&container->stderr_thread, err_pipe[0], container->id, container->log_path) == 0)
        container->stderr_thread_started = 1;
    pthread_mutex_unlock(&containers_mutex);

    if (monitor_ioctl_request(MONITOR_REGISTER, pid, tokens[1], soft_bytes, hard_bytes) < 0) {
        const char *warning = "warning: /dev/container_monitor registration failed\n";
        log_queue_push(tokens[1], containers[idx].log_path, warning, strlen(warning));
    }

    if (!wait_for_exit) {
        fd_printf(client_fd, "OK started id=%s pid=%d soft=%lluMiB hard=%lluMiB log=%s\n",
                  tokens[1], pid,
                  (unsigned long long)(soft_bytes / 1024ULL / 1024ULL),
                  (unsigned long long)(hard_bytes / 1024ULL / 1024ULL),
                  containers[idx].log_path);
    } else {
        fd_printf(client_fd, "OK running id=%s pid=%d log=%s\n", tokens[1], pid, containers[idx].log_path);
    }
    return 0;

fail_reserved:
    if (out_pipe[0] >= 0)
        close(out_pipe[0]);
    if (out_pipe[1] >= 0)
        close(out_pipe[1]);
    if (err_pipe[0] >= 0)
        close(err_pipe[0]);
    if (err_pipe[1] >= 0)
        close(err_pipe[1]);
    pthread_mutex_lock(&containers_mutex);
    if (idx == container_count - 1)
        container_count--;
    pthread_mutex_unlock(&containers_mutex);
    return -1;
}

static void classify_exit(struct container *container, int status)
{
    container->exit_code = -1;
    container->term_signal = 0;

    if (WIFEXITED(status)) {
        container->exit_code = WEXITSTATUS(status);
        safe_copy(container->state, sizeof(container->state), "exited");
        snprintf(container->reason, sizeof(container->reason), "exit_code_%d", container->exit_code);
    } else if (WIFSIGNALED(status)) {
        container->term_signal = WTERMSIG(status);
        if (container->stop_requested) {
            safe_copy(container->state, sizeof(container->state), "stopped");
            safe_copy(container->reason, sizeof(container->reason), "manual_stop");
        } else if (container->term_signal == SIGKILL) {
            safe_copy(container->state, sizeof(container->state), "hard_limit_killed");
            safe_copy(container->reason, sizeof(container->reason), "hard_limit_or_sigkill");
        } else {
            safe_copy(container->state, sizeof(container->state), "killed");
            snprintf(container->reason, sizeof(container->reason), "signal_%d", container->term_signal);
        }
    } else {
        safe_copy(container->state, sizeof(container->state), "stopped");
        safe_copy(container->reason, sizeof(container->reason), "unknown");
    }
}

static void *reaper_main(void *arg)
{
    (void)arg;

    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0) {
            int wait_fd = -1;
            char id[MAX_ID_LEN] = "";
            char state[MAX_STATE_LEN] = "";
            char reason[MAX_REASON_LEN] = "";
            int exit_code = -1;
            int term_signal = 0;
            uint64_t soft = 0;
            uint64_t hard = 0;
            pthread_t stdout_thread;
            pthread_t stderr_thread;
            int join_stdout = 0;
            int join_stderr = 0;
            int idx;
            struct container *container;

            pthread_mutex_lock(&containers_mutex);
            idx = find_container_by_pid_locked(pid);
            if (idx >= 0) {
                container = &containers[idx];
                classify_exit(container, status);
                wait_fd = container->wait_client_fd;
                container->wait_client_fd = -1;
                safe_copy(id, sizeof(id), container->id);
                safe_copy(state, sizeof(state), container->state);
                safe_copy(reason, sizeof(reason), container->reason);
                exit_code = container->exit_code;
                term_signal = container->term_signal;
                soft = container->soft_limit_bytes;
                hard = container->hard_limit_bytes;
                stdout_thread = container->stdout_thread;
                stderr_thread = container->stderr_thread;
                join_stdout = container->stdout_thread_started;
                join_stderr = container->stderr_thread_started;
                container->stdout_thread_started = 0;
                container->stderr_thread_started = 0;
            }
            pthread_mutex_unlock(&containers_mutex);

            if (idx >= 0) {
                if (join_stdout)
                    pthread_join(stdout_thread, NULL);
                if (join_stderr)
                    pthread_join(stderr_thread, NULL);
                wait_for_log_flush();
                monitor_ioctl_request(MONITOR_UNREGISTER, pid, id, soft, hard);
                if (wait_fd >= 0) {
                    int rc = exit_code >= 0 ? exit_code : 128 + term_signal;
                    fd_printf(wait_fd, "DONE id=%s state=%s reason=%s rc=%d\n",
                              id, state, reason, rc);
                    close(wait_fd);
                }
            }
            continue;
        }

        if (pid < 0 && errno == ECHILD && supervisor_stop)
            break;
        if (pid < 0 && errno != ECHILD)
            perror("waitpid");
        usleep(100000);
    }

    return NULL;
}

static void handle_ps(int client_fd)
{
    int i;

    pthread_mutex_lock(&containers_mutex);
    fd_printf(client_fd, "ID\tPID\tSTATE\tSOFT\tHARD\tSTART\tROOTFS\tCOMMAND\tREASON\tLOG\n");
    for (i = 0; i < container_count; i++) {
        char start_buf[32] = "-";
        struct tm tm_info;

        if (containers[i].start_time > 0 &&
            localtime_r(&containers[i].start_time, &tm_info)) {
            strftime(start_buf, sizeof(start_buf), "%Y-%m-%d_%H:%M:%S", &tm_info);
        }

        fd_printf(client_fd, "%s\t%d\t%s\t%lluMiB\t%lluMiB\t%s\t%s\t%s\t%s\t%s\n",
                  containers[i].id,
                  containers[i].pid,
                  containers[i].state,
                  (unsigned long long)(containers[i].soft_limit_bytes / 1024ULL / 1024ULL),
                  (unsigned long long)(containers[i].hard_limit_bytes / 1024ULL / 1024ULL),
                  start_buf,
                  containers[i].rootfs,
                  containers[i].command,
                  containers[i].reason,
                  containers[i].log_path);
    }
    pthread_mutex_unlock(&containers_mutex);
}

static void handle_logs(int client_fd, const char *id)
{
    char path[PATH_MAX] = "";
    char buf[1024];
    FILE *fp;
    size_t n;
    int idx;

    pthread_mutex_lock(&containers_mutex);
    idx = find_container_index_locked(id);
    if (idx >= 0)
        safe_copy(path, sizeof(path), containers[idx].log_path);
    pthread_mutex_unlock(&containers_mutex);

    if (idx < 0) {
        fd_printf(client_fd, "ERR unknown container id\n");
        return;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        fd_printf(client_fd, "ERR cannot open log %s: %s\n", path, strerror(errno));
        return;
    }
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        write_all(client_fd, buf, n);
    fclose(fp);
}

static void handle_stop(int client_fd, const char *id)
{
    pid_t pid = -1;
    int idx;

    pthread_mutex_lock(&containers_mutex);
    idx = find_container_index_locked(id);
    if (idx >= 0 &&
        (strcmp(containers[idx].state, "running") == 0 ||
         strcmp(containers[idx].state, "starting") == 0)) {
        containers[idx].stop_requested = 1;
        safe_copy(containers[idx].state, sizeof(containers[idx].state), "stopping");
        pid = containers[idx].pid;
    }
    pthread_mutex_unlock(&containers_mutex);

    if (idx < 0) {
        fd_printf(client_fd, "ERR unknown container id\n");
        return;
    }
    if (pid <= 0) {
        fd_printf(client_fd, "ERR container is not running\n");
        return;
    }
    if (kill(pid, SIGKILL) < 0) {
        fd_printf(client_fd, "ERR stop failed: %s\n", strerror(errno));
        return;
    }
    fd_printf(client_fd, "OK stopping id=%s pid=%d\n", id, pid);
}

static int tokenize_request(char *request, char **tokens, int max_tokens)
{
    int count = 0;
    char *save = NULL;
    char *token;

    request[strcspn(request, "\r\n")] = '\0';
    token = strtok_r(request, " \t", &save);
    while (token && count < max_tokens) {
        tokens[count++] = token;
        token = strtok_r(NULL, " \t", &save);
    }
    return count;
}

static void handle_client(int client_fd)
{
    char request[CLIENT_READ_SIZE];
    char *tokens[64];
    ssize_t n;
    int count;
    int keep_open = 0;

    n = read(client_fd, request, sizeof(request) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    request[n] = '\0';
    count = tokenize_request(request, tokens, (int)(sizeof(tokens) / sizeof(tokens[0])));

    if (count == 0) {
        fd_printf(client_fd, "ERR empty request\n");
    } else if (strcmp(tokens[0], "start") == 0) {
        launch_container(client_fd, tokens, count, 0);
    } else if (strcmp(tokens[0], "run") == 0) {
        if (launch_container(client_fd, tokens, count, 1) == 0)
            keep_open = 1;
    } else if (strcmp(tokens[0], "ps") == 0) {
        handle_ps(client_fd);
    } else if (strcmp(tokens[0], "logs") == 0 && count >= 2) {
        handle_logs(client_fd, tokens[1]);
    } else if (strcmp(tokens[0], "stop") == 0 && count >= 2) {
        handle_stop(client_fd, tokens[1]);
    } else if (strcmp(tokens[0], "shutdown") == 0) {
        supervisor_stop = 1;
        fd_printf(client_fd, "OK supervisor shutting down\n");
    } else {
        fd_printf(client_fd, "ERR unknown command\n");
    }

    if (!keep_open)
        close(client_fd);
}

static void signal_handler(int sig)
{
    (void)sig;
    supervisor_stop = 1;
    if (server_fd >= 0)
        close(server_fd);
}

static void stop_all_containers(void)
{
    int i;

    pthread_mutex_lock(&containers_mutex);
    for (i = 0; i < container_count; i++) {
        if (strcmp(containers[i].state, "running") == 0 ||
            strcmp(containers[i].state, "starting") == 0 ||
            strcmp(containers[i].state, "stopping") == 0) {
            containers[i].stop_requested = 1;
            kill(containers[i].pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&containers_mutex);
}

static void force_kill_remaining(void)
{
    int i;

    pthread_mutex_lock(&containers_mutex);
    for (i = 0; i < container_count; i++) {
        if (strcmp(containers[i].state, "running") == 0 ||
            strcmp(containers[i].state, "starting") == 0 ||
            strcmp(containers[i].state, "stopping") == 0) {
            kill(containers[i].pid, SIGKILL);
        }
    }
    pthread_mutex_unlock(&containers_mutex);
}

static int supervisor_main(const char *base_rootfs)
{
    struct sockaddr_un addr;

    (void)base_rootfs;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    if (ensure_dir(LOG_DIR) < 0)
        die("mkdir logs");

    unlink(SOCKET_PATH);
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0)
        die("socket");

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_copy(addr.sun_path, sizeof(addr.sun_path), SOCKET_PATH);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind");
    if (listen(server_fd, 32) < 0)
        die("listen");

    if (pthread_create(&log_consumer_thread, NULL, log_consumer_main, NULL) != 0)
        die("pthread_create log consumer");
    if (pthread_create(&reaper_thread, NULL, reaper_main, NULL) != 0)
        die("pthread_create reaper");

    printf("Supervisor ready on %s (base rootfs: %s)\n", SOCKET_PATH, base_rootfs);
    fflush(stdout);

    while (!supervisor_stop) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR || supervisor_stop)
                break;
            perror("accept");
            continue;
        }
        handle_client(client_fd);
    }

    stop_all_containers();
    sleep(1);
    force_kill_remaining();
    pthread_join(reaper_thread, NULL);

    pthread_mutex_lock(&log_mutex);
    log_shutdown = 1;
    pthread_cond_broadcast(&log_not_empty);
    pthread_cond_broadcast(&log_not_full);
    pthread_mutex_unlock(&log_mutex);
    pthread_join(log_consumer_thread, NULL);

    unlink(SOCKET_PATH);
    return 0;
}

static volatile sig_atomic_t run_stop_requested;
static char run_container_id[MAX_ID_LEN];

static void run_client_signal(int sig)
{
    (void)sig;
    run_stop_requested = 1;
}

static int connect_to_supervisor(void)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_copy(addr.sun_path, sizeof(addr.sun_path), SOCKET_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int client_send_stop(const char *id)
{
    int fd = connect_to_supervisor();

    if (fd < 0)
        return -1;
    fd_printf(fd, "stop %s\n", id);
    close(fd);
    return 0;
}

static int client_main(int argc, char *argv[])
{
    int fd;
    int i;
    char request[CLIENT_READ_SIZE] = "";
    size_t used = 0;
    char buf[1024];
    ssize_t n;
    int is_run;

    fd = connect_to_supervisor();
    if (fd < 0) {
        fprintf(stderr, "Could not connect to supervisor at %s. Start it with: sudo ./engine supervisor <base-rootfs>\n", SOCKET_PATH);
        return 1;
    }

    is_run = strcmp(argv[1], "run") == 0;
    if (is_run) {
        safe_copy(run_container_id, sizeof(run_container_id), argc > 2 ? argv[2] : "");
        signal(SIGINT, run_client_signal);
        signal(SIGTERM, run_client_signal);
    }

    for (i = 1; i < argc; i++) {
        int written = snprintf(request + used, sizeof(request) - used, "%s%s",
                               i == 1 ? "" : " ", argv[i]);
        if (written < 0 || (size_t)written >= sizeof(request) - used) {
            fprintf(stderr, "Request too long\n");
            close(fd);
            return 1;
        }
        used += (size_t)written;
    }
    snprintf(request + used, sizeof(request) - used, "\n");
    write_all(fd, request, strlen(request));

    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            write_all(STDOUT_FILENO, buf, (size_t)n);
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }

        if (is_run && run_stop_requested) {
            client_send_stop(run_container_id);
            run_stop_requested = 0;
        }
    }

    close(fd);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        return supervisor_main(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0 ||
        strcmp(argv[1], "run") == 0 ||
        strcmp(argv[1], "ps") == 0 ||
        strcmp(argv[1], "logs") == 0 ||
        strcmp(argv[1], "stop") == 0 ||
        strcmp(argv[1], "shutdown") == 0) {
        return client_main(argc, argv);
    }

    usage();
    return 1;
}
