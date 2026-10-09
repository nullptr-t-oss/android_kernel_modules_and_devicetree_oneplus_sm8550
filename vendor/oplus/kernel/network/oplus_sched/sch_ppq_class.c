#include "sch_ppq.h"

void ppq_reset(struct Qdisc *sch)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    int i;

    qdisc_watchdog_cancel(&priv->watchdog);
    for (i = 0; i < priv->num_classes; i++) {
        if (priv->queues[i])
            qdisc_reset(priv->queues[i]);
    }
}

static int ppq_graft(struct Qdisc *sch, unsigned long cl, struct Qdisc *new_q,
                     struct Qdisc **old_q, struct netlink_ext_ack *extack)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    struct Qdisc *old;

    if (cl - 1 >= 16)
        return -EINVAL;

    if (new_q == NULL) {
        new_q = qdisc_create_dflt(sch->dev_queue, &pfifo_qdisc_ops,
                                  TC_H_MAKE(sch->handle, cl), extack);
        if (new_q == NULL)
            new_q = &noop_qdisc;
        else
            qdisc_hash_add(new_q, true);
    }

    qdisc_class_lock(sch);
    old = priv->queues[cl - 1];
    priv->queues[cl - 1] = new_q;

    if (old != NULL) {
        struct gnet_stats_queue stats;
        qdisc_qstats_qlen_backlog(old, &stats.qlen, &stats.backlog);
        qdisc_reset(old);
        qdisc_tree_reduce_backlog(old, stats.qlen, stats.backlog);
    }
    qdisc_class_unlock(sch);

    *old_q = old;
    return 0;
}

static struct Qdisc *ppq_leaf(struct Qdisc *sch, unsigned long cl)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    if (cl - 1 >= priv->num_classes)
        return NULL;
    return priv->queues[cl - 1];
}

static unsigned long ppq_bind_tcf(struct Qdisc *sch, unsigned long parent, u32 classid)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    unsigned long cl = classid & 0xffff;
    if (cl - 1 >= priv->num_classes)
        return 0;
    return cl;
}

static void ppq_unbind_tcf(struct Qdisc *sch, unsigned long cl)
{
    /* No cleanup required */
}

static struct tcf_block *ppq_tcf_block(struct Qdisc *sch, unsigned long cl,
                                       struct netlink_ext_ack *extack)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    if (cl != 0)
        return NULL;
    return priv->block;
}

static void ppq_walk(struct Qdisc *sch, struct qdisc_walker *arg)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    unsigned long i;

    if (arg->stop || priv->num_classes <= 0)
        return;

    for (i = 0; i < priv->num_classes; i++) {
        if (arg->skip <= arg->count) {
            if (arg->fn(sch, i + 1, arg) < 0) {
                arg->stop = 1;
                return;
            }
        }
        arg->count++;
    }
}

static int ppq_dump_class(struct Qdisc *sch, unsigned long cl,
                          struct sk_buff *skb, struct tcmsg *tcm)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    
    if (cl - 1 >= priv->num_classes)
        return -EINVAL;

    tcm->tcm_handle |= TC_H_MIN(cl);
    if (priv->queues[cl - 1])
        tcm->tcm_info = priv->queues[cl - 1]->handle;

    return 0;
}

static int ppq_dump_class_stats(struct Qdisc *sch, unsigned long cl, 
                                struct gnet_dump *d)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    struct Qdisc *cl_q;

    if (cl - 1 >= priv->num_classes)
        return -EINVAL;

    cl_q = priv->queues[cl - 1];
    if (!cl_q)
        return -EINVAL;

    if (gnet_stats_copy_basic(qdisc_root_sleeping_running(sch), d, 
                              cl_q->cpu_bstats, &cl_q->bstats) < 0)
        return -1;

    if (gnet_stats_copy_queue(d, cl_q->cpu_qstats, &cl_q->qstats, 
                              qdisc_qlen_sum(cl_q)) < 0)
        return -1;

    return 0;
}

const struct Qdisc_class_ops ppq_class_ops = {
    .graft          = ppq_graft,
    .leaf           = ppq_leaf,
    .get            = ppq_bind_tcf,
    .put            = ppq_unbind_tcf,
    .walk           = ppq_walk,
    .tcf_block      = ppq_tcf_block,
    .bind_tcf       = ppq_bind_tcf,
    .unbind_tcf     = ppq_unbind_tcf,
    .dump           = ppq_dump_class,
    .dump_stats     = ppq_dump_class_stats,
};