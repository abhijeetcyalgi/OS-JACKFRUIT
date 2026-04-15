#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdint.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 10

static char container_stack[STACK_SIZE];

struct container {
    int pid;
    char id[64];
};

struct container containers[MAX_CONTAINERS];
int container_count = 0;

struct container_config {
    char rootfs[256];
    char id[64];
};

/* ================= CONTAINER ================= */

static int container_main(void *arg)
{
    struct container_config *config = arg;

    sethostname(config->id, strlen(config->id));
    chroot(config->rootfs);
    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

 
    char *const args[] = {"/memory_hog", NULL};
    execv("/memory_hog", args);

    return 0;
}

/* ================= START ================= */

void start_container(const char *id, const char *rootfs)
{
    struct container_config config;
    strcpy(config.rootfs, rootfs);
    strcpy(config.id, id);

    int flags =
        CLONE_NEWUTS |
        CLONE_NEWPID |
        CLONE_NEWNS |
        CLONE_NEWNET |
        CLONE_NEWIPC;

    int pid = clone(
        container_main,
        container_stack + STACK_SIZE,
        flags | SIGCHLD,
        &config
    );

    if (pid < 0) {
        perror("clone failed");
        exit(1);
    }

    printf("Container %s started with PID %d\n", id, pid);

    /* Save metadata */
    containers[container_count].pid = pid;
    strcpy(containers[container_count].id, id);
    container_count++;

    /* Register with kernel */
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("open device failed");
        return;
    }

    struct monitor_request req;

    req.pid = (int32_t)pid;
    strncpy(req.container_id, id, MONITOR_NAME_LEN);

    req.soft_limit_bytes = (uint64_t)(50ULL * 1024 * 1024);
    req.hard_limit_bytes = (uint64_t)(100ULL * 1024 * 1024);

    if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
        perror("ioctl register failed");
    } else {
        printf("Registered container with monitor\n");
    }

    close(fd);
}

/* ================= PS ================= */

void list_containers()
{
    printf("Running containers:\n");
    for (int i = 0; i < container_count; i++) {
        printf("ID: %s  PID: %d\n",
               containers[i].id,
               containers[i].pid);
    }
}

/* ================= MAIN ================= */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("./engine start <id> <rootfs>\n");
        printf("./engine ps\n");
        return 1;
    }

    if (strcmp(argv[1], "start") == 0 && argc >= 4) {
        start_container(argv[2], argv[3]);
        return 0;
    }

    if (strcmp(argv[1], "ps") == 0) {
        list_containers();
        return 0;
    }

    printf("Unknown command\n");
    return 0;
}
