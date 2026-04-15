#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MONITOR_NAME_LEN 64

struct monitor_request {
    __s32 pid;
    char container_id[MONITOR_NAME_LEN];
    __u64 soft_limit_bytes;
    __u64 hard_limit_bytes;
};

/* IOCTL commands */
#define MONITOR_REGISTER   _IOW('M', 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW('M', 2, struct monitor_request)

#endif
