// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2025 Oplus. All rights reserved.
 */
#include <linux/memory.h>
#include <linux/freezer.h>
#include <linux/swap.h>
#include <linux/device.h>
#include <linux/memcontrol.h>

#include "ezreclaimd.h"

/******************************************************************************
 *                          declare
 ******************************************************************************/
#define DEFAULT_MIN_BATCH_MB (8)
#define LOOP_BATCH_MB (64)
#define DEFAULT_THRASHING_LIMIT_PCT (80)

#if PAGE_SHIFT < 20
#define M2P(mb)	((mb) << (20 - PAGE_SHIFT))
#else
#define M2P(mb)	((mb) >> (PAGE_SHIFT - 20))
#endif

enum ezr_stat_item {
	EZR_SLEEP_DISPLAY_OFF,
	EZR_SLEEP_CAMERA,
	/* available */
	EZR_STOP_SWAPD_PAUSE,
	EZR_STOP_ANON_THRASHING,
	EZR_STOP_LOW_ANON,
	EZR_STOP_LOW_SWAP,
	EZR_STOP_AVAILABLE_OK,
	EZR_STOP_MIN_BUFFER_OK,
	/* demote */
	EZR_STOP_FILE_THRASHING,
	EZR_STOP_LOW_FILE,
	EZR_STOP_DEMOTE_OK,
	/* reclaim */
	EZR_RC_DIRECT,
	EZR_RC_KSWAPD,
	EZR_RC_SLOW,
	EZR_RC_FAILED0,
	EZR_RC_FAILED_ORDER,
	/* misc */
	EZR_SHRINK_IGNORE_FOLIOS,
	NR_MAX_EZR_STAT,
	/* do not use */
	EZR_STOP_UNKNOWN,
};

static const char *ezr_stat_item_txt[NR_MAX_EZR_STAT] = {
	"sl_display_off",
	"sl_camera",
	/* available */
	"st_pasue",
	"sl_anon_thrashing",
	"st_low_anon",
	"st_low_swap",
	"st_avail_ok",
	"st_min_ok",
	/* demote */
	"sl_file_thrashing",
	"st_low_file",
	"st_demote_ok",
	/* reclaim */
	"rc_direct",
	"rc_kswapd",
	"rc_slow",
	"rcf_order0",
	"rcf_order",
	/* misc */
	"shrink_ignore",
};

enum wakeup_event_item {
	WAKEUP_AVAILABLE = 1,
	WAKEUP_DEMOTE
};

struct ezr_struct {
	unsigned int min_batch;

	/* thrashing control */
	unsigned long last_demote_cnt;
	int wake_flags;

	unsigned long window_sz_hz;
	unsigned int thrashing_limit_pct;
	unsigned long jiffies_file_thrashing;
	unsigned long jiffies_anon_thrashing;
	unsigned int thrashing_file_pct, thrashing_anon_pct;
	unsigned long last_file_lru, last_anon_lru;
	unsigned long last_ws_refault_file, last_ws_refault_anon;

	struct timer_list refault_timer;
	struct wait_queue_head ezreclaimd_wait;
	struct task_struct *ezreclaimd_task;

	atomic_t stat_items[NR_MAX_EZR_STAT];
};

enum {
	EZR_WM_DIRECT_RECLAIM,
	EZR_WM_MIN, /* reserved for camera */
	NR_EZR_WMARKS
};

enum {
	EZR_RECLAIM_KSWAPD,
	EZR_RECLAIM_DIRECT_RECLAIM,
	EZR_RECLAIM_ALL,
};

/*
 * Precision of these counts is not a concern, so atomic operations
 * are not used
 */
static unsigned long ezreclaimable_promote_cnt;
static unsigned long ezreclaimable_demote_cnt;
static unsigned long ezreclaimable_reclaim_cnt;

static unsigned long ezr_wmarks[NR_EZR_WMARKS];
static unsigned int ezr_reclaim_timeout = HZ / 2;
static int ezr_min_avail_buffer, ezr_high_avail_buffer;

atomic_t ezreclaimable_nr = ATOMIC_INIT(0);

static void wakeup_ezreclaimd(void);

/******************************************************************************
 *                          kprobe
 ******************************************************************************/
typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
typedef struct mem_cgroup *(*mem_cgroup_iter_t)(struct mem_cgroup *root,
						struct mem_cgroup *prev,
						struct mem_cgroup_reclaim_cookie *reclaim);
typedef bool (*isolate_page_t)(struct lruvec *lruvec, struct page *page,
			       struct scan_control *sc);
typedef void (*mem_cgroup_iter_break_t)(struct mem_cgroup *root, struct mem_cgroup *prev);

static kallsyms_lookup_name_t p_kallsyms_lookup_name;
static mem_cgroup_iter_break_t kp_mem_cgroup_iter_break;
static mem_cgroup_iter_t kp_mem_cgroup_iter;

static int init_kallsyms_lookup_name(void)
{
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	int ret;

	ret = register_kprobe(&kp);
	if (ret) {
		ezr_loge("failed to read kallsyms_lookup_name\n");
		return ret;
	}
	p_kallsyms_lookup_name = (void *)kp.addr;
	unregister_kprobe(&kp);
	return 0;
}

