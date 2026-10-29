/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

/*
this file is splited from the sa_common.h to adapt the OKI,
IS_ENABLED is not allowed in here, because the macro will not work in OKI.
*/
#ifndef _OPLUS_SA_COMMON_STRUCT_H_
#define _OPLUS_SA_COMMON_STRUCT_H_

#define MAX_CLUSTER            (3)

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_JANKINFO)
#define MAX_TASK_COMM_LEN 256
struct uid_struct {
	uid_t uid;
	u64 uid_total_cycle;
	u64 uid_total_inst;
	spinlock_t lock;
	char leader_comm[TASK_COMM_LEN];
	char cmdline[MAX_TASK_COMM_LEN];
};

struct amu_uid_entry {
	uid_t uid;
	struct uid_struct *uid_struct;
	struct hlist_node node;
};
#endif

#if IS_ENABLED(CONFIG_OPLUS_LOCKING_STRATEGY)

enum locking_info_state {
	UX_CONTRIB_BIT,
	NEED_RESCHED_LAZY_BIT,
	LOCK_BOOST_TICKOUT_BIT,
	/*
	 * Whether task is ux when it's going to be added to mutex or
	 * rwsem waiter list. It helps us check whether there is ux
	 * task on mutex or rwsem waiter list. Also, a task can't be
	 * added to both mutex and rwsem at one time, so use one common
	 * field is OK.
	 */
	IS_BLOCK_UX_BIT,
};

struct locking_info {
	u64 waittime_stamp;
	/* Used in torture acquire latency statistic.*/
	u64 acquire_stamp;
	/*
	 * mutex or rwsem optimistic spin start time. Because a task
	 * can't spin both on mutex and rwsem at one time, use one common
	 * threshold time is OK.
	 */
	u64 opt_spin_start_time;
	struct task_struct *holder;
	unsigned long state_flags;
	u32 waittype;
};
#endif

enum oplus_task_struct_common_flag {
	OTS_STATE_SET_AFFINITY,
	OTS_STATE_DDL_ACTIVE,
	OTS_STATE_DDL_ACTIVE_PREEMPTED,
	IS_VIP_MVP_BIT,
	LD_FLAG_BIT,
	IS_UPDATE_RUNTIME_BIT,
	FBG_RUNNING_BIT,
	CFBT_RUNNING_BIT,
	INELIGIBLE_FLAG_BIT,
	BINDER_ASYNC_UX_STS_BIT,
	UPDATE_RUNNING_START_TIME_BIT,
	SG_FLAG_BIT,
};

/* Please add your own members of task_struct here :) */
struct oplus_task_struct {
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
	/* The write mostly fields in oplus_task_struct, aligned to one cache line (64 bytes)*/
	struct {
		struct rb_node ux_entry;
		struct rb_node exec_time_node;
		u64 total_exec;
		u64 vruntime;
	} ____cacheline_aligned;

	struct task_struct *task;
	atomic64_t inherit_ux;
	s64 cfs_delta;
	u64 enqueue_time;
	u64 inherit_ux_start;
	u64 preset_vruntime;
	unsigned long im_flag;
	unsigned long common_flags;
	/* contains ux state
	1. if static and inherited ux both exist, static ux stores in ux_state, inherited ux in sub_ux_state.
	2. if only static ux exists, static ux stores in ux_state.
	2. if only inherited ux exists, inherited ux stores in ux_state */
	int ux_state;
	int sub_ux_state;
	pid_t affinity_pid;
	pid_t affinity_tgid;
	u8 ux_depth;
	s8 ux_priority;
	s8 ux_nice;
#endif

#if IS_ENABLED(CONFIG_OPLUS_LOCKING_STRATEGY)
	struct locking_info lkinfo;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_DDL)
	u64 ddl;
	u64 ddl_active_ts;
	u64 runnable_ts;
	struct rb_node ddl_node;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_TASK_SCHED)
	u64 wake_tid;
	u64 running_start_time;
	u64 exec_calc_runtime;
#endif

#if IS_ENABLED(CONFIG_LOCKING_PROTECT)
	struct list_head locking_entry;
	unsigned long locking_start_time;
	int locking_depth;
	int lk_tick_hit;
#endif

/* for loadbalance */
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_LOADBALANCE)
	/* rt boost task */
	struct plist_node rtb;
	/*
	* The following variables are used to calculate the time
	* a task spends in the running/runnable state.
	*/
	u64 snap_run_delay;
	unsigned long snap_pcount;
#endif

#if IS_ENABLED(CONFIG_ARM64_AMU_EXTN) && IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_JANKINFO)
	struct uid_struct *uid_struct;
	u64 amu_instruct;
	u64 amu_cycle;
#endif

/* for binder ux */
#if IS_ENABLED(CONFIG_OPLUS_BINDER_STRATEGY)
	struct binder_node *binder_thread_node;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_FRAME_BOOST)
	struct list_head fbg_list;
	u64 last_wake_ts;
	raw_spinlock_t fbg_list_entry_lock;
	int fbg_cur_group;
	u16 fbg_state;
	s8 preferred_cluster_id;
	s8 fbg_depth;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_ABNORMAL_FLAG)
	s16 abnormal_flag;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_QOS_SCHED)
	struct mutex qs_mutex;
	int qos_level;
	int qos_recover_prio;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_JANKINFO)
	u64 block_start_time;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_PIPELINE)
	atomic_t pipeline_cpu;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_SPREAD)
	int lb_state;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_TASK_SCHED)
	int target_process;
#endif

#if IS_ENABLED(CONFIG_OPLUS_BINDER_STRATEGY)
	s8 binder_async_ux_enable;
#endif

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_FDLEAK_CHECK)
	u8 fdleak_flag;
#endif

#if IS_ENABLED(CONFIG_OPLUS_SCHED_TUNE)
	s8 stune_idx;
#endif
} ____cacheline_aligned;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_LOADBALANCE)
#define INVALID_PID						(-1)
struct oplus_lb {
	/* used for active_balance to record the running task. */
	pid_t pid;
};
#endif

#endif /* _OPLUS_SA_COMMON_STRUCT_H_ */

