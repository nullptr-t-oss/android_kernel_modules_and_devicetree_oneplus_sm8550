// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2025 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "[HYB_ZRAM]" fmt

#include <uapi/linux/sched/types.h>
#include <linux/sched.h>
#include <linux/memory.h>
#include <linux/freezer.h>
#include <linux/swap.h>
#include <linux/cgroup-defs.h>
#include <linux/seq_file.h>
#include <linux/device.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/file.h>
#include <linux/memcontrol.h>

#include "../zram_drv.h"
#include "../zram_drv_internal.h"
#include "internal.h"
#include "hybridswap.h"

struct swapd_param {
	unsigned int min_score;
	unsigned int max_score;
	unsigned int ub_mem2zram_ratio;
	unsigned int ub_zram2ufs_ratio;
	unsigned int refault_threshold;
};

#define HS_SWAP_ANON_REFAULT_THRESHOLD 22000
#define ANON_REFAULT_SNAPSHOT_MIN_INTERVAL 200
#define EMPTY_ROUND_SKIP_INTERNVAL 20
#define MAX_SKIP_INTERVAL 1000
#define EMPTY_ROUND_CHECK_THRESHOLD 10
#define ZRAM_WM_RATIO 75
#define COMPRESS_RATIO 30
#define SWAPD_MAX_LEVEL_NUM 10
#define SWAPD_DEFAULT_BIND_CPUS "0-3"
#define MAX_RECLAIMIN_SZ (200llu << 20)
#define page_to_kb(nr) (nr << (PAGE_SHIFT - 10))
#define SWAPD_SHRINK_WINDOW (HZ * 10)
#define SWAPD_SHRINK_SIZE_PER_WINDOW 1024
#define PAGES_TO_MB(pages) ((pages) >> 8)
#define PAGES_PER_1MB (1 << 8)

typedef bool (*free_swap_is_low_func)(void);
static free_swap_is_low_func free_swap_is_low_fp;

static atomic64_t zram_wm_ratio = ATOMIC_LONG_INIT(ZRAM_WM_RATIO);
static atomic64_t compress_ratio = ATOMIC_LONG_INIT(COMPRESS_RATIO);
static atomic_t avail_buffers = ATOMIC_INIT(0);
static atomic_t min_avail_buffers = ATOMIC_INIT(0);
static atomic_t high_avail_buffers = ATOMIC_INIT(0);
static atomic_t max_reclaim_size = ATOMIC_INIT(100);
static atomic64_t free_swap_threshold = ATOMIC64_INIT(0);
static atomic64_t zram_crit_thres = ATOMIC_LONG_INIT(0);
static atomic64_t cpuload_threshold = ATOMIC_LONG_INIT(0);
static atomic64_t hs_swap_anon_refault_threshold = ATOMIC_LONG_INIT(HS_SWAP_ANON_REFAULT_THRESHOLD);
static atomic64_t anon_refault_snapshot_min_interval = ATOMIC_LONG_INIT(ANON_REFAULT_SNAPSHOT_MIN_INTERVAL);
static atomic64_t empty_round_skip_interval = ATOMIC_LONG_INIT(EMPTY_ROUND_SKIP_INTERNVAL);
static atomic64_t max_skip_interval = ATOMIC_LONG_INIT(MAX_SKIP_INTERVAL);
static atomic64_t empty_round_check_threshold = ATOMIC_LONG_INIT(EMPTY_ROUND_CHECK_THRESHOLD);
static unsigned long all_totalreserve_pages;
static u64 zram_used_limit_pages;

static struct swapd_param zswap_param[SWAPD_MAX_LEVEL_NUM];
static pid_t swapd_pid = -1;
static atomic_long_t fault_out_pause = ATOMIC_LONG_INIT(0);
static atomic_long_t fault_out_pause_cnt = ATOMIC_LONG_INIT(0);

static atomic_t swapd_pause = ATOMIC_INIT(0);
static atomic_t swapd_enabled = ATOMIC_INIT(0);
static unsigned long swapd_nap_jiffies = 1;
static atomic_t *p_ezreclaimable_nr;

static void wake_all_swapd(void);

#ifdef CONFIG_OPLUS_JANK
extern u32 get_cpu_load(u32 win_cnt, struct cpumask *mask);
#endif

static inline u64 get_compress_ratio_value(void)
{
	return atomic64_read(&compress_ratio);
}

