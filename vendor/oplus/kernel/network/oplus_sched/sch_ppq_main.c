#include "sch_ppq.h"

static int ppq_tune(struct Qdisc *sch, struct nlattr *opt,
                    struct netlink_ext_ack *extack)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    struct tc_ratespec conf = { .linklayer = TC_LINKLAYER_ETHERNET };
    struct Qdisc *qs[PPQ_MAX_CLASSES] = { NULL };
    struct ppq_tc_opt *ctl;
    int old_classes, new_classes, i;
    u32 rate;

    if (!opt || nla_len(opt) < sizeof(*ctl))
        return -EINVAL;

    ctl = nla_data(opt);
    new_classes = ctl->classes;
    rate = ctl->rate;
    if (new_classes < 1 || new_classes > PPQ_MAX_CLASSES)
        return -EINVAL;

    old_classes = q->num_classes;

    /* Allocate new children before taking the lock. */
    for (i = old_classes; i < new_classes; i++) {
        qs[i] = qdisc_create_dflt(sch->dev_queue, &pfifo_qdisc_ops,
                                  TC_H_MAKE(sch->handle, i + 1), extack);
        if (!qs[i]) {
            while (--i >= old_classes)
                qdisc_put(qs[i]);
            return -ENOMEM;
        }
    }

    sch_tree_lock(sch);

    q->num_classes = new_classes;
    q->rate_limit = rate;
    psched_ratecfg_precompute(&q->rate_fair, &conf, rate / 10);
    psched_ratecfg_precompute(&q->rate_prio, &conf, rate - rate / 10);
    q->prio_tokens = 0;
    q->fair_tokens = 0;
    q->max_burst = PPQ_MAX_BURST_NS;
    q->last_update = ktime_get();
    q->rr_cursor = 0;

    /* Shrinking: detach the dropped children. */
    for (i = new_classes; i < old_classes; i++) {
        struct Qdisc *c = q->queues[i];
        u32 qlen, backlog;

        qs[i] = c;
        q->queues[i] = NULL;
        if (c) {
            qdisc_qstats_qlen_backlog(c, &qlen, &backlog);
            qdisc_tree_reduce_backlog(c, qlen, backlog);
        }
    }

    /* Growing: install the new children. */
    for (i = old_classes; i < new_classes; i++) {
        q->queues[i] = qs[i];
        if (qs[i] != &noop_qdisc)
            qdisc_hash_add(qs[i], true);
    }

    sch_tree_unlock(sch);

    /* Put detached children outside the lock. */
    for (i = new_classes; i < old_classes; i++) {
        if (qs[i])
            qdisc_put(qs[i]);
    }

    return 0;
}

static int ppq_init(struct Qdisc *sch, struct nlattr *opt,
                    struct netlink_ext_ack *extack)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    int err;

    qdisc_watchdog_init(&q->watchdog, sch);

    if (!opt)
        return -EINVAL;

    err = tcf_block_get(&q->block, &q->filter_list, sch, extack);
    if (err)
        return err;

    err = ppq_tune(sch, opt, extack);
    if (!err)
        ppq_dbg_attach(sch);
    return err;
}

static void ppq_reset(struct Qdisc *sch)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    int i;

    qdisc_watchdog_cancel(&q->watchdog);
    for (i = 0; i < q->num_classes; i++) {
        if (q->queues[i])
            qdisc_reset(q->queues[i]);
    }
    /* The vendor binary skips this; sch_prio does it. */
    sch->qstats.backlog = 0;
    sch->q.qlen = 0;
}

static void ppq_destroy(struct Qdisc *sch)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    int i;

    ppq_dbg_detach(sch);
    qdisc_watchdog_cancel(&q->watchdog);
    tcf_block_put(q->block);

    for (i = 0; i < q->num_classes; i++) {
        if (q->queues[i])
            qdisc_put(q->queues[i]);
    }
}

