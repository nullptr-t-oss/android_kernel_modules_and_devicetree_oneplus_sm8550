#include "sch_ppq.h"

#ifdef OPLUS_SCHED_DEBUG

#include <linux/debugfs.h>
#include <linux/seq_file.h>

/*
 * /sys/kernel/debug/ppq/<dev>_<handle-major-hex>
 *   cat   : counters and live bucket state
 *   write : any write clears the counters
 *
 * Readers take the same lock enqueue/dequeue/tune run under. They never take
 * RTNL, so ppq_destroy() (which holds RTNL) can debugfs_remove() safely.
 */

static struct dentry *ppq_dbg_root;

static spinlock_t *ppq_dbg_lock(struct Qdisc *sch)
{
    return qdisc_lock(qdisc_root_sleeping(sch));
}

static int ppq_dbg_show(struct seq_file *m, void *unused)
{
    struct Qdisc *sch = m->private;
    struct ppq_sched_data *q = qdisc_priv(sch);
    struct ppq_dbg *d = &q->dbg;
    spinlock_t *lock = ppq_dbg_lock(sch);
    int i;

    spin_lock_bh(lock);

    seq_printf(m, "dev %s  handle %x:\n", qdisc_dev(sch)->name,
               TC_H_MAJ(sch->handle) >> 16);
    seq_printf(m, "config   classes=%d rate=%llu B/s (prio=%llu fair=%llu) max_burst=%lld ns\n",
               q->num_classes, q->rate_limit,
               q->rate_prio.rate_bytes_ps, q->rate_fair.rate_bytes_ps,
               q->max_burst);
    seq_printf(m, "state    prio_tokens=%lld fair_tokens=%lld rr_cursor=%d\n",
               q->prio_tokens, q->fair_tokens, q->rr_cursor);
    seq_printf(m, "lows     prio_tokens=%lld fair_tokens=%lld (floor %lld)\n",
               d->min_prio_tokens, d->min_fair_tokens, 1 - q->max_burst);
    seq_printf(m, "classify direct=%llu filter=%llu default=%llu\n",
               d->cls_direct, d->cls_filter, d->cls_default);
    seq_printf(m, "enqueue  ok=%llu child_drop=%llu cls_drop=%llu stolen=%llu\n",
               d->enq_ok, d->enq_child_drop, d->enq_cls_drop, d->enq_stolen);
    seq_printf(m, "dequeue  gated=%llu empty=%llu watchdog=%llu max_wd_delay=%lld ns\n",
               d->deq_gated, d->deq_empty, d->wd_armed, d->max_wd_delay_ns);
    seq_printf(m, "delta    nonpos=%llu clamped=%llu\n",
               d->delta_nonpos, d->delta_clamped);
    seq_printf(m, "bytes    prio=%llu fair=%llu\n", d->bytes_prio, d->bytes_fair);

    seq_puts(m, "class    qlen   backlog        hit   deq_prio   deq_fair\n");
    for (i = 0; i < q->num_classes; i++) {
        struct Qdisc *c = q->queues[i];

        seq_printf(m, "%5d %7u %9u %10llu %10llu %10llu\n", i + 1,
                   c ? (unsigned int)qdisc_qlen_sum(c) : 0U,
                   c ? (unsigned int)c->qstats.backlog : 0U,
                   d->cls_hit[i], d->deq_prio[i], d->deq_fair[i]);
    }

    spin_unlock_bh(lock);
    return 0;
}

static int ppq_dbg_open(struct inode *inode, struct file *file)
{
    return single_open(file, ppq_dbg_show, inode->i_private);
}

static ssize_t ppq_dbg_write(struct file *file, const char __user *ubuf,
                             size_t len, loff_t *off)
{
    struct seq_file *m = file->private_data;
    struct Qdisc *sch = m->private;
    spinlock_t *lock = ppq_dbg_lock(sch);

    spin_lock_bh(lock);
    memset(PPQ_DBG(sch), 0, sizeof(struct ppq_dbg));
    spin_unlock_bh(lock);
    return len;
}

static const struct file_operations ppq_dbg_fops = {
    .owner   = THIS_MODULE,
    .open    = ppq_dbg_open,
    .read    = seq_read,
    .write   = ppq_dbg_write,
    .llseek  = seq_lseek,
    .release = single_release,
};

void ppq_dbg_attach(struct Qdisc *sch)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    char name[IFNAMSIZ + 16];

    if (!ppq_dbg_root)
        return;

    snprintf(name, sizeof(name), "%s_%x", qdisc_dev(sch)->name,
             TC_H_MAJ(sch->handle) >> 16);
    q->dbg_dentry = debugfs_create_file(name, 0600, ppq_dbg_root, sch,
                                        &ppq_dbg_fops);
}

void ppq_dbg_detach(struct Qdisc *sch)
{
    struct ppq_sched_data *q = qdisc_priv(sch);

    debugfs_remove(q->dbg_dentry);   /* waits for in-flight readers; NULL/ERR ok */
    q->dbg_dentry = NULL;
}

void ppq_dbg_module_init(void)
{
    ppq_dbg_root = debugfs_create_dir("ppq", NULL);
    if (IS_ERR(ppq_dbg_root))
        ppq_dbg_root = NULL;
    pr_info("ppq: debug build, stats in debugfs ppq/\n");
}

void ppq_dbg_module_exit(void)
{
    debugfs_remove_recursive(ppq_dbg_root);
    ppq_dbg_root = NULL;
}

#endif /* OPLUS_SCHED_DEBUG */