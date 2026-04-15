#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 5

MODULE_LICENSE("GPL");

/* ================= STRUCT ================= */

struct monitored_entry {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int soft_warned;
    struct list_head list;
};

/* ================= GLOBAL ================= */

static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(list_mutex);

static dev_t dev_number;
static struct cdev monitor_cdev;

static struct class *monitor_class;
static struct device *monitor_device;

static struct timer_list monitor_timer;

/* ================= HELPERS ================= */

static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    long rss = -1;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm) {
        rss = get_mm_rss(task->mm) << PAGE_SHIFT;
    }
    rcu_read_unlock();

    return rss;
}

static void kill_process(const char *container_id, pid_t pid,
                         unsigned long limit, unsigned long usage)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        printk(KERN_INFO
               "[container_monitor] HARD LIMIT EXCEEDED: %s pid=%d usage=%lu limit=%lu\n",
               container_id, pid, usage, limit);

        send_sig(SIGKILL, task, 0);
    }
    rcu_read_unlock();
}

static void log_soft_limit_event(const char *container_id, pid_t pid,
                                unsigned long limit, unsigned long usage)
{
    printk(KERN_INFO
           "[container_monitor] SOFT LIMIT EXCEEDED: %s pid=%d usage=%lu limit=%lu\n",
           container_id, pid, usage, limit);
}

/* ================= TIMER ================= */

static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;

    mutex_lock(&list_mutex);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {

        long rss = get_rss_bytes(entry->pid);

        if (rss < 0) {
            printk(KERN_INFO "[container_monitor] Process exited: %s pid=%d\n",
                   entry->container_id, entry->pid);

            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if ((unsigned long)rss >= entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);

            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (!entry->soft_warned &&
            (unsigned long)rss >= entry->soft_limit_bytes) {

            log_soft_limit_event(entry->container_id,
                                 entry->pid,
                                 entry->soft_limit_bytes,
                                 rss);

            entry->soft_warned = 1;
        }
    }

    mutex_unlock(&list_mutex);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ================= IOCTL ================= */

static long monitor_ioctl(struct file *file,
                          unsigned int cmd,
                          unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {

        struct monitored_entry *entry;

        if (req.soft_limit_bytes > req.hard_limit_bytes)
            return -EINVAL;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        strncpy(entry->container_id, req.container_id, MONITOR_NAME_LEN);
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned = 0;

        mutex_lock(&list_mutex);
        list_add(&entry->list, &monitored_list);
        mutex_unlock(&list_mutex);

        printk(KERN_INFO "[container_monitor] Registered: %s pid=%d\n",
               req.container_id, req.pid);
    }

    else if (cmd == MONITOR_UNREGISTER) {

        struct monitored_entry *entry, *tmp;

        mutex_lock(&list_mutex);

        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid) {

                printk(KERN_INFO "[container_monitor] Unregistered: %s pid=%d\n",
                       req.container_id, req.pid);

                list_del(&entry->list);
                kfree(entry);
                break;
            }
        }

        mutex_unlock(&list_mutex);
    }

    else {
        return -EINVAL;
    }

    return 0;
}

/* ================= FILE OPS ================= */

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ================= INIT ================= */

static int __init monitor_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);
    if (ret < 0)
        return ret;

    cdev_init(&monitor_cdev, &fops);
    cdev_add(&monitor_cdev, dev_number, 1);

    monitor_class = class_create(DEVICE_NAME);
    monitor_device = device_create(monitor_class, NULL,
                                  dev_number, NULL, DEVICE_NAME);

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded\n");

    return 0;
}

/* ================= EXIT ================= */

static void __exit monitor_exit(void)
{
    struct monitored_entry *entry, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_mutex);

    device_destroy(monitor_class, dev_number);
    class_destroy(monitor_class);
    cdev_del(&monitor_cdev);
    unregister_chrdev_region(dev_number, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
