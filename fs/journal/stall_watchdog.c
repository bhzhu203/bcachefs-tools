// SPDX-License-Identifier: GPL-2.0
/*
 * Stall watchdog
 *
 * The hangs this is meant to diagnose wedge every writer to the filesystem
 * (dirty throttling holding inode locks, writeback that never completes), so
 * the machine is unreachable and there's no chance to capture stacks
 * interactively; the kernel's hung-task checker also runs out of warnings and
 * gets suppressed. Instead, periodically sample this filesystem's own
 * backing-device dirty/writeback counters, and if they stop moving for a
 * while, dump the journal state and the stacks of all blocked tasks to the
 * kernel log, which lands on the console/serial log for post-mortem.
 */
#include <linux/backing-dev.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/nmi.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/swap.h>
#include <linux/vmstat.h>

#include "bcachefs.h"
#include "btree/cache.h"
#include "btree/key_cache.h"
#include "journal/journal.h"
#include "journal/stall_watchdog.h"
#include "util/printbuf.h"

#define STALL_CHECK_INTERVAL		(HZ * 10)
/* 6 checks x 10s = 1 minute of zero progress before dumping: */
#define STALL_CHECKS			6
/* Don't dump more than once every 2 minutes while the stall persists: */
#define STALL_DUMP_INTERVAL		(HZ * 120)
/*
 * Dirty pages with no writeback in flight is only suspicious at scale - a
 * small idle dirty working set is normal.
 */
#define STALL_DIRTY_FLOOR		(256 * 1024 * 1024 / PAGE_SIZE)

void bch2_stall_dump_stacks(struct bch_fs *c)
{
	struct task_struct *g, *p;
	unsigned blocked = 0, bch_threads = 0;

	bch_err(c, "dumping stacks of all blocked (TASK_UNINTERRUPTIBLE) tasks and all bch-* threads");

	rcu_read_lock();
	for_each_process_thread(g, p) {
		unsigned state = READ_ONCE(p->__state);

		touch_nmi_watchdog();

		/* skip TASK_IDLE, like sysrq-w: */
		if ((state & TASK_UNINTERRUPTIBLE) && !(state & TASK_NOLOAD)) {
			blocked++;
			sched_show_task(p);
		} else if (!strncmp(p->comm, "bch-", 4)) {
			bch_threads++;
			sched_show_task(p);
		}
	}
	rcu_read_unlock();

	bch_err(c, "stack dump done: %u blocked tasks, %u bch threads",
		blocked, bch_threads);
}

void bch2_stall_dump_meminfo(struct bch_fs *c)
{
	struct sysinfo si;
	CLASS(printbuf, buf)();

	si_meminfo(&si);

	unsigned long slab_rcl	= global_node_page_state(NR_SLAB_RECLAIMABLE_B);
	unsigned long slab_unc	= global_node_page_state(NR_SLAB_UNRECLAIMABLE_B);
	unsigned long anon	= global_node_page_state(NR_ANON_MAPPED) << PAGE_SHIFT;
	unsigned long filecache	= global_node_page_state(NR_FILE_PAGES) << PAGE_SHIFT;
	unsigned long dirty	= global_node_page_state(NR_FILE_DIRTY) << PAGE_SHIFT;
	unsigned long writeback	= global_node_page_state(NR_WRITEBACK) << PAGE_SHIFT;
	long swap_free		= get_nr_swap_pages();

	prt_printf(&buf, bch2_fmt(c,
		"meminfo: total=%luMB free=%luMB available=%luMB anon=%luMB filecache=%luMB slab_reclaimable=%luMB slab_unreclaimable=%luMB dirty=%luMB writeback=%luMB swap_free=%ldMB"),
		si.totalram * si.mem_unit >> 20,
		si.freeram * si.mem_unit >> 20,
		si_mem_available() << PAGE_SHIFT >> 20,
		anon >> 20,
		filecache >> 20,
		slab_rcl >> 20,
		slab_unc >> 20,
		dirty >> 20,
		writeback >> 20,
		swap_free << PAGE_SHIFT >> 20);
	bch2_print_str(c, KERN_ERR, buf.buf);
}