static inline unsigned int get_avail_buffers_value(void)
{
	return atomic_read(&avail_buffers);
}

static inline unsigned int get_min_avail_buffers_value(void)
{
	return atomic_read(&min_avail_buffers);
}

static inline unsigned int get_high_avail_buffers_value(void)
{
	return atomic_read(&high_avail_buffers);
}

static inline u64 get_swapd_max_reclaim_size(void)
{
	return atomic_read(&max_reclaim_size);
}

static inline u64 get_free_swap_threshold_value(void)
{
	return atomic64_read(&free_swap_threshold);
}

static inline unsigned long long get_hs_swap_anon_refault_threshold_value(void)
{
	return atomic64_read(&hs_swap_anon_refault_threshold);
}

static inline unsigned long get_anon_refault_snapshot_min_interval_value(void)
{
	return atomic64_read(&anon_refault_snapshot_min_interval);
}

static inline unsigned long long get_empty_round_skip_interval_value(void)
{
	return atomic64_read(&empty_round_skip_interval);
}

static inline unsigned long long get_max_skip_interval_value(void)
{
	return atomic64_read(&max_skip_interval);
}

static inline unsigned long long get_empty_round_check_threshold_value(void)
{
	return atomic64_read(&empty_round_check_threshold);
}

static inline u64 get_zram_critical_threshold_value(void)
{
	return atomic64_read(&zram_crit_thres);
}

static inline u64 get_cpuload_threshold_value(void)
{
	return atomic64_read(&cpuload_threshold);
}

static ssize_t avail_buffers_params_write(struct kernfs_open_file *of,
		char *buf, size_t nbytes, loff_t off)
{
	unsigned int avail_buffers_value;
	unsigned int min_avail_buffers_value;
	unsigned int high_avail_buffers_value;
	u64 free_swap_threshold_value;

	buf = strstrip(buf);

	if (sscanf(buf, "%u %u %u %llu",
				&avail_buffers_value,
				&min_avail_buffers_value,
				&high_avail_buffers_value,
				&free_swap_threshold_value) != 4)
		return -EINVAL;

	atomic_set(&avail_buffers, avail_buffers_value);
	atomic_set(&min_avail_buffers, min_avail_buffers_value);
	atomic_set(&high_avail_buffers, high_avail_buffers_value);
	atomic64_set(&free_swap_threshold,
			(free_swap_threshold_value * (SZ_1M / PAGE_SIZE)));

	return nbytes;
}

static int avail_buffers_params_show(struct seq_file *m, void *v)
{
	seq_printf(m, "avail_buffers: %u\n",
			atomic_read(&avail_buffers));
	seq_printf(m, "min_avail_buffers: %u\n",
			atomic_read(&min_avail_buffers));
	seq_printf(m, "high_avail_buffers: %u\n",
			atomic_read(&high_avail_buffers));
	seq_printf(m, "free_swap_threshold: %llu\n",
			(atomic64_read(&free_swap_threshold) * PAGE_SIZE / SZ_1M));

	return 0;
}

static ssize_t swapd_max_reclaim_size_write(struct kernfs_open_file *of,
		char *buf, size_t nbytes, loff_t off)
{
	const unsigned int base = 10;
	u32 max_reclaim_size_value;
	int ret;

	buf = strstrip(buf);
	ret = kstrtouint(buf, base, &max_reclaim_size_value);
	if (ret)
		return -EINVAL;

	atomic_set(&max_reclaim_size, max_reclaim_size_value);

	return nbytes;
}

static int swapd_max_reclaim_size_show(struct seq_file *m, void *v)
{
	seq_printf(m, "swapd_max_reclaim_size: %u\n",
			atomic_read(&max_reclaim_size));

	return 0;
}

static int hs_swap_anon_refault_threshold_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&hs_swap_anon_refault_threshold, val);

	return 0;
}

static s64 hs_swap_anon_refault_threshold_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&hs_swap_anon_refault_threshold);
}

static int empty_round_skip_interval_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&empty_round_skip_interval, val);

	return 0;
}

static s64 empty_round_skip_interval_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&empty_round_skip_interval);
}

static int max_skip_interval_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&max_skip_interval, val);

	return 0;
}

static s64 max_skip_interval_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&max_skip_interval);
}

static int empty_round_check_threshold_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&empty_round_check_threshold, val);

	return 0;
}

static s64 empty_round_check_threshold_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&empty_round_check_threshold);
}


