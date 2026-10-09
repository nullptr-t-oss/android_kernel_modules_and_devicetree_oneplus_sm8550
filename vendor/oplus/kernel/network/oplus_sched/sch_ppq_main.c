#include "sch_ppq.h"

static int ppq_init(struct Qdisc *sch, struct nlattr *opt, 
                    struct netlink_ext_ack *extack)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    struct ppq_tc_opt opt_00;
    int err;

    qdisc_watchdog_init(&priv->watchdog, sch);

    if (!opt)
        return -EINVAL;

    err = tcf_block_get(&priv->block, &priv->filter_list, sch, extack);
    if (err == 0) {
        opt_00.rate = nla_get_u32(opt);
        err = ppq_tune(sch, &opt_00, extack);
    }
    
    return err;
}

static void ppq_destroy(struct Qdisc *sch)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    int i;

    qdisc_watchdog_cancel(&priv->watchdog);
    tcf_block_put(priv->block);

    for (i = 0; i < priv->num_classes; i++) {
        if (i < 16 && priv->queues[i])
            qdisc_put(priv->queues[i]);
    }
}

static int ppq_tune(struct Qdisc *sch, struct nlattr *opt, struct netlink_ext_ack *extack)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    struct ppq_tc_opt *user_opt;
    struct Qdisc *temp_queues[16] = { NULL };
    int old_classes, new_classes;
    u32 rate;
    int i, err = 0;
    struct psched_ratespec p_rate = { .rate = 0 };

    if (!opt)
        return -EINVAL;

    user_opt = nla_data(opt);
    if (nla_len(opt) < sizeof(*user_opt))
        return -EINVAL;

    new_classes = user_opt->classes;
    if (new_classes <= 0 || new_classes > 16)
        return -EINVAL;

    rate = user_opt->rate;
    old_classes = priv->num_classes;

    if (old_classes < new_classes) {
        for (i = old_classes; i < new_classes; i++) {
            struct Qdisc *q;
            q = qdisc_create_dflt(sch->dev_queue, &pfifo_qdisc_ops,
                                  TC_H_MAKE(sch->handle, i + 1), extack);
            if (!q) {
                err = -ENOMEM;
                goto rollback;
            }
            temp_queues[i] = q;
        }
    }

    if (!(sch->flags & TCQ_F_INGRESS)) {
        struct Qdisc *root = qdisc_root_sleeping(sch);
        ASSERT_RTNL();
        spin_lock_bh(qdisc_lock(root));
    } else {
        spin_lock_bh(&sch->q.lock);
    }

    priv->num_classes = new_classes;
    priv->rate_limit = (u64)rate;

    p_rate.rate = rate;
    psched_ratecfg_precompute(&priv->rate_2, &p_rate, rate / 10);
    psched_ratecfg_precompute(&priv->rate_1, &p_rate, rate - (rate / 10));

    priv->bytes_sent = 0;
    priv->tokens = 0;
    priv->max_burst = 2000000000;
    priv->last_update_time = ktime_get();

    if (new_classes < old_classes) {
        for (i = new_classes; i < old_classes; i++) {
            struct Qdisc *q = priv->queues[i];
            if (q) {
                struct gnet_stats_queue qstats;
                qdisc_qstats_qlen_backlog(q, &qstats.qlen, &qstats.backlog);
                qdisc_tree_reduce_backlog(q, qstats.qlen, qstats.backlog);
            }
        }
    }

    if (old_classes < new_classes) {
        for (i = old_classes; i < new_classes; i++) {
            if (temp_queues[i]) {
                priv->queues[i] = temp_queues[i];
                if (temp_queues[i] != &noop_qdisc)
                    qdisc_hash_add(temp_queues[i], true);
            }
        }
    }

    if (!(sch->flags & TCQ_F_INGRESS)) {
        struct Qdisc *root = qdisc_root_sleeping(sch);
        spin_unlock_bh(qdisc_lock(root));
    } else {
        spin_unlock_bh(&sch->q.lock);
    }

    if (new_classes < old_classes) {
        for (i = new_classes; i < old_classes; i++) {
            qdisc_put(priv->queues[i]);
            priv->queues[i] = NULL;
        }
    }

    return 0;

rollback:
    for (i = 0; i < 16; i++) {
        if (temp_queues[i])
            qdisc_put(temp_queues[i]);
    }
    return err;
}