#define EZR_SYMBOL_LOOKUP(symbol_name) \
	((void *)p_kallsyms_lookup_name(#symbol_name))

int __nocfi ezr_read_symbols_address(void)
{
	int ret;

	ret = init_kallsyms_lookup_name();
	if (ret)
		return ret;

	ezr_logi("start read symbols\n");
	/* memcg */
	kp_mem_cgroup_iter =
		EZR_SYMBOL_LOOKUP(mem_cgroup_iter);
	if (!kp_mem_cgroup_iter)
		return -EINVAL;
	kp_mem_cgroup_iter_break =
		EZR_SYMBOL_LOOKUP(mem_cgroup_iter_break);
	if (!kp_mem_cgroup_iter_break)
		return -EINVAL;
	ezr_logi("read all symbols done\n");
	return 0;
}

/******************************************************************************
 *                          struct initialize
 ******************************************************************************/
static struct ezr_struct g_ezr = {
	.thrashing_limit_pct	= DEFAULT_THRASHING_LIMIT_PCT,
	.min_batch		= (DEFAULT_MIN_BATCH_MB * SZ_1M) / PAGE_SIZE,
	.ezreclaimd_wait	= __WAIT_QUEUE_HEAD_INITIALIZER(g_ezr.ezreclaimd_wait),
};

/******************************************************************************
 *                          stats counter
 ******************************************************************************/
static void inc_ezr_event(enum ezr_stat_item i)
{
	atomic_add(1, g_ezr.stat_items + i);
}

/******************************************************************************
 *                          ezreclaim page flag
 ******************************************************************************/
static inline bool page_test_ezreclaimable(struct page *page)
{
	return test_bit(PG_ezreclaimable, &page->flags);
}

static __always_inline void page_mark_ezreclaimable(struct page *page)
{
	set_bit(PG_ezreclaimable, &page->flags);
}

static __always_inline void page_clear_ezreclaimable(struct page *page)
{
	clear_bit(PG_ezreclaimable, &page->flags);
}

/******************************************************************************
 *                          demote
 *      mark_demote_current
 *		try_to_free_pages (filter folio)
 *			evict_folios
 *			__remove_mapping (keep_reclaimed_folio)
 *				list_for_each_entry_safe_reverse
 *					move_folios_to_lru (lru_gen_add_folio)
 *
 ******************************************************************************/
static inline bool current_is_demote_files(void)
{
	struct ezr_struct *ezrs = &g_ezr;

	return current == ezrs->ezreclaimd_task;
}

static void ezr_vh_shrink_page_list(void *data, struct page *page, bool dirty,
				     bool writeback, bool *activate, bool *keep)
{
	if (!current_is_demote_files())
		return;

	if (writeback || dirty || compound_nr(page) != 1)
		goto keep_page;

	if (page_zonenum(page) != ZONE_NORMAL ||
		get_pageblock_migratetype(page) == MIGRATE_CMA)
		goto keep_page;

	return;
keep_page:
	/* only one task can do this. safe now. */
	*keep = true;
	inc_ezr_event(EZR_SHRINK_IGNORE_FOLIOS);
}

static void ezr_vh_keep_reclaimed_page(void *data, struct page *page, int refcount, bool *keep)
{
	if (current_is_demote_files() && !PageAnon(page) &&
	    !PageSwapBacked(page) && !page_test_ezreclaimable(page)) {
		page_mark_ezreclaimable(page);
		page_ref_unfreeze(page, refcount);
		*keep = true;
	}
}

/* list_for_each_entry_safe_reverse */
static void ezr_vh_evict_pages_bypass(void *data, struct page *page, bool *bypass)
{
	if (page_test_ezreclaimable(page))
		*bypass = true;
}

static inline bool ezreclaimable_add_page(struct lruvec *lruvec, struct page *page)
{
	/* we abuse active/inactive lists as ezreclaimable list */
	struct list_head *head = &lruvec->lists[0];

	if (!page_test_ezreclaimable(page))
		return false;
	if (PageUnevictable(page) || PageActive(page)) {
		page_clear_ezreclaimable(page);
		return false;
	}

	ezreclaimable_demote_cnt++;
	list_add(&page->lru, head);
	atomic_add(compound_nr(page), &ezreclaimable_nr);
	return true;
}

static void ezr_vh_lru_gen_add_page_skip(void *data, struct lruvec *lruvec,
					  struct page *page, bool *skip)
{
	if (ezreclaimable_add_page(lruvec, page))
		*skip = true;
}

/******************************************************************************
 *                          promote
 *      __filemap_get_folio
 *      filemap_map_pages (fault_around)
 ******************************************************************************/

/* not export by kernel, just copy from memcontrol.c without lruvec_memcg_debug */
struct lruvec *lock_page_lruvec_irqsave_dup(struct page *page, unsigned long *flags)
{
	struct lruvec *lruvec;

	lruvec = mem_cgroup_page_lruvec(page);
	spin_lock_irqsave(&lruvec->lru_lock, *flags);

	return lruvec;
}

static inline void unlock_page_lruvec_irqrestore_dup(struct lruvec *lruvec,
		unsigned long flags)
{
	spin_unlock_irqrestore(&lruvec->lru_lock, flags);
}

static inline bool page_ezreclaimable_promote(struct page *page)
{
	struct lruvec *locked_lruvec;
	unsigned long flags;

	if (!page_test_ezreclaimable(page))
		return false;

	locked_lruvec = lock_page_lruvec_irqsave_dup(page, &flags);
	if (PageLRU(page)) {
		del_page_from_lru_list(page, locked_lruvec);
		page_clear_ezreclaimable(page);
		add_page_to_lru_list(page, locked_lruvec);
	} else {
		page_clear_ezreclaimable(page);
	}
	unlock_page_lruvec_irqrestore_dup(locked_lruvec, flags);

	ezreclaimable_promote_cnt++;
	return true;
}

static void ezr_vh_filemap_get_page(void *data, struct address_space *mapping,
				     pgoff_t index, int fgp_flags, gfp_t gfp_mask,
				     struct page *page)
{
	if (!page)
		return;

	page_ezreclaimable_promote(page);
}

static void ezr_vh_filemap_pages(void *data, struct page *page)
{
	page_ezreclaimable_promote(page);
}

/******************************************************************************
 *                          migrate
 *      set newfolio ezr flag
 ******************************************************************************/
static void ezr_vh_look_around_migrate_page(void *data, struct page *page,
					     struct page *newpage)
{
	if (page_test_ezreclaimable(page))
		page_mark_ezreclaimable(newpage);
}

/******************************************************************************
 *                          ezreclaimd
 *	tune swappiness
 *      refault timer & mglru workingset refault (get_type_to_scan_dup)
 *
 ******************************************************************************/
static inline bool inactive_file_is_low(void)
{
	return global_node_page_state(NR_INACTIVE_FILE) < totalram_pages() / 16;
}

static inline bool inactive_anon_is_low(void)
{
	return global_node_page_state(NR_INACTIVE_ANON) < totalram_pages() / 16;
}

static inline unsigned int system_cur_avail_buffers(void)
{
	return si_mem_available() >> 8;
}

static inline unsigned int get_min_avail_buffers_value(void)
{
	/* workaround here, todo refactor this */
	if (ezr_min_avail_buffer)
		return ezr_min_avail_buffer;
	return ezr_min_avail_buffers_value();
}

static inline unsigned int get_high_avail_buffers_value(void)
{
	if (ezr_high_avail_buffer)
		return ezr_high_avail_buffer;
	return ezr_high_avail_buffers_value();
}

static bool high_buffer_is_suitable(void)
{
	u32 curr_buffers = system_cur_avail_buffers();

	if (curr_buffers >= get_high_avail_buffers_value())
		return true;

	return false;
}

static bool min_buffer_is_suitable(void)
{
	u32 curr_buffers = system_cur_avail_buffers();

	if (curr_buffers >= get_min_avail_buffers_value())
		return true;

	return false;
}

static bool ezr_need_refill(void)
{
	long delta;
	struct ezr_struct *ezrs = &g_ezr;

	delta = ezr_wmarks[EZR_WM_DIRECT_RECLAIM] - atomic_read(&ezreclaimable_nr);
	return delta > ezrs->min_batch;
}

static void ezr_vh_tune_swappiness(void *data, int *swappiness)
{
	struct ezr_struct *ezrs = &g_ezr;

	if (!current_is_demote_files())
		return;

	if (ezrs->wake_flags == WAKEUP_AVAILABLE) {
		if (ezr_need_refill() && !inactive_file_is_low()) {
			*swappiness = 180;
			return;
		}
		*swappiness = 200;
		return;
	}
	*swappiness = 2;
}

static void refault_poll_timer_fn(struct timer_list *unused)
{
	struct ezr_struct *data = &g_ezr;
	unsigned long lru, ws;

	/* calc thrash file */
	lru = global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_FILE);
	ws = global_node_page_state_pages(WORKINGSET_REFAULT_FILE);
	data->thrashing_file_pct = (ws - data->last_ws_refault_file) * 100 /
		(data->last_file_lru + 1);
	data->last_ws_refault_file = ws;
	data->last_file_lru = lru;

	/* calc thrash anon */
	lru = global_node_page_state(NR_ACTIVE_ANON) +
		global_node_page_state(NR_INACTIVE_ANON);
	ws = global_node_page_state_pages(WORKINGSET_REFAULT_ANON);
	data->thrashing_anon_pct = (ws - data->last_ws_refault_anon) * 100 /
		(data->last_anon_lru + 1);
	data->last_ws_refault_anon = ws;
	data->last_anon_lru = lru;

	if (data->thrashing_file_pct > 10 || data->thrashing_anon_pct > 10)
		ezr_logi("thrashing_file_pct: %d thrashing_anon_pct: %d\n",
			 data->thrashing_file_pct, data->thrashing_anon_pct);

	if (data->thrashing_file_pct > data->thrashing_limit_pct)
		data->jiffies_file_thrashing = jiffies + 2 * data->window_sz_hz;
	if (data->thrashing_anon_pct > data->thrashing_limit_pct)
		data->jiffies_anon_thrashing = jiffies + 2 * data->window_sz_hz;
	wakeup_ezreclaimd();
	mod_timer(&data->refault_timer, jiffies + data->window_sz_hz);
}

static void ezr_refault_timer_init(unsigned long window_sz_hz)
{
	struct ezr_struct *data = &g_ezr;

	timer_setup(&data->refault_timer, refault_poll_timer_fn, 0);
	mod_timer(&data->refault_timer, jiffies + data->window_sz_hz);
	ezr_logi("timer enable, window_size: %lu\n", data->window_sz_hz);
}

struct ctrl_pos {
	unsigned long refaulted;
	unsigned long total;
	int gain;
};

static bool positive_ctrl_err_dup(struct ctrl_pos *sp, struct ctrl_pos *pv)
{
	/*
	 * Return true if the PV has a limited number of refaults or a lower
	 * refaulted/total than the SP.
	 */
	return pv->refaulted < MIN_LRU_BATCH ||
	       pv->refaulted * (sp->total + MIN_LRU_BATCH) * sp->gain <=
	       (sp->refaulted + 1) * pv->total * pv->gain;
}


static void read_ctrl_pos_dup(struct lruvec *lruvec, int type, int tier, int gain,
			  struct ctrl_pos *pos)
{
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	int hist = lru_hist_from_seq(lrugen->min_seq[type]);

	pos->refaulted = lrugen->avg_refaulted[type][tier] +
			 atomic_long_read(&lrugen->refaulted[hist][type][tier]);
	pos->total = lrugen->avg_total[type][tier] +
		     atomic_long_read(&lrugen->evicted[hist][type][tier]);
	if (tier)
		pos->total += lrugen->protected[hist][type][tier - 1];
	pos->gain = gain;
}

static int get_type_to_scan_dup(struct lruvec *lruvec, int swappiness)
{
	struct ctrl_pos sp, pv;
	int gain[ANON_AND_FILE] = { swappiness, 200 - swappiness };

	/*
	 * Compare the first tier of anon with that of file to determine which
	 * type to scan. Also need to compare other tiers of the selected type
	 * with the first tier of the other type to determine the last tier (of
	 * the selected type) to evict.
	 */
	read_ctrl_pos_dup(lruvec, LRU_GEN_ANON, 0, gain[LRU_GEN_ANON], &sp);
	read_ctrl_pos_dup(lruvec, LRU_GEN_FILE, 0, gain[LRU_GEN_FILE], &pv);
	return positive_ctrl_err_dup(&sp, &pv);
}

static bool ezreclaimd_should_sleep(void)
{
	if (ezr_display_off()) {
		inc_ezr_event(EZR_SLEEP_DISPLAY_OFF);
		return true;
	}

	if (osvelte_test_scene(MM_SCENE_CAMERA)) {
		inc_ezr_event(EZR_SLEEP_CAMERA);
		return true;
	}
	return false;
}

static int read_system_state(int flags, bool ttwu)
{
	struct ezr_struct *ezrs = &g_ezr;

	if (flags == WAKEUP_AVAILABLE) {
		if (ezr_swapd_pasue())
			return EZR_STOP_SWAPD_PAUSE;
		if (time_before(jiffies, ezrs->jiffies_anon_thrashing))
			return EZR_STOP_ANON_THRASHING;
		if (inactive_anon_is_low())
			return EZR_STOP_LOW_ANON;
		if (!ezr_free_zram_is_ok())
			return EZR_STOP_LOW_SWAP;
		if (high_buffer_is_suitable())
			return EZR_STOP_AVAILABLE_OK;
		/* TTWU check min_buffer_is_suitable */
		if (!ttwu)
			return 0;
		if (ttwu && !min_buffer_is_suitable())
			return 0;
		return EZR_STOP_UNKNOWN;
	}

	if (flags == WAKEUP_DEMOTE) {
		if (time_before(jiffies, ezrs->jiffies_file_thrashing))
			return EZR_STOP_FILE_THRASHING;
		if (inactive_file_is_low())
			return EZR_STOP_LOW_FILE;
		if (!ezr_need_refill())
			return EZR_STOP_DEMOTE_OK;
		return 0;
	}
	return EZR_STOP_UNKNOWN;
}

static void wakeup_ezreclaimd(void)
{
	struct ezr_struct *ezrs = &g_ezr;
	int flags;
	int ret;

	if (!ezrs->ezreclaimd_task)
		return;

	/* if ezreclaimd running, return */
	if (!waitqueue_active(&ezrs->ezreclaimd_wait))
		return;

	if (ezreclaimd_should_sleep())
		return;

	/* first check mem_availiable  */
	for (flags = WAKEUP_AVAILABLE; flags <= WAKEUP_DEMOTE; flags++) {
		ret = read_system_state(flags, true);
		if (!ret)
			break;
		if (ret != EZR_STOP_UNKNOWN)
			inc_ezr_event(ret);
	}
	/* system state not good, just return */
	if (ret)
		return;
	/* wakeup */
	ezrs->wake_flags = flags;
	wake_up_interruptible(&ezrs->ezreclaimd_wait);
}

static int shrink_ezr_pages(struct scan_control *sc);

static int ezreclaimd(void *p)
{
	struct task_struct *tsk = current;
	struct ezr_struct *ezrs = &g_ezr;
	int retries = 0, max_retries, flag, min_batch;
	int ret;
	unsigned long nr;
	unsigned long start_demote, reclaimed;
	unsigned long start_js, reclaim_jiffies;

	tsk->flags |= PF_MEMALLOC;
	set_freezable();

	while (true) {
		wait_event_freezable(ezrs->ezreclaimd_wait, ezrs->wake_flags);

		/* initlialize */
		flag = ezrs->wake_flags;
		retries = 0;
		reclaimed = 0;
		start_js = jiffies;
		start_demote = ezreclaimable_demote_cnt;
		min_batch = ezrs->min_batch;
		ret = -1;
		if (flag == WAKEUP_AVAILABLE)
			max_retries = M2P(get_high_avail_buffers_value() -
					  get_min_avail_buffers_value()) / min_batch;
		else
			max_retries = M2P(LOOP_BATCH_MB) / min_batch;

		mm_trace_fmt_begin("%d,%d,%d", flag, max_retries,
				   atomic_read(&ezreclaimable_nr));
start_over:
		if (ezreclaimd_should_sleep())
			goto out;

		ret = read_system_state(flag, false);
		if (ret) {
			if (ret != EZR_STOP_UNKNOWN)
				inc_ezr_event(ret);
			goto out;
		}

		/* used by abort scan */
		ezrs->last_demote_cnt = ezreclaimable_demote_cnt;
		nr = try_to_free_mem_cgroup_pages(root_mem_cgroup, min_batch, GFP_KERNEL, true);
		reclaimed += nr;

		if (time_after_eq(jiffies, start_js + ezr_reclaim_timeout)) {
			inc_ezr_event(EZR_RC_SLOW);
			goto out;
		}

		if (retries++ < max_retries)
			goto start_over;
out:
		mm_trace_fmt_end();
		reclaim_jiffies = jiffies - start_js;
		if (flag == WAKEUP_AVAILABLE)
			ezr_logi("wake_flag:1 retries:[%d-%d] avail:%d wm:[%d-%d] ret:%d demote:%lu reclaimed:%lu ezr:%d dur:%u\n",
				 retries, max_retries,
				 system_cur_avail_buffers(),
				 get_min_avail_buffers_value(),
				 get_high_avail_buffers_value(), ret,
				 ezreclaimable_demote_cnt - start_demote,
				 reclaimed, atomic_read(&ezreclaimable_nr),
				 jiffies_to_msecs(reclaim_jiffies));
		else
			ezr_logi("wake_flag:2 retries:[%d-%d] ret:%d demote:%lu reclaimed:%lu ezr:%d dur:%u\n",
				 retries, max_retries, ret,
				 ezreclaimable_demote_cnt - start_demote,
				 reclaimed, atomic_read(&ezreclaimable_nr),
				 jiffies_to_msecs(reclaim_jiffies));

		/* recalim : sleep = 1 : 1 */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(reclaim_jiffies);
		ezrs->wake_flags = 0;
	}
	return 0;
}

/******************************************************************************
 *                          ezr memory reclaim
 *      lru_gen_del_folio
 *		clear_reclaimed_folio
 *			isolate_ezr_folios & reclaim_pages
 *				iter_memcgs
 *					shrink_ezr_folios
 ******************************************************************************/
static inline bool ezreclaimable_del_page(struct lruvec *lruvec, struct page *page)
{
	if (!page_test_ezreclaimable(page))
		return false;

	list_del(&page->lru);
	atomic_add(-compound_nr(page), &ezreclaimable_nr);
	return true;
}

static void ezr_vh_lru_gen_del_page_skip(void *data, struct lruvec *lruvec,
					 struct page *page, bool *skip)
{
	if (ezreclaimable_del_page(lruvec, page))
		*skip = true;
}

static void ezr_vh_clear_reclaimed_page(void *data, struct page *page, bool reclaimed)
{
	if (reclaimed && page_test_ezreclaimable(page)) {
		ezreclaimable_reclaim_cnt++;
		page_clear_ezreclaimable(page);
	}
}

static bool sort_ezreclaimable_page(struct lruvec *lruvec, struct page *page)
{
	int delta = compound_nr(page);
	bool success;

	if (page_evictable(page))
		return false;

	page_clear_ezreclaimable(page);
	success = lru_gen_del_page(lruvec, page, true);
	VM_WARN_ON_ONCE_PAGE(!success, folio);
	SetPageUnevictable(page);
	add_page_to_lru_list(page, lruvec);
	__count_vm_events(UNEVICTABLE_PGCULLED, delta);
	return true;
}

static void __nocfi isolate_ezr_pages(struct scan_control *sc,
				      struct lruvec *lruvec,
				      struct list_head *list)
{
	int skipped = 0;
	int isolated = 0;
	int sorted = 0;
	int remaining = sc->nr_to_reclaim;
	struct list_head *head = ezr_lru(lruvec);
	int scanned = 0;
	LIST_HEAD(moved);

	while (!list_empty(head)) {
		struct page *page = lru_to_page(head);
		int delta = compound_nr(page);

		VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);
		VM_WARN_ON_ONCE_PAGE(PageActive(page), page);

		scanned += delta;

		if (sort_ezreclaimable_page(lruvec, page))
			sorted += delta;
		else if (isolate_page(lruvec, page, sc)) {
			list_add(&page->lru, list);
			isolated += delta;
		} else {
			list_move(&page->lru, &moved);
			skipped += delta;
		}

		/* whether we need remaining here? */
		if (!--remaining || isolated >= sc->nr_to_reclaim)
			break;
	}
	sc->nr_scanned += scanned;

	if (skipped)
		list_splice(&moved, head);
}

