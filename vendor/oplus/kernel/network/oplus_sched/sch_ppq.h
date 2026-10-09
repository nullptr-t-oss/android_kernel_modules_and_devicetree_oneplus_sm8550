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

#ifdef OPLUS_SCHED_DEBUG
#include <linux/dcache.h>

/* All counters are updated under the root qdisc lock, so plain u64 is fine. */
struct ppq_dbg {
    u64 cls_direct;                     /* skb->priority major == our handle */
    u64 cls_filter;                     /* classified by a tc filter */
    u64 cls_default;                    /* no filter / no match -> last class */
    u64 cls_hit[PPQ_MAX_CLASSES];       /* class chosen (before enqueue) */
    u64 enq_ok;
    u64 enq_child_drop;                 /* child qdisc refused the skb */
    u64 enq_cls_drop;                   /* TC_ACT_SHOT or no child */
    u64 enq_stolen;                     /* STOLEN / QUEUED / TRAP */
    u64 deq_prio[PPQ_MAX_CLASSES];      /* served from class i, charged to prio bucket */
    u64 deq_fair[PPQ_MAX_CLASSES];      /* served from class i by fair-share RR */
    u64 deq_gated;                      /* backlog present but nothing allowed */
    u64 deq_empty;
    u64 bytes_prio;
    u64 bytes_fair;
    u64 wd_armed;
    s64 max_wd_delay_ns;
    u64 delta_nonpos;                   /* delta <= 0 -> forced to max_burst */
    u64 delta_clamped;                  /* delta > max_burst */
    s64 min_prio_tokens;                /* lowest credit seen (floor is 1 - max_burst) */
    s64 min_fair_tokens;
};
#endif

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
#ifdef OPLUS_SCHED_DEBUG
    struct ppq_dbg dbg;
    struct dentry *dbg_dentry;
#endif
};

#ifdef OPLUS_SCHED_DEBUG
#define PPQ_DBG(sch)            (&((struct ppq_sched_data *)qdisc_priv(sch))->dbg)
#define PPQ_STAT_INC(sch, f)    (PPQ_DBG(sch)->f++)
#define PPQ_STAT_ADD(sch, f, n) (PPQ_DBG(sch)->f += (n))
#define PPQ_STAT_MAX(sch, f, v) \
    do { if ((v) > PPQ_DBG(sch)->f) PPQ_DBG(sch)->f = (v); } while (0)
#define PPQ_STAT_MIN(sch, f, v) \
    do { if ((v) < PPQ_DBG(sch)->f) PPQ_DBG(sch)->f = (v); } while (0)

void ppq_dbg_module_init(void);
void ppq_dbg_module_exit(void);
void ppq_dbg_attach(struct Qdisc *sch);
void ppq_dbg_detach(struct Qdisc *sch);
#else
#define PPQ_STAT_INC(sch, f)    do { } while (0)
#define PPQ_STAT_ADD(sch, f, n) do { } while (0)
#define PPQ_STAT_MAX(sch, f, v) do { } while (0)
#define PPQ_STAT_MIN(sch, f, v) do { } while (0)

static inline void ppq_dbg_module_init(void) { }
static inline void ppq_dbg_module_exit(void) { }
static inline void ppq_dbg_attach(struct Qdisc *sch) { }
static inline void ppq_dbg_detach(struct Qdisc *sch) { }
#endif

extern const struct Qdisc_class_ops ppq_class_ops;

#endif /* _SCH_PPQ_H */