static struct Qdisc *ppq_classify(struct sk_buff *skb, struct Qdisc *sch,
                                  int *qerr)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    u32 prio = skb->priority;
    struct tcf_result res = {};
    struct tcf_proto *fl;
    unsigned int idx;
    int result;

    *qerr = NET_XMIT_SUCCESS | __NET_XMIT_BYPASS;

    if ((prio & TC_H_MAJ_MASK) == sch->handle) {
        PPQ_STAT_INC(sch, cls_direct);
        idx = TC_H_MIN(prio) - 1;
    } else {
        fl = rcu_dereference_bh(q->filter_list);
        result = tcf_classify(skb, NULL, fl, &res, false);
        switch (result) {
        case TC_ACT_STOLEN:
        case TC_ACT_QUEUED:
        case TC_ACT_TRAP:
            *qerr = NET_XMIT_SUCCESS | __NET_XMIT_STOLEN;
            fallthrough;
        case TC_ACT_SHOT:
            return NULL;
        }

        if (fl && result >= 0) {
            PPQ_STAT_INC(sch, cls_filter);
            idx = TC_H_MIN(res.classid) - 1;
        } else {
            PPQ_STAT_INC(sch, cls_default);
            idx = q->num_classes - 1;
        }
    }

    if (idx >= (unsigned int)q->num_classes)
        idx = q->num_classes - 1;
    if (idx >= PPQ_MAX_CLASSES)
        return NULL;

    PPQ_STAT_INC(sch, cls_hit[idx]);
    return q->queues[idx];
}

static int ppq_enqueue(struct sk_buff *skb, struct Qdisc *sch,
                       struct sk_buff **to_free)
{
    unsigned int len = qdisc_pkt_len(skb);
    struct Qdisc *child;
    int ret;

    child = ppq_classify(skb, sch, &ret);
    if (!child) {
        if (ret & __NET_XMIT_BYPASS) {
            qdisc_qstats_drop(sch);
            PPQ_STAT_INC(sch, enq_cls_drop);
        } else {
            PPQ_STAT_INC(sch, enq_stolen);
        }
        __qdisc_drop(skb, to_free);
        return ret;
    }

    ret = qdisc_enqueue(skb, child, to_free);
    if (ret == NET_XMIT_SUCCESS) {
        sch->qstats.backlog += len;
        sch->q.qlen++;
        PPQ_STAT_INC(sch, enq_ok);
        return NET_XMIT_SUCCESS;
    }
    PPQ_STAT_INC(sch, enq_child_drop);
    if (net_xmit_drop_count(ret))
        qdisc_qstats_drop(sch);
    return ret;
}

static void ppq_charge(s64 *tokens, const struct psched_ratecfg *r,
                       const struct sk_buff *skb, s64 max_burst)
{
    *tokens -= psched_l2t_ns(r, qdisc_pkt_len(skb));
    if (*tokens < -max_burst)
        *tokens = 1 - max_burst;
}

