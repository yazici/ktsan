#include "ktsan.h"

#include <linux/spinlock.h>

void kt_mtx_pre_lock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr, bool try)
{
	/* Will be used for deadlock detection.
	   We can also put sleeps for random time here. */
}

void kt_mtx_post_lock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr, bool try,
		      bool success)
{
	kt_tab_sync_t *sync;

	/* Sometimes even locks that are not trylocks might fail.
	   For example a thread calling mutex_lock might be rescheduled.
	   In that case we call mtx_post_lock(try = false, success = false). */
	/* BUG_ON(!try && !success); */

	if (!success)
		return;

	sync = kt_sync_ensure_created(thr, pc, addr);
	if (!sync)
		return;

	/* This can catch unsafe publication of a mutex. */
	kt_access(thr, pc, addr, KT_ACCESS_SIZE_1, true, false);

	kt_mutex_lock(thr, pc, sync->uid, wr);

	kt_acquire(thr, pc, sync);

	BUG_ON(sync->lock_tid != -1);
	if (wr)
		sync->lock_tid = thr->id;
	sync->last_lock_time = kt_clk_get(&thr->clk, thr->id);

	kt_spin_unlock(&sync->tab.lock);
}

void kt_mtx_pre_unlock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr)
{
	kt_tab_sync_t *sync;

	sync = kt_sync_ensure_created(thr, pc, addr);
	if (!sync)
		return;

	/* This can catch race between unlock and mutex destruction. */
	kt_access(thr, pc, addr, KT_ACCESS_SIZE_1, true, false);

	kt_mutex_unlock(thr, sync->uid, wr);

	kt_release(thr, pc, sync);

	if (wr) {
		BUG_ON(sync->lock_tid == -1);
		if (wr && sync->lock_tid != thr->id)
			kt_report_bad_mtx_unlock(thr, pc, sync);
		sync->lock_tid = -1;
	}
	BUG_ON(sync->lock_tid != -1);
	sync->last_unlock_time = kt_clk_get(&thr->clk, thr->id);

	kt_spin_unlock(&sync->tab.lock);
}

void kt_mtx_post_unlock(kt_thr_t *thr, uptr_t pc, uptr_t addr, bool wr)
{
}

void kt_mtx_downgrade(kt_thr_t *thr, uptr_t pc, uptr_t addr)
{
	kt_tab_sync_t *sync;

	sync = kt_sync_ensure_created(thr, pc, addr);
	if (!sync)
		return;

	BUG_ON(sync->lock_tid == -1);
	if (sync->lock_tid != thr->id)
		kt_report_bad_mtx_unlock(thr, pc, sync);
	sync->lock_tid = -1;

	kt_mutex_downgrade(thr, sync->uid);

	kt_spin_unlock(&sync->tab.lock);
}
