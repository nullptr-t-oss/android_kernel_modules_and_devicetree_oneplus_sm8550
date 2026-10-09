#ifndef _SCH_PPQ_H
#define _SCH_PPQ_H

#include <linux/module.h>
#include <linux/types.h>
#include <linux/ktime.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <net/netlink.h>

#define PPQ_MAX_CLASSES   16
#define PPQ_MAX_BURST_NS  2000000000LL

/* TCA_OPTIONS payload. 8 bytes on the wire: {classes, rate}. */
struct ppq_tc_opt {
    u32 classes;
    u32 rate;
};

struct ppq_sched_data {
    struct tcf_block *block;
    struct tcf_proto __rcu *filter_list;
    int num_classes;                    /* 1..16 */
    u64 rate_limit;
    struct psched_ratecfg rate_prio;    /* rate - rate/10 */
    struct psched_ratecfg rate_fair;    /* rate/10 */
    s64 prio_tokens;                    /* ns credit, <= 0 */
    s64 fair_tokens;                    /* ns credit, <= 0 */
    s64 max_burst;
    ktime_t last_update;
    int rr_cursor;                      /* fair-share round robin position */
    struct Qdisc *queues[PPQ_MAX_CLASSES];
    struct qdisc_watchdog watchdog;
};

extern const struct Qdisc_class_ops ppq_class_ops;

#endif /* _SCH_PPQ_H */