static struct sk_buff *ppq_dequeue(struct Qdisc *sch)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    ktime_t now = ktime_get();
    struct sk_buff *skb;
    s64 delta, next;
    int i, idx;

    delta = ktime_to_ns(ktime_sub(now, q->last_update));
    if (delta <= 0 || delta > q->max_burst) {
        if (delta <= 0)
            PPQ_STAT_INC(sch, delta_nonpos);
        else
            PPQ_STAT_INC(sch, delta_clamped);
        delta = q->max_burst;
    }
    q->last_update = now;

    /* Refill; credit is capped at 0 (no burst allowance). */
    q->prio_tokens = min_t(s64, q->prio_tokens + delta, 0);
    q->fair_tokens = min_t(s64, q->fair_tokens + delta, 0);

    /* Class 0 is always tried first and is never gated, only accounted. */
    skb = q->queues[0]->dequeue(q->queues[0]);
    if (skb) {
        ppq_charge(&q->prio_tokens, &q->rate_prio, skb, q->max_burst);
        PPQ_STAT_INC(sch, deq_prio[0]);
        PPQ_STAT_ADD(sch, bytes_prio, qdisc_pkt_len(skb));
    } else if (q->prio_tokens < 0) {
        /* Priority budget spent: fair-share round robin over all classes. */
        if (q->fair_tokens >= 0) {
            for (i = 0; i < q->num_classes; i++) {
                idx = (q->rr_cursor + i) % q->num_classes;
                skb = q->queues[idx]->dequeue(q->queues[idx]);
                if (skb) {
                    q->rr_cursor = (idx + 1) % q->num_classes;
                    ppq_charge(&q->fair_tokens, &q->rate_fair, skb,
                               q->max_burst);
                    PPQ_STAT_INC(sch, deq_fair[idx]);
                    PPQ_STAT_ADD(sch, bytes_fair, qdisc_pkt_len(skb));
                    break;
                }
            }
        }
    } else {
        /* Priority budget available: strict priority over classes 1..n-1. */
        for (i = 1; i < q->num_classes; i++) {
            skb = q->queues[i]->dequeue(q->queues[i]);
            if (skb) {
                ppq_charge(&q->prio_tokens, &q->rate_prio, skb,
                           q->max_burst);
                PPQ_STAT_INC(sch, deq_prio[i]);
                PPQ_STAT_ADD(sch, bytes_prio, qdisc_pkt_len(skb));
                break;
            }
        }
    }

    /* Wake when the least-indebted bucket recovers. */
    PPQ_STAT_MIN(sch, min_prio_tokens, q->prio_tokens);
    PPQ_STAT_MIN(sch, min_fair_tokens, q->fair_tokens);

    next = max(q->prio_tokens, q->fair_tokens);
    if (next < 0) {
        PPQ_STAT_INC(sch, wd_armed);
        PPQ_STAT_MAX(sch, max_wd_delay_ns, -next);
        qdisc_watchdog_schedule_range_ns(&q->watchdog,
                                         ktime_to_ns(now) - next, 0);
    }

    if (skb) {
        qdisc_bstats_update(sch, skb);
        qdisc_qstats_backlog_dec(sch, skb);
        sch->q.qlen--;
    } else if (sch->q.qlen) {
        PPQ_STAT_INC(sch, deq_gated);
    } else {
        PPQ_STAT_INC(sch, deq_empty);
    }
    return skb;
}

static int ppq_dump(struct Qdisc *sch, struct sk_buff *skb)
{
    struct ppq_sched_data *q = qdisc_priv(sch);
    unsigned char *b = skb_tail_pointer(skb);
    struct ppq_tc_opt opt = {
        .classes = q->num_classes,
        .rate = (u32)q->rate_limit,
    };

    if (nla_put(skb, TCA_OPTIONS, sizeof(opt), &opt))
        goto nla_put_failure;
    return skb->len;

nla_put_failure:
    nlmsg_trim(skb, b);
    return -1;
}

static struct Qdisc_ops ppq_qdisc_ops __read_mostly = {
    .cl_ops     = &ppq_class_ops,
    .id         = "ppq",
    .priv_size  = sizeof(struct ppq_sched_data),
    .enqueue    = ppq_enqueue,
    .dequeue    = ppq_dequeue,
    .peek       = qdisc_peek_dequeued,
    .init       = ppq_init,
    .reset      = ppq_reset,
    .destroy    = ppq_destroy,
    .change     = ppq_tune,
    .dump       = ppq_dump,
    .owner      = THIS_MODULE,
};

static int __init ppq_module_init(void)
{
    int err;

    pr_info("ppq_module_init\n");
    ppq_dbg_module_init();
    err = register_qdisc(&ppq_qdisc_ops);
    if (err)
        ppq_dbg_module_exit();
    return err;
}

static void __exit ppq_module_exit(void)
{
    pr_info("ppq_module_exit\n");
    unregister_qdisc(&ppq_qdisc_ops);
    ppq_dbg_module_exit();
}

module_init(ppq_module_init);
module_exit(ppq_module_exit);
MODULE_ALIAS("sch_ppq");
MODULE_LICENSE("GPL");