static int anon_refault_snapshot_min_interval_write(
		struct cgroup_subsys_state *css, struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&anon_refault_snapshot_min_interval, val);

	return 0;
}

static s64 anon_refault_snapshot_min_interval_read(
		struct cgroup_subsys_state *css, struct cftype *cft)
{
	return atomic64_read(&anon_refault_snapshot_min_interval);
}

static int zram_critical_thres_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&zram_crit_thres, val << (20 - PAGE_SHIFT));

	return 0;
}

static s64 zram_critical_thres_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&zram_crit_thres) >> (20 - PAGE_SHIFT);
}

static s64 cpuload_threshold_read(struct cgroup_subsys_state *css,
		struct cftype *cft)

{
	return atomic64_read(&cpuload_threshold);
}

static int cpuload_threshold_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&cpuload_threshold, val);

	return 0;
}

static s64 swapd_pid_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return swapd_pid;
}

static void swapd_memcgs_param_parse(int level_num)
{
	struct mem_cgroup *memcg = NULL;
	memcg_hybs_t *hybs = NULL;
	int i;

	while ((memcg = get_next_memcg(memcg))) {
		hybs = MEMCGRP_ITEM_DATA(memcg);

		for (i = 0; i < level_num; ++i) {
			if (atomic64_read(&hybs->app_score) >= zswap_param[i].min_score &&
					atomic64_read(&hybs->app_score) <= zswap_param[i].max_score)
				break;
		}
		atomic_set(&hybs->ub_mem2zram_ratio, zswap_param[i].ub_mem2zram_ratio);
		atomic_set(&hybs->ub_zram2ufs_ratio, zswap_param[i].ub_zram2ufs_ratio);
		atomic_set(&hybs->refault_threshold, zswap_param[i].refault_threshold);
	}
}

static int update_swapd_memcgs_param(char *buf)
{
	static const char delim[] = " ";
	char *token = NULL;
	int level_num;
	int i;

	buf = strstrip(buf);
	token = strsep(&buf, delim);

	if (!token)
		return -EINVAL;

	if (kstrtoint(token, 0, &level_num))
		return -EINVAL;

	if (level_num > SWAPD_MAX_LEVEL_NUM || level_num < 0)
		return -EINVAL;

	log_warn("%s\n", buf);

	mutex_lock(&reclaim_para_lock);
	for (i = 0; i < level_num; ++i) {
		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].min_score) ||
				zswap_param[i].min_score > MAX_APP_SCORE)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].max_score) ||
				zswap_param[i].max_score > MAX_APP_SCORE)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].ub_mem2zram_ratio) ||
				zswap_param[i].ub_mem2zram_ratio > MAX_RATIO)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].ub_zram2ufs_ratio) ||
				zswap_param[i].ub_zram2ufs_ratio > MAX_RATIO)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].refault_threshold))
			goto out;
	}

	swapd_memcgs_param_parse(level_num);
	mutex_unlock(&reclaim_para_lock);
	return 0;

out:
	mutex_unlock(&reclaim_para_lock);
	return -EINVAL;
}

static ssize_t swapd_memcgs_param_write(struct kernfs_open_file *of, char *buf,
		size_t nbytes, loff_t off)
{
	int ret = update_swapd_memcgs_param(buf);

	if (ret)
		return ret;

	return nbytes;
}

static int swapd_memcgs_param_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < SWAPD_MAX_LEVEL_NUM; ++i) {
		seq_printf(m, "level %d min score: %u\n",
				i, zswap_param[i].min_score);
		seq_printf(m, "level %d max score: %u\n",
				i, zswap_param[i].max_score);
		seq_printf(m, "level %d ub_mem2zram_ratio: %u\n",
				i, zswap_param[i].ub_mem2zram_ratio);
		seq_printf(m, "level %d ub_zram2ufs_ratio: %u\n",
				i, zswap_param[i].ub_zram2ufs_ratio);
		seq_printf(m, "memcg %d refault_threshold: %u\n",
				i, zswap_param[i].refault_threshold);
	}

	return 0;
}

static ssize_t swapd_nap_jiffies_write(struct kernfs_open_file *of, char *buf,
		size_t nbytes, loff_t off)
{
	unsigned long nap;

	buf = strstrip(buf);
	if (!buf)
		return -EINVAL;

	if (kstrtoul(buf, 0, &nap))
		return -EINVAL;

	swapd_nap_jiffies = nap;
	return nbytes;
}