static int ppq_enqueue(struct sk_buff *skb, struct Qdisc *sch, struct sk_buff **to_free)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    unsigned int cls_idx;
    int err;
    int pkt_len = qdisc_pkt_len(skb);

    if ((skb->priority & 0xffff0000) == sch->handle) {
        cls_idx = (skb->priority & 0xffff) - 1;
        if (cls_idx >= priv->num_classes)
            cls_idx = priv->num_classes - 1;
        
        if (cls_idx >= 16)
            goto drop_pkt;

        struct Qdisc *target_q = priv->queues[cls_idx];
        if (target_q) {
            err = qdisc_enqueue(skb, target_q, to_free);
            if (err == NET_XMIT_SUCCESS) {
                sch->qstats.backlog += pkt_len;
                sch->q.qlen++;
            } else if (!net_xmit_drop_count(err)) {
                sch->qstats.drops++;
            }
            return err;
        }
        goto drop_pkt;
    } else {
        struct tcf_result res;
        int result = tcf_classify(skb, priv->filter_list, &res, false);
        switch (result) {
        case TC_ACT_OK:
        case TC_ACT_RECLASSIFY:
            if (res.classid) {
                cls_idx = (res.classid & 0xffff) - 1;
                if (cls_idx < priv->num_classes) {
                    struct Qdisc *target_q = priv->queues[cls_idx];
                    if (target_q) {
                        err = qdisc_enqueue(skb, target_q, to_free);
                        if (err == NET_XMIT_SUCCESS) {
                            sch->qstats.backlog += pkt_len;
                            sch->q.qlen++;
                        } else if (!net_xmit_drop_count(err)) {
                            sch->qstats.drops++;
                        }
                        return err;
                    }
                }
            }
            cls_idx = priv->num_classes - 1;
            break;
        case TC_ACT_SHOT:
            goto drop_pkt;
        default:
            cls_idx = priv->num_classes - 1;
            break;
        }

        if (cls_idx < priv->num_classes && priv->queues[cls_idx]) {
            err = qdisc_enqueue(skb, priv->queues[cls_idx], to_free);
            if (err == NET_XMIT_SUCCESS) {
                sch->qstats.backlog += pkt_len;
                sch->q.qlen++;
            } else if (!net_xmit_drop_count(err)) {
                sch->qstats.drops++;
            }
            return err;
        }
    }

drop_pkt:
    sch->qstats.drops++;
    __qdisc_drop(skb, to_free);
    return NET_XMIT_DROP;
}

static struct sk_buff *ppq_dequeue(struct Qdisc *sch)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    ktime_t now = ktime_get();
    s64 delta = ktime_to_ns(ktime_sub(now, priv->last_update_time));
    struct sk_buff *skb = NULL;
    int i;

    if (delta <= 0)
        delta = priv->max_burst;
    else if (delta > priv->max_burst)
        delta = priv->max_burst;

    priv->last_update_time = now;
    
    // Token updates based on bandwidth limits
    priv->bytes_sent += delta;
    priv->tokens += delta;

    if (priv->queues[0]) {
        skb = priv->queues[0]->dequeue(priv->queues[0]);
    }

    if (skb) {
        unsigned int len = qdisc_pkt_len(skb) + priv->rate_1.overhead;
        if (len < priv->rate_1.mpu)
            len = priv->rate_1.mpu;
        priv->bytes_sent -= (priv->rate_1.mult * len) >> priv->rate_1.shift;
    } else if ((s64)priv->bytes_sent < 0) {
        for (i = 0; i < priv->num_classes; i++) {
            struct Qdisc *q = priv->queues[i];
            if (q) {
                skb = q->dequeue(q);
                if (skb) {
                    unsigned int len = qdisc_pkt_len(skb) + priv->rate_2.overhead;
                    if (len < priv->rate_2.mpu)
                        len = priv->rate_2.mpu;
                    priv->tokens -= (priv->rate_2.mult * len) >> priv->rate_2.shift;
                    break;
                }
            }
        }
    }

    if (skb) {
        sch->bstats.bytes += qdisc_pkt_len(skb);
        sch->bstats.packets++;
        sch->qstats.backlog -= qdisc_pkt_len(skb);
        sch->q.qlen--;
    } else {
        qdisc_watchdog_schedule_range_ns(&priv->watchdog, ktime_to_ns(now) + 1000000, 0);
    }

    return skb;
}

static int ppq_dump(struct Qdisc *sch, struct sk_buff *skb)
{
    struct ppq_sched_data *priv = qdisc_priv(sch);
    struct ppq_tc_opt opt = {
        .flags = 0,
        .classes = priv->num_classes,
        .rate = (u32)priv->rate_limit
    };

    if (nla_put(skb, TCA_OPTIONS, sizeof(opt), &opt))
        goto nla_put_failure;

    return skb->len;

nla_put_failure:
    return -1;
}

static struct Qdisc_ops ppq_qdisc_ops __read_mostly = {
    .cl_ops         = &ppq_class_ops,
    .id             = "ppq",
    .priv_size      = sizeof(struct ppq_sched_data),
    .enqueue        = ppq_enqueue,
    .dequeue        = ppq_dequeue,
    .peek           = qdisc_peek_dequeued,
    .init           = ppq_init,
    .reset          = ppq_reset,
    .destroy        = ppq_destroy,
    .change         = ppq_tune,
    .dump           = ppq_dump,
    .owner          = THIS_MODULE,
};

static int __init ppq_module_init(void)
{
    pr_info("ppq_module_init");
    return register_qdisc(&ppq_qdisc_ops);
}

static void __exit ppq_module_exit(void)
{
    pr_info("ppq_module_exit");
    unregister_qdisc(&ppq_qdisc_ops);
}

module_init(ppq_module_init);
module_exit(ppq_module_exit);
MODULE_LICENSE("GPL");