static void bch2_stall_report(struct bch_fs *c, s64 wb, s64 dirty, s64 written, u64 seq_ondisk)
{
	struct bch_fs_btree_cache *bc = &c->btree.cache;
	struct bch_fs_btree_key_cache *kc = &c->btree.key_cache;
	CLASS(printbuf, buf)();

	/*
	 * The btree cache counters discriminate between the known wedge
	 * shapes: commits blocked with in_flight_inner pinned high means
	 * btree node writes aren't completing, while in_flight low with a
	 * high dirty ratio means reclaim isn't writing dirty nodes out.
	 */
	prt_printf(&buf, bch2_fmt(c,
		"writeback stall detected: nr_writeback=%lli nr_dirty=%lli nr_written=%lli btree_in_flight_inner=%li btree_write_io_limit=%u btree_cache_live=%zu btree_cache_dirty=%zu btree_throttle=%u journal seq_ondisk=%llu cur_seq=%llu"),
		wb, dirty, written,
		atomic_long_read(&bc->nr_in_flight_inner),
		BTREE_WRITE_IO_LIMIT(c),
		btree_cache_nr_live(bc), btree_cache_nr_dirty(bc),
		READ_ONCE(bc->should_throttle),
		seq_ondisk, atomic64_read(&c->journal.seq));

	/*
	 * Key cache entries are journal-pinned slab: if this is what's eating
	 * memory, OOM-killing userspace can't free it. nr_dirty vs the
	 * must_wait threshold shows whether commit backpressure is engaged.
	 */
	prt_printf(&buf, "\nkey cache: nr_keys=%li nr_dirty=%li must_wait=%zi requested_to_free=%lu freed=%lu skipped_dirty=%lu skipped_accessed=%lu skipped_lock_fail=%lu",
		atomic_long_read(&kc->nr_keys),
		atomic_long_read(&kc->nr_dirty),
		__bch2_btree_key_cache_must_wait(c),
		kc->requested_to_free, kc->freed,
		kc->skipped_dirty, kc->skipped_accessed,
		kc->skipped_lock_fail);

	bch2_journal_debug_to_text(&buf, &c->journal);
	bch2_print_str(c, KERN_ERR, buf.buf);

	bch2_stall_dump_meminfo(c);
	bch2_stall_dump_stacks(c);
}

static int stall_watchdog_thread(void *arg)
{
	struct bch_fs *c = arg;
	struct journal *j = &c->journal;

	unsigned stalled = 0;
	s64 last_wb = -1, last_dirty = -1, last_written = -1;
	unsigned long last_dump = jiffies - STALL_DUMP_INTERVAL;

	while (!kthread_should_stop()) {
		schedule_timeout_interruptible(STALL_CHECK_INTERVAL);
		if (kthread_should_stop())
			break;

		/*
		 * Must be scoped to this filesystem's own bdi: the global
		 * page counters pick up writeback from every other device on
		 * the machine, which looks like progress and keeps resetting
		 * the stall window. s_bdi isn't set until bch2_mount() calls
		 * super_setup_bdi(), which happens after this thread starts -
		 * until then there are no writers whose dirty state could
		 * wedge, so just wait.
		 */
		struct super_block *sb = READ_ONCE(c->vfs_sb);
		if (!sb || sb->s_bdi == &noop_backing_dev_info) {
			stalled = 0;
			continue;
		}

		s64 wb	= wb_stat_sum(&sb->s_bdi->wb, WB_WRITEBACK);
		s64 dirty = wb_stat_sum(&sb->s_bdi->wb, WB_RECLAIMABLE);
		s64 written = wb_stat_sum(&sb->s_bdi->wb, WB_WRITTEN);
		u64 seq = READ_ONCE(j->seq_ondisk);

		/*
		 * nr_written is monotonic, so unchanged across an interval
		 * means not a single writeback completed. Journal progress
		 * deliberately does not count: a wedged writeback can
		 * coexist with unrelated commits (e.g. snapshot deletion)
		 * still advancing the journal.
		 */
		bool progress = wb != last_wb || dirty != last_dirty ||
				written != last_written;

		last_wb = wb;
		last_dirty = dirty;
		last_written = written;

		if (progress ||
		    (wb == 0 && dirty < STALL_DIRTY_FLOOR)) {
			stalled = 0;
			continue;
		}

		if (++stalled < STALL_CHECKS)
			continue;

		if (time_before(jiffies, last_dump + STALL_DUMP_INTERVAL))
			continue;
		last_dump = jiffies;

		bch2_stall_report(c, wb, dirty, written, seq);
	}

	return 0;
}

void bch2_stall_watchdog_stop(struct bch_fs *c)
{
	struct task_struct *p = c->stall_watchdog;

	c->stall_watchdog = NULL;

	if (p) {
		kthread_stop(p);
		put_task_struct(p);
	}
}

int bch2_stall_watchdog_start(struct bch_fs *c)
{
	struct task_struct *p;
	int ret;

	if (c->stall_watchdog)
		return 0;

	p = kthread_create(stall_watchdog_thread, c, "bch-stallwd/%s", c->name);
	ret = PTR_ERR_OR_ZERO(p);
	bch_err_msg(c, ret, "creating stall watchdog thread");
	if (ret)
		return ret;

	get_task_struct(p);
	c->stall_watchdog = p;
	wake_up_process(p);

	bch_info(c, "stall watchdog started");
	return 0;
}