static int iter_memcg_callback(struct mem_cgroup *memcg,
			       struct lruvec *lruvec, void *private)
{
	struct scan_control *sc = (struct scan_control *)private;
	int mode = sc->android_vendor_data1;
	LIST_HEAD(list);

	if (mode == EZR_RECLAIM_KSWAPD) {
		/* do nothing */
	} else if (mode == EZR_RECLAIM_DIRECT_RECLAIM) {
		if (get_type_to_scan_dup(lruvec, 7) == LRU_GEN_ANON)
			return 0;
	}

	spin_lock_irq(&lruvec->lru_lock);
	isolate_ezr_pages(sc, lruvec, &list);
	spin_unlock_irq(&lruvec->lru_lock);

	sc->nr_reclaimed += reclaim_pages(&list);
	/* abort reclaim */
	if (sc->nr_reclaimed >= sc->nr_to_reclaim)
		return 1;
	return 0;
}

static void __nocfi do_iter_mem_cgroups_lruvec(int (*cb)(struct mem_cgroup *memcg,
					  struct lruvec *lruvec, void *private),
				void *private)
{
	struct mem_cgroup *memcg;
	int nid, ret;

	/* fixme if more node in device */
	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);

		memcg = kp_mem_cgroup_iter(NULL, NULL, NULL);
		do {
			struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);

			ret = cb(memcg, lruvec, private);
			if (ret) {
				kp_mem_cgroup_iter_break(NULL, memcg);
				break;
			}
			memcg = kp_mem_cgroup_iter(NULL, memcg, NULL);
		} while (memcg);
	}
}