static int swapd_nap_jiffies_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%lu\n", swapd_nap_jiffies);

	return 0;
}

static ssize_t swapd_single_memcg_param_write(struct kernfs_open_file *of,
		char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned int ub_mem2zram_ratio;
	unsigned int ub_zram2ufs_ratio;
	unsigned int refault_threshold;
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	if (!hybs)
		return -EINVAL;

	buf = strstrip(buf);

	if (sscanf(buf, "%u %u %u", &ub_mem2zram_ratio, &ub_zram2ufs_ratio,
				&refault_threshold) != 3)
		return -EINVAL;

	if (ub_mem2zram_ratio > MAX_RATIO || ub_zram2ufs_ratio > MAX_RATIO)
		return -EINVAL;

	log_warn("%u %u %u\n",
	     ub_mem2zram_ratio, ub_zram2ufs_ratio, refault_threshold);

	atomic_set(&MEMCGRP_ITEM(memcg, ub_mem2zram_ratio), ub_mem2zram_ratio);
	atomic_set(&MEMCGRP_ITEM(memcg, ub_zram2ufs_ratio), ub_zram2ufs_ratio);
	atomic_set(&MEMCGRP_ITEM(memcg, refault_threshold), refault_threshold);

	return nbytes;
}


static int swapd_single_memcg_param_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	if (!hybs)
		return -EINVAL;

	seq_printf(m, "memcg score: %lld\n",
			atomic64_read(&hybs->app_score));
	seq_printf(m, "memcg ub_mem2zram_ratio: %u\n",
			atomic_read(&hybs->ub_mem2zram_ratio));
	seq_printf(m, "memcg ub_zram2ufs_ratio: %u\n",
			atomic_read(&hybs->ub_zram2ufs_ratio));
	seq_printf(m, "memcg refault_threshold: %u\n",
			atomic_read(&hybs->refault_threshold));

	return 0;
}

static int mem_cgroup_zram_wm_ratio_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val > MAX_RATIO || val < MIN_RATIO)
		return -EINVAL;

	atomic64_set(&zram_wm_ratio, val);

	return 0;
}

static s64 mem_cgroup_zram_wm_ratio_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&zram_wm_ratio);
}

static int mem_cgroup_compress_ratio_write(struct cgroup_subsys_state *css,
		struct cftype *cft, s64 val)
{
	if (val > MAX_RATIO || val < MIN_RATIO)
		return -EINVAL;

	atomic64_set(&compress_ratio, val);

	return 0;
}

static s64 mem_cgroup_compress_ratio_read(struct cgroup_subsys_state *css,
		struct cftype *cft)
{
	return atomic64_read(&compress_ratio);
}

static int memcg_active_app_info_list_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long anon_size;
	unsigned long zram_size;
	unsigned long eswap_size;

	while ((memcg = get_next_memcg(memcg))) {
		u64 score;

		if (!MEMCGRP_ITEM_DATA(memcg))
			continue;

		score = atomic64_read(&MEMCGRP_ITEM(memcg, app_score));
		anon_size = memcg_anon_pages(memcg);
		eswap_size = hybridswap_read_memcg_stats(memcg,
				MCG_DISK_STORED_PG_SZ);
		zram_size = hybridswap_read_memcg_stats(memcg,
				MCG_ZRAM_STORED_PG_SZ);

		if (anon_size + zram_size + eswap_size == 0)
			continue;

		if (!strlen(MEMCGRP_ITEM(memcg, name)))
			continue;

		anon_size *= PAGE_SIZE / SZ_1K;
		zram_size *= PAGE_SIZE / SZ_1K;
		eswap_size *= PAGE_SIZE / SZ_1K;

		seq_printf(m, "%s %llu %lu %lu %lu %llu\n",
				MEMCGRP_ITEM(memcg, name), score,
				anon_size, zram_size, eswap_size,
				MEMCGRP_ITEM(memcg, reclaimed_pagefault));
	}
	return 0;
}

static unsigned long get_totalreserve_pages(void)
{
	int nid;
	unsigned long val = 0;

	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);

		if (pgdat)
			val += pgdat->totalreserve_pages;
	}

	return val;
}

static unsigned int system_cur_avail_buffers(void)
{
	return si_mem_available() >> 8;
}

