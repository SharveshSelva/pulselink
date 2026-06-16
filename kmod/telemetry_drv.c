// SPDX-License-Identifier: GPL-2.0
/*
 * telemetry_drv.c — PulseLink /dev/telemetry character device (Stage 3, M6).
 *
 * telemetryd's "dev" sink write(2)s telemetry_records in; any number of readers
 * (telemetry_cat) read(2) them out, oldest first, blocking until data arrives.
 *
 * Concurrency: a single spinlock guards the ring. The critical section is a
 * bounded O(1) struct copy with no sleeping, and every operation that *can*
 * sleep or fault (copy_to_user / copy_from_user) happens OUTSIDE the lock — so a
 * spinlock is both correct and cheaper than a mutex here. A wait queue turns an
 * empty ring into a blocking read; writers wake it. The ring drops the OLDEST
 * record when full, because a slow reader should fall behind on stale data, not
 * stall the producer (the opposite trade-off from the firmware's bounded queue,
 * for the opposite reason).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "telemetry_uapi.h"
#include "tring.h"

#define REC_SZ   (sizeof(struct telemetry_record))
#define BATCH    32                         /* records moved per read() call */

static struct tring          g_ring;
static DEFINE_SPINLOCK(g_lock);
static DECLARE_WAIT_QUEUE_HEAD(g_readq);

/* ---- write: server -> kernel ------------------------------------------- */
static ssize_t tlm_write(struct file *f, const char __user *ubuf,
                         size_t len, loff_t *off)
{
    size_t nrec = len / REC_SZ;
    size_t i;

    if (nrec == 0)
        return -EINVAL;                      /* must write whole records */

    for (i = 0; i < nrec; i++) {
        struct telemetry_record rec;
        if (copy_from_user(&rec, ubuf + i * REC_SZ, REC_SZ))   /* may fault */
            return -EFAULT;
        spin_lock(&g_lock);
        tring_push(&g_ring, &rec);           /* O(1), no sleeping */
        spin_unlock(&g_lock);
    }

    wake_up_interruptible(&g_readq);
    *off += nrec * REC_SZ;
    return (ssize_t)(nrec * REC_SZ);
}

/* ---- read: kernel -> reader (blocking) --------------------------------- */
static ssize_t tlm_read(struct file *f, char __user *ubuf,
                        size_t len, loff_t *off)
{
    struct telemetry_record batch[BATCH];
    size_t maxrec = len / REC_SZ;
    unsigned n = 0, want;

    if (maxrec == 0)
        return -EINVAL;                      /* buffer too small for a record */

    spin_lock(&g_lock);
    while (tring_count(&g_ring) == 0) {
        spin_unlock(&g_lock);
        if (f->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(g_readq, tring_count(&g_ring) > 0))
            return -ERESTARTSYS;             /* interrupted by a signal */
        spin_lock(&g_lock);
    }

    want = maxrec < BATCH ? (unsigned)maxrec : BATCH;
    while (n < want && tring_pop(&g_ring, &batch[n]))
        n++;
    spin_unlock(&g_lock);

    if (copy_to_user(ubuf, batch, n * REC_SZ))   /* outside the lock */
        return -EFAULT;
    *off += n * REC_SZ;
    return (ssize_t)(n * REC_SZ);
}

/* ---- poll: lets readers select()/epoll() on the device ----------------- */
static __poll_t tlm_poll(struct file *f, struct poll_table_struct *wait)
{
    __poll_t mask = 0;

    poll_wait(f, &g_readq, wait);
    spin_lock(&g_lock);
    if (tring_count(&g_ring) > 0)
        mask |= POLLIN | POLLRDNORM;
    spin_unlock(&g_lock);
    return mask;
}

/* ---- open: mark the device non-seekable (modern replacement for the removed
 *      no_llseek; works across old and new kernels) -------------------------- */
static int tlm_open(struct inode *ino, struct file *f)
{
    return nonseekable_open(ino, f);
}

/* ---- ioctl: query / reset the ring counters (M7) ----------------------- */
static long tlm_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case PL_IOC_GET_STATS: {
        struct pl_stats s;
        spin_lock(&g_lock);
        s.produced = g_ring.produced;
        s.dropped  = g_ring.dropped;
        s.in_ring  = tring_count(&g_ring);
        s.capacity = TRING_CAP;
        spin_unlock(&g_lock);
        if (copy_to_user((void __user *)arg, &s, sizeof s))
            return -EFAULT;
        return 0;
    }
    case PL_IOC_RESET:
        spin_lock(&g_lock);
        tring_init(&g_ring);
        spin_unlock(&g_lock);
        return 0;
    default:
        return -ENOTTY;
    }
}

static const struct file_operations tlm_fops = {
    .owner          = THIS_MODULE,
    .open           = tlm_open,
    .read           = tlm_read,
    .write          = tlm_write,
    .poll           = tlm_poll,
    .unlocked_ioctl = tlm_ioctl,
    .compat_ioctl   = tlm_ioctl,
};

/* ---- /proc/pulselink: human-readable status (M7) ----------------------- */
static int tlm_proc_show(struct seq_file *m, void *v)
{
    unsigned long long produced, dropped;
    unsigned in_ring;

    spin_lock(&g_lock);
    produced = g_ring.produced;
    dropped  = g_ring.dropped;
    in_ring  = tring_count(&g_ring);
    spin_unlock(&g_lock);

    seq_printf(m, "device      : /dev/%s\n", PL_TELEMETRY_DEVNAME);
    seq_printf(m, "capacity    : %d records\n", TRING_CAP);
    seq_printf(m, "record_size : %zu bytes\n", REC_SZ);
    seq_printf(m, "in_ring     : %u\n", in_ring);
    seq_printf(m, "produced    : %llu\n", produced);
    seq_printf(m, "dropped     : %llu\n", dropped);
    return 0;
}

static int tlm_proc_open(struct inode *ino, struct file *f)
{
    return single_open(f, tlm_proc_show, NULL);
}

static const struct proc_ops tlm_proc_ops = {
    .proc_open    = tlm_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static struct miscdevice tlm_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = PL_TELEMETRY_DEVNAME,           /* /dev/telemetry */
    .fops  = &tlm_fops,
    .mode  = 0666,
};

static int __init tlm_init(void)
{
    int ret;
    tring_init(&g_ring);
    ret = misc_register(&tlm_misc);
    if (ret) {
        pr_err("pulselink: misc_register failed: %d\n", ret);
        return ret;
    }
    if (!proc_create("pulselink", 0444, NULL, &tlm_proc_ops)) {
        pr_err("pulselink: proc_create failed\n");
        misc_deregister(&tlm_misc);
        return -ENOMEM;
    }
    pr_info("pulselink: /dev/%s ready (ring capacity %d records, %zu bytes each)\n",
            PL_TELEMETRY_DEVNAME, TRING_CAP, REC_SZ);
    return 0;
}

static void __exit tlm_exit(void)
{
    remove_proc_entry("pulselink", NULL);
    misc_deregister(&tlm_misc);
    pr_info("pulselink: /dev/%s removed (produced=%llu dropped=%llu)\n",
            PL_TELEMETRY_DEVNAME, g_ring.produced, g_ring.dropped);
}

module_init(tlm_init);
module_exit(tlm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PulseLink");
MODULE_DESCRIPTION("PulseLink telemetry character device (record ring with blocking read)");
MODULE_VERSION("0.7");