static int shrink_ezr_pages(struct scan_control *sc)
{
	do_iter_mem_cgroups_lruvec(iter_memcg_callback, sc);
	return sc->nr_reclaimed;
}

/******************************************************************************
 *                          ezr memory reclaim
 *	ezr_memory_reclaim_all
 *	ezr_memory_reclaim
 ******************************************************************************/
static inline void shrink_ezr_pages_all(struct scan_control *sc)
{
	unsigned int nr_retries = MAX_RECLAIM_RETRIES / 4;
	unsigned long long start_nsecs;
	unsigned long nr_to_reclaim, nr_reclaimed;

	start_nsecs = sched_clock();
	nr_to_reclaim = atomic_read(&ezreclaimable_nr);
	nr_reclaimed = 0;
	while (nr_reclaimed < nr_to_reclaim) {
		unsigned long batch_size = (nr_to_reclaim - nr_reclaimed) / 4;
		unsigned long reclaimed;

		sc->nr_to_reclaim = batch_size;
		/* shrink_ezr_pages assign reclaimed to nr_reclaimed, so reset here. */
		sc->nr_reclaimed = 0;

		reclaimed = shrink_ezr_pages(sc);
		if (!reclaimed && !nr_retries--)
			goto out;

		nr_reclaimed += reclaimed;
	}
	sc->nr_reclaimed = nr_reclaimed;
out:
	ezr_logi("EZRECLAIM_MIN: nr_to_reclaim: %lu nr_reclaimed: %lu use(ms): %llu\n",
		nr_to_reclaim, sc->nr_reclaimed, (sched_clock() - start_nsecs) / NSEC_PER_MSEC);
}