static int zram_used_limit_mb_write(struct cgroup_subsys_state *css,
				    struct cftype *cft, s64 val)
{
	zram_used_limit_pages = (val << 20) >> PAGE_SHIFT;
	return 0;
}

static s64 zram_used_limit_mb_read(struct cgroup_subsys_state *css,
				   struct cftype *cft)
{
	return (zram_used_limit_pages << PAGE_SHIFT) >> 20;
}

static struct cftype mem_cgroup_swapd_legacy_files[] = {
	{
		.name = "active_app_info_list",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = memcg_active_app_info_list_show,
	},
	{
		.name = "zram_wm_ratio",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = mem_cgroup_zram_wm_ratio_write,
		.read_s64 = mem_cgroup_zram_wm_ratio_read,
	},
	{
		.name = "compress_ratio",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = mem_cgroup_compress_ratio_write,
		.read_s64 = mem_cgroup_compress_ratio_read,
	},
	{
		.name = "swapd_pid",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.read_s64 = swapd_pid_read,
	},
	{
		.name = "avail_buffers",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = avail_buffers_params_write,
		.seq_show = avail_buffers_params_show,
	},
	{
		.name = "swapd_max_reclaim_size",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_max_reclaim_size_write,
		.seq_show = swapd_max_reclaim_size_show,
	},
	{
		.name = "area_anon_refault_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = hs_swap_anon_refault_threshold_write,
		.read_s64 = hs_swap_anon_refault_threshold_read,
	},
	{
		.name = "empty_round_skip_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = empty_round_skip_interval_write,
		.read_s64 = empty_round_skip_interval_read,
	},
	{
		.name = "max_skip_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = max_skip_interval_write,
		.read_s64 = max_skip_interval_read,
	},
	{
		.name = "empty_round_check_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = empty_round_check_threshold_write,
		.read_s64 = empty_round_check_threshold_read,
	},
	{
		.name = "anon_refault_snapshot_min_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = anon_refault_snapshot_min_interval_write,
		.read_s64 = anon_refault_snapshot_min_interval_read,
	},
	{
		.name = "swapd_memcgs_param",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_memcgs_param_write,
		.seq_show = swapd_memcgs_param_show,
	},
	{
		.name = "swapd_single_memcg_param",
		.write = swapd_single_memcg_param_write,
		.seq_show = swapd_single_memcg_param_show,
	},
	{
		.name = "zram_critical_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = zram_critical_thres_write,
		.read_s64 = zram_critical_thres_read,
	},
	{
		.name = "cpuload_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = cpuload_threshold_write,
		.read_s64 = cpuload_threshold_read,
	},
	{
		.name = "swapd_nap_jiffies",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_nap_jiffies_write,
		.seq_show = swapd_nap_jiffies_show,
	},
	{
		.name = "zram_used_limit_mb",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = zram_used_limit_mb_write,
		.read_s64 = zram_used_limit_mb_read,
	},
	{ }, /* terminate */
};

#define INC_EXTRA_ZRAM_RATIO (2)
static unsigned long get_nr_zram_increase(void)
{
	if (unlikely(!swapd_zram))
		return 0;

	return swapd_zram->increase_nr_pages;
}

static unsigned long zram_used_pages(void)
{
	if (unlikely(!swapd_zram))
		return 0;

	return (u64)atomic64_read(&swapd_zram->stats.pages_stored);
}

/*
 * add extra zram_increase / INC_EXTRA_ZRAM_RATIO to zram, same
 * pages do not occupy physical memory
 */
static unsigned long get_nr_zram_total(void)
{
	unsigned long nr_zram = 1;

	if (!swapd_zram)
		return nr_zram;

	if (zram_used_limit_pages)
		return zram_used_limit_pages;

	nr_zram = swapd_zram->disksize >> PAGE_SHIFT;
#if (defined CONFIG_ZRAM_WRITEBACK) || (defined CONFIG_HYBRIDSWAP_CORE)
	nr_zram -= (get_nr_zram_increase() / INC_EXTRA_ZRAM_RATIO);
#endif
	return nr_zram ?: 1;
}

