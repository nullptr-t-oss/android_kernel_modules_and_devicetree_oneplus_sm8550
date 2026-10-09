#ifndef _SCH_PPQ_H
#define _SCH_PPQ_H

#include <linux/module.h>
#include <linux/types.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#include <net/netlink.h>

/* Netlink configuration payload mapped from userspace */
struct ppq_tc_opt {
    u32 flags;
    u32 classes;
    u32 rate;
};

/* The 344-byte private data structure mapped from the binary 
 * Note: this struct was fully re'd
 */
struct ppq_sched_data {
    struct tcf_block *block;
    u32 num_classes;
    u64 rate_limit;
    struct psched_ratecfg rate_1;
    struct psched_ratecfg rate_2;
    u64 bytes_sent;
    u64 tokens;
    u64 max_burst;
    ktime_t last_update_time;
    struct qdisc_watchdog watchdog;
    struct Qdisc *queues[16]; /* 1-indexed classes mapped to 0-15 */
    u32 last_polled_queue;
};

/* External declarations for class operations */
extern const struct Qdisc_class_ops ppq_class_ops;

/* Prototypes for shared functions */
void ppq_reset(struct Qdisc *sch);

#endif /* _SCH_PPQ_H */