/******************************************************************************
 *                          memory reclaim boost
 *	direct_reclaim
 *	kswapd
 ******************************************************************************/
static void ezr_rvh_perform_reclaim(void *data, int order, gfp_t gfp_mask,
				    nodemask_t *nodemask,
				    unsigned long *nr_reclaimed,
				    bool *skip)
{
	struct scan_control sc = {
		.gfp_mask = current_gfp_context(gfp_mask),
		.android_vendor_data1 = EZR_RECLAIM_DIRECT_RECLAIM,
	};
	long wmark, delta;

	if (order) {
		inc_ezr_event(EZR_RC_FAILED_ORDER);
		return;
	}

	if (unlikely(osvelte_test_scene(MM_SCENE_CAMERA)))
		wmark = 0;
	else
		wmark = ezr_wmarks[EZR_WM_MIN];
	delta = atomic_read(&ezreclaimable_nr) - wmark;
	if (delta < MIN_LRU_BATCH)
		return;

	sc.nr_to_reclaim = MIN_LRU_BATCH;
	shrink_ezr_pages(&sc);
	if (sc.nr_reclaimed < SWAP_CLUSTER_MAX) {
		inc_ezr_event(EZR_RC_FAILED0);
		return;
	}

	*skip = true;
	*nr_reclaimed = sc.nr_reclaimed;
	inc_ezr_event(EZR_RC_DIRECT);
	wakeup_ezreclaimd();
}