static bool zram_watermark_ok(void)
{
	long long diff_buffers;
	long long wm = 0;
	long long cur_ratio = 0;
	unsigned long zram_used = zram_used_pages();
	const unsigned int percent_constant = 100;

	diff_buffers = get_high_avail_buffers_value() -
		system_cur_avail_buffers();
	diff_buffers *= SZ_1M / PAGE_SIZE;
	diff_buffers *= get_compress_ratio_value() / 10;
	diff_buffers = diff_buffers * percent_constant / get_nr_zram_total();

	cur_ratio = zram_used * percent_constant / get_nr_zram_total();
	wm  = min(get_zram_wm_ratio_value(), get_zram_wm_ratio_value() - diff_buffers);

	return cur_ratio > wm;
}

static inline bool zram_is_full(void)
{
	return zram_used_pages() >= get_nr_zram_total();
}

static void wake_all_swapd(void) { }

static bool free_swap_is_low(void)
{
	struct sysinfo info;

	si_swapinfo(&info);

	return (info.freeswap < get_free_swap_threshold_value());
}

static bool hybridswap_swapd_enabled(void)
{
	return !!atomic_read(&swapd_enabled);
}

static int swapd_pre_init(void)
{
	free_swap_is_low_fp = free_swap_is_low;
	all_totalreserve_pages = get_totalreserve_pages();
	atomic_set(&swapd_enabled, 1);
	return 0;
}

static void swapd_pre_deinit(void) {}

static void vh_alloc_pages_slowpath(void *data, gfp_t gfp_flags,
				    unsigned int order, unsigned long delta)
{
}

static void vh_tune_scan_type(void *data, char *scan_balance)
{
}

static void vh_shrink_slab_bypass(void *data, gfp_t gfp_mask, int nid,
			   struct mem_cgroup *memcg, int priority,
			   bool *bypass)
{
}

static int swapd_init(struct zram **zram)
{
	/*
	 * register_panel_event_notifier should be called after boot completed
	 */
	register_panel_event_notifier();
	return 0;
}

static void swapd_exit(void)
{
	log_info("unsupport for ezr_empty\n");
}

bool ezr_free_zram_is_ok(void)
{
	return free_zram_is_ok();
}
EXPORT_SYMBOL_GPL(ezr_free_zram_is_ok);

bool ezr_display_off(void)
{
	return atomic_read(&display_off) == 1;
}
EXPORT_SYMBOL_GPL(ezr_display_off);

unsigned int ezr_min_avail_buffers_value(void)
{
	return get_min_avail_buffers_value();
}
EXPORT_SYMBOL_GPL(ezr_min_avail_buffers_value);

unsigned int ezr_high_avail_buffers_value(void)
{
	return get_high_avail_buffers_value();
}
EXPORT_SYMBOL_GPL(ezr_high_avail_buffers_value);

void ezr_set_swapd_pid(pid_t pid)
{
	swapd_pid = pid;
}
EXPORT_SYMBOL_GPL(ezr_set_swapd_pid);

bool ezr_swapd_pasue(void)
{
	return atomic_read(&swapd_pause);
}
EXPORT_SYMBOL_GPL(ezr_swapd_pasue);

int ezr_nr_pages(void)
{
	if (!p_ezreclaimable_nr)
		return 0;
	return atomic_read(p_ezreclaimable_nr);
}

void ezr_register_nr_pages(atomic_t *nr)
{
	p_ezreclaimable_nr = nr;
}
EXPORT_SYMBOL_GPL(ezr_register_nr_pages);

void ezr_empty_ops_init(struct hybridswapd_operations *ops)
{
	/* add a replica of hybridswapd for compat */
	ops->fault_out_pause = &fault_out_pause;
	ops->fault_out_pause_cnt = &fault_out_pause_cnt;
	ops->swapd_pause = &swapd_pause;

	ops->memcg_legacy_files = mem_cgroup_swapd_legacy_files;
	ops->update_memcg_param = update_swapd_memcg_param;

	ops->pre_init = swapd_pre_init;
	ops->pre_deinit = swapd_pre_deinit;

	ops->init = swapd_init;
	ops->deinit = swapd_exit;
	ops->enabled = hybridswap_swapd_enabled;

	ops->free_zram_is_ok = free_zram_is_ok;
	ops->zram_watermark_ok = zram_watermark_ok;
	ops->zram_total_pages = get_nr_zram_total;
	ops->wakeup_kthreads = wake_all_swapd;

	ops->vh_alloc_pages_slowpath = vh_alloc_pages_slowpath;
	ops->vh_tune_scan_type = vh_tune_scan_type;
	ops->vh_shrink_slab_bypass = vh_shrink_slab_bypass;
	log_info("+\n");
}
