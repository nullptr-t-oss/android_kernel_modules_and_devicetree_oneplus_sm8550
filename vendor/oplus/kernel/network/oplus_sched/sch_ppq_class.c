#include "sch_ppq.h"

static int ppq_graft(struct Qdisc *sch, unsigned long cl, struct Qdisc *new_q,
                     struct Qdisc **old_q, struct netlink_ext_ack *extack)
{
    struct ppq_sched_data *q = qdisc_priv(sch);

    if (cl - 1 >= (unsigned long)q->num_classes)
        return -EINVAL;

    if (!new_q) {
        new_q = qdisc_create_dflt(sch->dev_queue, &pfifo_qdisc_ops,
                                  TC_H_MAKE(sch->handle, cl), extack);
        if (!new_q)
            new_q = &noop_qdisc;
        else
            qdisc_hash_add(new_q, true);
    }

    /* sch_tree_lock + swap + purge old (qlen/backlog propagated up). */
    *old_q = qdisc_replace(sch, new_q, &q->queues[cl - 1]);
    return 0;
}

static struct Qdisc *ppq_leaf(struct Qdisc *sch, unsigned long cl)
{
    struct ppq_sched_data *q = qdisc_priv(sch);

    if (cl - 1 >= (unsigned long)q->num_classes)
        return NULL;
    return q->queues[cl - 1];
}

static unsigned long ppq_find(struct Qdisc *sch, u32 classid)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    unsigned long cl = TC_H_MIN(classid);

    if (cl - 1 >= (unsigned long)q->num_classes)
        return 0;
    return cl;
}

static unsigned long ppq_bind(struct Qdisc *sch, unsigned long parent,
                              u32 classid)
{
    return ppq_find(sch, classid);
}

static void ppq_unbind(struct Qdisc *sch, unsigned long cl)
{
}

static struct tcf_block *ppq_tcf_block(struct Qdisc *sch, unsigned long cl,
                                       struct netlink_ext_ack *extack)
{
    struct ppq_sched_data *q = qdisc_priv(sch);

    if (cl)
        return NULL;
    return q->block;
}

static void ppq_walk(struct Qdisc *sch, struct qdisc_walker *arg)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    int i;

    if (arg->stop)
        return;

    for (i = 0; i < q->num_classes; i++) {
        if (arg->count < arg->skip) {
            arg->count++;
            continue;
        }
        if (arg->fn(sch, i + 1, arg) < 0) {
            arg->stop = 1;
            break;
        }
        arg->count++;
    }
}

static int ppq_dump_class(struct Qdisc *sch, unsigned long cl,
                          struct sk_buff *skb, struct tcmsg *tcm)
{
    struct ppq_sched_data *q = qdisc_priv(sch);

    if (cl - 1 >= (unsigned long)q->num_classes)
        return -EINVAL;

    tcm->tcm_handle |= TC_H_MIN(cl);
    tcm->tcm_info = q->queues[cl - 1]->handle;
    return 0;
}

static int ppq_dump_class_stats(struct Qdisc *sch, unsigned long cl,
                                struct gnet_dump *d)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    struct Qdisc *cl_q;

    if (cl - 1 >= (unsigned long)q->num_classes)
        return -EINVAL;

    cl_q = q->queues[cl - 1];
    if (gnet_stats_copy_basic(qdisc_root_sleeping_running(sch), d,
                              cl_q->cpu_bstats, &cl_q->bstats) < 0 ||
        qdisc_qstats_copy(d, cl_q) < 0)
        return -1;

    return 0;
}

const struct Qdisc_class_ops ppq_class_ops = {
    .graft      = ppq_graft,
    .leaf       = ppq_leaf,
    .find       = ppq_find,
    .walk       = ppq_walk,
    .tcf_block  = ppq_tcf_block,
    .bind_tcf   = ppq_bind,
    .unbind_tcf = ppq_unbind,
    .dump       = ppq_dump_class,
    .dump_stats = ppq_dump_class_stats,
};