static void ezr_rvh_kswapd_shrink_node(void *data, unsigned long *nr_to_reclaim)
{
	struct ezr_struct *ezrs = &g_ezr;
	struct scan_control sc = {
		.android_vendor_data1 = EZR_RECLAIM_KSWAPD,
	};
	struct scan_control *kswapd_sc;
	long wmark, delta;

	kswapd_sc = container_of(nr_to_reclaim, struct scan_control, nr_to_reclaim);
	if (unlikely(osvelte_test_scene(MM_SCENE_CAMERA)))
		wmark = 0;
	else
		wmark = ezr_wmarks[EZR_WM_DIRECT_RECLAIM];
	delta = atomic_read(&ezreclaimable_nr) - wmark;
	if (delta < ezrs->min_batch)
		return;

	sc.nr_to_reclaim = min_t(unsigned long, *nr_to_reclaim,
				 (unsigned long)delta);
	shrink_ezr_pages(&sc);
	kswapd_sc->nr_reclaimed += sc.nr_reclaimed;
	inc_ezr_event(EZR_RC_KSWAPD);
	wakeup_ezreclaimd();
}

/* reclaim vh */
static inline void show_val_meminfo(struct seq_file *m,
				    const char *str, long size)
{
	char name[17];
	int len = strlen(str);

	if (len <= 15) {
		snprintf(name, sizeof(name), "%s:", str);
	} else {
		strscpy(name, str, 15);
		name[15] = ':';
		name[16] = '\0';
	}

	seq_printf(m, "%-16s%8ld kB\n", name, size);
}

/******************************************************************************
 *                          meminfo & mem_availiable adjust
 ******************************************************************************/
static void ezr_vh_meminfo_proc_show(void *data, struct seq_file *m)
{
	show_val_meminfo(m, "Ezr",
			 atomic_read(&ezreclaimable_nr) << (PAGE_SHIFT - 10));
}

static void ezr_vh_mglru_should_abort_scan(void *data, unsigned long *nr_reclaimed)
{
	struct scan_control *sc;
	struct ezr_struct *ezrs = &g_ezr;

	if (!current_is_demote_files())
		return;

	sc = container_of(nr_reclaimed, struct scan_control, nr_reclaimed);
	sc->nr_reclaimed += ezreclaimable_demote_cnt - g_ezr.last_demote_cnt;
	ezrs->last_demote_cnt = ezreclaimable_demote_cnt;
}

static void ezr_vh_available_adjust(void *data, unsigned long *available)
{
	long delta;

	delta = atomic_read(&ezreclaimable_nr) -
		(int)ezr_wmarks[EZR_WM_MIN];
	if (delta < 0)
		delta = 0;
	*available += delta;
}

/******************************************************************************
 *                          sysfs knobs
 *      stats
 *      reclaim_test
 *      wmarks
 *      thrashing limit pct
 ******************************************************************************/
static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int size = 0, i;
	struct ezr_struct *ezrs = &g_ezr;

	size += sysfs_emit_at(buf, size, "nr_pages             %u\n",
			      atomic_read(&ezreclaimable_nr));

	size += sysfs_emit_at(buf, size, "promote_cnt          %lu\n",
			      ezreclaimable_promote_cnt);
	size += sysfs_emit_at(buf, size, "demote_cnt           %lu\n",
			      ezreclaimable_demote_cnt);
	size += sysfs_emit_at(buf, size, "reclaim_cnt          %lu\n",
			      ezreclaimable_reclaim_cnt);

	size += sysfs_emit_at(buf, size, "wm_direct            %lu\n",
			      ezr_wmarks[EZR_WM_DIRECT_RECLAIM]);
	size += sysfs_emit_at(buf, size, "wm_min               %lu\n",
			      ezr_wmarks[EZR_WM_MIN]);

	size += sysfs_emit_at(buf, size, "thrashing_limit_pct  %u\n",
			      ezrs->thrashing_limit_pct);
	size += sysfs_emit_at(buf, size, "display_off          %d\n",
			      ezr_display_off());

	for (i = EZR_SLEEP_DISPLAY_OFF; i < NR_MAX_EZR_STAT; i++)
		size += sysfs_emit_at(buf, size, "%-20s %u\n",
				      ezr_stat_item_txt[i],
				      atomic_read(ezrs->stat_items + i));
	return size;
}

static ssize_t reclaim_test_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long value;
	struct scan_control sc = {
		.android_vendor_data1 = EZR_RECLAIM_ALL,
	};

	ret = kstrtoul(buf, 10, &value);
	if (ret < 0)
		return ret;
	if (value != 1)
		return -EINVAL;

	shrink_ezr_pages_all(&sc);
	return count;
}

static ssize_t wmarks_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long wmarks[NR_EZR_WMARKS];
	int ret, i;

	ret = sscanf(buf, "%lu %lu", &wmarks[EZR_WM_DIRECT_RECLAIM], &wmarks[EZR_WM_MIN]);
	if (ret != 2)
		return -EINVAL;

	for (i = 0; i < 2; i++)
		ezr_wmarks[i] = wmarks[i];
	return count;
}

static ssize_t thrashing_limit_pct_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	int ret;
	unsigned long value;

	ret = kstrtoul(buf, 10, &value);
	if (ret < 0)
		return ret;

	if (value <= 0)
		return -EINVAL;
	g_ezr.thrashing_limit_pct = value;
	return count;
}

static struct kobj_attribute stats_attr = __ATTR_RO(stats);
static struct kobj_attribute reclaim_test_attr = __ATTR_WO(reclaim_test);
static struct kobj_attribute wmarks_attr = __ATTR_WO(wmarks);
static struct kobj_attribute thrashing_limit_pct_attr = __ATTR_WO(thrashing_limit_pct);

static struct attribute *attrs[] = {
	&stats_attr.attr,
	&reclaim_test_attr.attr,
	&wmarks_attr.attr,
	&thrashing_limit_pct_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static int __nocfi register_vendor_hooks(void)
{
	int ret;

	ret = register_trace_android_vh_si_mem_available_adjust(ezr_vh_available_adjust, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_shrink_page_list(ezr_vh_shrink_page_list, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_tune_swappiness(ezr_vh_tune_swappiness, NULL);
	if (ret)
		return ret;

	/* mglru hook */
	ret = register_trace_android_vh_lru_gen_add_page_skip(ezr_vh_lru_gen_add_page_skip, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_lru_gen_del_page_skip(ezr_vh_lru_gen_del_page_skip, NULL);
	if (ret)
		return ret;

	/* reclaim hook */
	ret = register_trace_android_rvh_perform_reclaim(ezr_rvh_perform_reclaim, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_rvh_kswapd_shrink_node(ezr_rvh_kswapd_shrink_node, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_look_around_migrate_page(ezr_vh_look_around_migrate_page, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_clear_reclaimed_page(ezr_vh_clear_reclaimed_page, NULL);
	if (ret)
		return ret;

	/* demote hook */
	ret = register_trace_android_vh_meminfo_proc_show(ezr_vh_meminfo_proc_show, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_keep_reclaimed_page(ezr_vh_keep_reclaimed_page, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_mglru_should_abort_scan(ezr_vh_mglru_should_abort_scan, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_evict_pages_bypass(ezr_vh_evict_pages_bypass, NULL);
	if (ret)
		return ret;

	/* filemap hook */
	ret = register_trace_android_vh_pagecache_get_page(ezr_vh_filemap_get_page, NULL);
	if (ret)
		return ret;
	ret = register_trace_android_vh_filemap_pages(ezr_vh_filemap_pages, NULL);
	if (ret)
		return ret;
	return ret;
}

static int __init ezreclaimable_init(void)
{
	int ret;
	struct ezr_struct *ezrs = &g_ezr;
	unsigned long total_ram;
	struct kobject *ezr_sysfs_kobj;
	struct kobject *oplus_mm_kobj = NULL;
	struct config_ezreclaimd *config;

	config = oplus_read_mm_config(module_name_ezreclaimd);
	if(!config) {
		ezr_loge("ezr do not support\n");
		return 0;
	}

	if (!config->enable) {
		ezr_loge("disabled by config\n");
		return 0;
	}

	if (ezr_read_symbols_address()) {
		ezr_loge("kprobe failed\n");
		return -EINVAL;
	}

	oplus_mm_kobj = (struct kobject *)osvelte_read_symbol(OPLUS_MM_KOBJ, true);
	if (!oplus_mm_kobj) {
		ezr_logi("create oplus_mm_kobj failed\n");
		return -EINVAL;
	}

	ezr_sysfs_kobj = kobject_create_and_add("ezr", oplus_mm_kobj);
	if (!ezr_sysfs_kobj) {
		ezr_loge("failed to create sysfs kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(ezr_sysfs_kobj, &attr_group);
	if (ret) {
		ezr_loge("failed to create syfs attr group\n");
		kobject_put(oplus_mm_kobj);
	}
	ezr_logi("stage1: create sysfs group\n");

	/* copy from init.oplus.nandswap.sh */
	total_ram = totalram_pages();
	if (total_ram <= (SZ_1G / PAGE_SIZE * 6)) {
		ezr_wmarks[EZR_WM_DIRECT_RECLAIM] = (SZ_1M / PAGE_SIZE * 128);
		ezr_wmarks[EZR_WM_MIN] = 0;
	} else if (total_ram <= (SZ_8G / PAGE_SIZE)) {
		ezr_wmarks[EZR_WM_DIRECT_RECLAIM] = (SZ_1M / PAGE_SIZE * 320);
		ezr_wmarks[EZR_WM_MIN] = (SZ_1M / PAGE_SIZE * 128);
		ezr_min_avail_buffer = 2400;
		ezr_high_avail_buffer = 2500;
	} else {
		ezr_wmarks[EZR_WM_DIRECT_RECLAIM] = (SZ_1M / PAGE_SIZE * 640);
		ezr_wmarks[EZR_WM_MIN] = (SZ_1M / PAGE_SIZE * 512);
	}

	ezr_logi("stage2: resgister vendor hook\n");
	ret = register_vendor_hooks();
	if (ret) {
		ezr_loge("failed to register vendor hook\n");
		return ret;
	}

	ezrs->ezreclaimd_task = kthread_run(ezreclaimd, ezrs, "ezreclaimd");
	ezrs->window_sz_hz = msecs_to_jiffies(1000);
	ezr_set_swapd_pid(ezrs->ezreclaimd_task->tgid);
	ezr_refault_timer_init(ezrs->window_sz_hz);
	osvelte_register_symbol(OPLUS_TASK_EZRECLAIMD, ezrs->ezreclaimd_task);
	ezr_register_nr_pages(&ezreclaimable_nr);
	ezr_logi("stage3: init done, window_sz_hz: %lu\n", ezrs->window_sz_hz);
	return 0;
}

static void __exit ezreclaimable_exit(void)
{
	ezr_loge("unsupport for now\n");
}

module_init(ezreclaimable_init);
module_exit(ezreclaimable_exit);
MODULE_LICENSE("GPL v2");
