/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * dlmthread.c
 *
 * standalone DLM module
 *
 * Copyright (C) 2004 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 */


#include <linux/module.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/utsname.h>
#include <linux/init.h>
#include <linux/sysctl.h>
#include <linux/random.h>
#include <linux/blkdev.h>
#include <linux/socket.h>
#include <linux/inet.h>
#include <linux/timer.h>
#include <linux/kthread.h>
#include <linux/delay.h>


#include "cluster/heartbeat.h"
#include "cluster/nodemanager.h"
#include "cluster/tcp.h"

#include "dlmapi.h"
#include "dlmcommon.h"
#include "dlmdomain.h"

#define MLOG_MASK_PREFIX (ML_DLM|ML_DLM_THREAD)
#include "cluster/masklog.h"

static int dlm_thread(void *data);
static void dlm_flush_asts(struct dlm_ctxt *dlm);

#define dlm_lock_is_remote(dlm, lock)     ((lock)->ml.node != (dlm)->node_num)

/* will exit holding res->spinlock, but may drop in function */
/* waits until flags are cleared on res->state */
void __dlm_wait_on_lockres_flags(struct dlm_lock_resource *res, int flags)
{
	DECLARE_WAITQUEUE(wait, current);

	assert_spin_locked(&res->spinlock);

	add_wait_queue(&res->wq, &wait);
repeat:
	set_current_state(TASK_UNINTERRUPTIBLE);
	if (res->state & flags) {
		spin_unlock(&res->spinlock);
		schedule();
		spin_lock(&res->spinlock);
		goto repeat;
	}
	remove_wait_queue(&res->wq, &wait);
	__set_current_state(TASK_RUNNING);
}

int __dlm_lockres_has_locks(struct dlm_lock_resource *res)
{
	if (list_empty(&res->granted) &&
	    list_empty(&res->converting) &&
	    list_empty(&res->blocked))
		return 0;
	return 1;
}

/* "unused": the lockres has no locks, is not on the dirty list,
 * has no inflight locks (in the gap between mastery and acquiring
 * the first lock), and has no bits in its refmap.
 * truly ready to be freed. */
int __dlm_lockres_unused(struct dlm_lock_resource *res)
{
	if (!__dlm_lockres_has_locks(res) &&
	    (list_empty(&res->dirty) && !(res->state & DLM_LOCK_RES_DIRTY))) {
		/* try not to scan the bitmap unless the first two
		 * conditions are already true */
		int bit = find_next_bit(res->refmap, O2NM_MAX_NODES, 0);
		if (bit >= O2NM_MAX_NODES) {
			/* since the bit for dlm->node_num is not
			 * set, inflight_locks better be zero */
			BUG_ON(res->inflight_locks != 0);
			return 1;
		}
	}
	return 0;
}


/* Call whenever you may have added or deleted something from one of
 * the lockres queue's. This will figure out whether it belongs on the
 * unused list or not and does the appropriate thing. */
void __dlm_lockres_calc_usage(struct dlm_ctxt *dlm,
			      struct dlm_lock_resource *res)
{
	mlog_entry("%.*s\n", res->lockname.len, res->lockname.name);

	assert_spin_locked(&dlm->spinlock);
	assert_spin_locked(&res->spinlock);

	if (__dlm_lockres_unused(res)){
		if (list_empty(&res->purge)) {
			mlog(0, "putting lockres %.*s:%p onto purge list\n",
			     res->lockname.len, res->lockname.name, res);

			res->last_used = jiffies;
			dlm_lockres_get(res);
			list_add_tail(&res->purge, &dlm->purge_list);
			dlm->purge_count++;
		}
	} else if (!list_empty(&res->purge)) {
		mlog(0, "removing lockres %.*s:%p from purge list, owner=%u\n",
		     res->lockname.len, res->lockname.name, res, res->owner);

		list_del_init(&res->purge);
		dlm_lockres_put(res);
		dlm->purge_count--;
	}
}

void dlm_lockres_calc_usage(struct dlm_ctxt *dlm,
			    struct dlm_lock_resource *res)
{
	mlog_entry("%.*s\n", res->lockname.len, res->lockname.name);
	spin_lock(&dlm->spinlock);
	spin_lock(&res->spinlock);

	__dlm_lockres_calc_usage(dlm, res);

	spin_unlock(&res->spinlock);
	spin_unlock(&dlm->spinlock);
}

static int dlm_purge_lockres(struct dlm_ctxt *dlm,
			     struct dlm_lock_resource *res)
{
	int master;
	int ret = 0;

	spin_lock(&res->spinlock);
	if (!__dlm_lockres_unused(res)) {
		spin_unlock(&res->spinlock);
		mlog(0, "%s:%.*s: tried to purge but not unused\n",
		     dlm->name, res->lockname.len, res->lockname.name);
		return -ENOTEMPTY;
	}
	master = (res->owner == dlm->node_num);
	if (!master)
		res->state |= DLM_LOCK_RES_DROPPING_REF;
	spin_unlock(&res->spinlock);

	mlog(0, "purging lockres %.*s, master = %d\n", res->lockname.len,
	     res->lockname.name, master);

	if (!master) {
		/* drop spinlock...  retake below */
		spin_unlock(&dlm->spinlock);

		spin_lock(&res->spinlock);
		/* This ensures that clear refmap is sent after the set */
		__dlm_wait_on_lockres_flags(res, (DLM_LOCK_RES_SETREF_INPROG |
						  DLM_LOCK_RES_MIGRATING));
		spin_unlock(&res->spinlock);

		/* clear our bit from the master's refmap, ignore errors */
		ret = dlm_drop_lockres_ref(dlm, res);
		if (ret < 0) {
			mlog_errno(ret);
			if (!dlm_is_host_down(ret))
				BUG();
		}
		mlog(0, "%s:%.*s: dlm_deref_lockres returned %d\n",
		     dlm->name, res->lockname.len, res->lockname.name, ret);
		spin_lock(&dlm->spinlock);
	}

	if (!list_empty(&res->purge)) {
		mlog(0, "removing lockres %.*s:%p from purgelist, "
		     "master = %d\n", res->lockname.len, res->lockname.name,
		     res, master);
		list_del_init(&res->purge);
		dlm_lockres_put(res);
		dlm->purge_count--;
	}
	__dlm_unhash_lockres(res);

	/* lockres is not in the hash now.  drop the flag and wake up
	 * any processes waiting in dlm_get_lock_resource. */
	if (!master) {
		spin_lock(&res->spinlock);
		res->state &= ~DLM_LOCK_RES_DROPPING_REF;
		spin_unlock(&res->spinlock);
		wake_up(&res->wq);
	}
	return 0;
}

static void dlm_run_purge_list(struct dlm_ctxt *dlm,
			       int purge_now)
{
	unsigned int run_max, unused;
	unsigned long purge_jiffies;
	struct dlm_lock_resource *lockres;

	spin_lock(&dlm->spinlock);
	run_max = dlm->purge_count;

	while(run_max && !list_empty(&dlm->purge_list)) {
		run_max--;

		lockres = list_entry(dlm->purge_list.next,
				     struct dlm_lock_resource, purge);

		/* Status of the lockres *might* change so double
		 * check. If the lockres is unused, holding the dlm
		 * spinlock will prevent people from getting and more
		 * refs on it -- there's no need to keep the lockres
		 * spinlock. */
		spin_lock(&lockres->spinlock);
		unused = __dlm_lockres_unused(lockres);
		spin_unlock(&lockres->spinlock);

		if (!unused)
			continue;

		purge_jiffies = lockres->last_used +
			msecs_to_jiffies(DLM_PURGE_INTERVAL_MS);

		/* Make sure that we want to be processing this guy at
		 * this time. */
		if (!purge_now && time_after(purge_jiffies, jiffies)) {
			/* Since resources are added to the purge list
			 * in tail order, we can stop at the first
			 * unpurgable resource -- anyone added after
			 * him will have a greater last_used value */
			break;
		}

		dlm_lockres_get(lockres);

		/* This may drop and reacquire the dlm spinlock if it
		 * has to do migration. */
		if (dlm_purge_lockres(dlm, lockres))
			BUG();

		dlm_lockres_put(lockres);

		/* Avoid adding any scheduling latencies */
		cond_resched_lock(&dlm->spinlock);
	}

	spin_unlock(&dlm->spinlock);
}

static void dlm_shuffle_lists(struct dlm_ctxt *dlm,
			      struct dlm_lock_resource *res)
{
	struct dlm_lock *lock, *target;
	struct list_head *iter;
	struct list_head *head;
	int can_grant = 1;

	//mlog(0, "res->lockname.len=%d\n", res->lockname.len);
	//mlog(0, "res->lockname.name=%p\n", res->lockname.name);
	//mlog(0, "shuffle res %.*s\n", res->lockname.len,
	//	  res->lockname.name);

	/* because this function is called with the lockres
	 * spinlock, and because we know that it is not migrating/
	 * recovering/in-progress, it is fine to reserve asts and
	 * basts right before queueing them all throughout */
	assert_spin_locked(&res->spinlock);
	BUG_ON((res->state & (DLM_LOCK_RES_MIGRATING|
			      DLM_LOCK_RES_RECOVERING|
			      DLM_LOCK_RES_IN_PROGRESS)));

converting:
	if (list_empty(&res->converting))
		goto blocked;
	mlog(0, "res %.*s has locks on a convert queue\n", res->lockname.len,
	     res->lockname.name);

	target = list_entry(res->converting.next, struct dlm_lock, list);
	if (target->ml.convert_type == LKM_IVMODE) {
		mlog(ML_ERROR, "%.*s: converting a lock with no "
		     "convert_type!\n", res->lockname.len, res->lockname.name);
		BUG();
	}
	head = &res->granted;
	list_for_each(iter, head) {
		lock = list_entry(iter, struct dlm_lock, list);
		if (lock==target)
			continue;
		if (!dlm_lock_compatible(lock->ml.type,
					 target->ml.convert_type)) {
			can_grant = 0;
			/* queue the BAST if not already */
			if (lock->ml.highest_blocked == LKM_IVMODE) {
				__dlm_lockres_reserve_ast(res);
				dlm_queue_bast(dlm, lock);
			}
			/* update the highest_blocked if needed */
			if (lock->ml.highest_blocked < target->ml.convert_type)
				lock->ml.highest_blocked =
					target->ml.convert_type;
		}
	}
	head = &res->converting;
	list_for_each(iter, head) {
		lock = list_entry(iter, struct dlm_lock, list);
		if (lock==target)
			continue;
		if (!dlm_lock_compatible(lock->ml.type,
					 target->ml.convert_type)) {
			can_grant = 0;
			if (lock->ml.highest_blocked == LKM_IVMODE) {
				__dlm_lockres_reserve_ast(res);
				dlm_queue_bast(dlm, lock);
			}
			if (lock->ml.highest_blocked < target->ml.convert_type)
				lock->ml.highest_blocked =
					target->ml.convert_type;
		}
	}

	/* we can convert the lock */
	if (can_grant) {
		spin_lock(&target->spinlock);
		BUG_ON(target->ml.highest_blocked != LKM_IVMODE);

		mlog(0, "calling ast for converting lock: %.*s, have: %d, "
		     "granting: %d, node: %u\n", res->lockname.len,
		     res->lockname.name, target->ml.type,
		     target->ml.convert_type, target->ml.node);

		target->ml.type = target->ml.convert_type;
		target->ml.convert_type = LKM_IVMODE;
		list_move_tail(&target->list, &res->granted);

		BUG_ON(!target->lksb);
		target->lksb->status = DLM_NORMAL;

		spin_unlock(&target->spinlock);

		__dlm_lockres_reserve_ast(res);
		dlm_queue_ast(dlm, target);
		/* go back and check for more */
		goto converting;
	}

blocked:
	if (list_empty(&res->blocked))
		goto leave;
	target = list_entry(res->blocked.next, struct dlm_lock, list);

	head = &res->granted;
	list_for_each(iter, head) {
		lock = list_entry(iter, struct dlm_lock, list);
		if (lock==target)
			continue;
		if (!dlm_lock_compatible(lock->ml.type, target->ml.type)) {
			can_grant = 0;
			if (lock->ml.highest_blocked == LKM_IVMODE) {
				__dlm_lockres_reserve_ast(res);
				dlm_queue_bast(dlm, lock);
			}
			if (lock->ml.highest_blocked < target->ml.type)
				lock->ml.highest_blocked = target->ml.type;
		}
	}

	head = &res->converting;
	list_for_each(iter, head) {
		lock = list_entry(iter, struct dlm_lock, list);
		if (lock==target)
			continue;
		if (!dlm_lock_compatible(lock->ml.type, target->ml.type)) {
			can_grant = 0;
			if (lock->ml.highest_blocked == LKM_IVMODE) {
				__dlm_lockres_reserve_ast(res);
				dlm_queue_bast(dlm, lock);
			}
			if (lock->ml.highest_blocked < target->ml.type)
				lock->ml.highest_blocked = target->ml.type;
		}
	}

	/* we can grant the blocked lock (only
	 * possible if converting list empty) */
	if (can_grant) {
		spin_lock(&target->spinlock);
		BUG_ON(target->ml.highest_blocked != LKM_IVMODE);

		mlog(0, "calling ast for blocked lock: %.*s, granting: %d, "
		     "node: %u\n", res->lockname.len, res->lockname.name,
		     target->ml.type, target->ml.node);

		// target->ml.type is already correct
		list_move_tail(&target->list, &res->granted);

		BUG_ON(!target->lksb);
		target->lksb->status = DLM_NORMAL;

		spin_unlock(&target->spinlock);

		__dlm_lockres_reserve_ast(res);
		dlm_queue_ast(dlm, target);
		/* go back and check for more */
		goto converting;
	}

leave:
	return;
}

/* must have NO locks when calling this with res !=NULL * */
void dlm_kick_thread(struct dlm_ctxt *dlm, struct dlm_lock_resource *res)
{
	mlog_entry("dlm=%p, res=%p\n", dlm, res);
	if (res) {
		spin_lock(&dlm->spinlock);
		spin_lock(&res->spinlock);
		__dlm_dirty_lockres(dlm, res);
		spin_unlock(&res->spinlock);
		spin_unlock(&dlm->spinlock);
	}
	wake_up(&dlm->dlm_thread_wq);
}

void __dlm_dirty_lockres(struct dlm_ctxt *dlm, struct dlm_lock_resource *res)
{
	mlog_entry("dlm=%p, res=%p\n", dlm, res);

	assert_spin_locked(&dlm->spinlock);
	assert_spin_locked(&res->spinlock);

	/* don't shuffle secondary queues */
	if ((res->owner == dlm->node_num)) {
		if (res->state & (DLM_LOCK_RES_MIGRATING |
				  DLM_LOCK_RES_BLOCK_DIRTY))
		    return;

		if (list_empty(&res->dirty)) {
			/* ref for dirty_list */
			dlm_lockres_get(res);
			list_add_tail(&res->dirty, &dlm->dirty_list);
			res->state |= DLM_LOCK_RES_DIRTY;
		}
	}
}


/* Launch the NM thread for the mounted volume */
int dlm_launch_thread(struct dlm_ctxt *dlm)
{
	mlog(0, "starting dlm thread...\n");

	dlm->dlm_thread_task = kthread_run(dlm_thread, dlm, "dlm_thread");
	if (IS_ERR(dlm->dlm_thread_task)) {
		mlog_errno(PTR_ERR(dlm->dlm_thread_task));
		dlm->dlm_thread_task = NULL;
		return -EINVAL;
	}

	return 0;
}

void dlm_complete_thread(struct dlm_ctxt *dlm)
{
	if (dlm->dlm_thread_task) {
		mlog(ML_KTHREAD, "waiting for dlm thread to exit\n");
		kthread_stop(dlm->dlm_thread_task);
		dlm->dlm_thread_task = NULL;
	}
}

static int dlm_dirty_list_empty(struct dlm_ctxt *dlm)
{
	int empty;

	spin_lock(&dlm->spinlock);
	empty = list_empty(&dlm->dirty_list);
	spin_unlock(&dlm->spinlock);

	return empty;
}

static void dlm_flush_asts(struct dlm_ctxt *dlm)
{
	int ret;
	struct dlm_lock *lock;
	struct dlm_lock_resource *res;
	u8 hi;

	spin_lock(&dlm->ast_lock);
	while (!list_empty(&dlm->pending_asts)) {
		lock = list_entry(dlm->pending_asts.next,
				  struct dlm_lock, ast_list);
		/* get an extra ref on lock */
		dlm_lock_get(lock);
		res = lock->lockres;
		mlog(0, "delivering an ast for this lockres\n");

		BUG_ON(!lock->ast_pending);

		/* remove from list (including ref) */
		list_del_init(&lock->ast_list);
		dlm_lock_put(lock);
		spin_unlock(&dlm->ast_lock);

		if (lock->ml.node != dlm->node_num) {
			ret = dlm_do_remote_ast(dlm, res, lock);
			if (ret < 0)
				mlog_errno(ret);
		} else
			dlm_do_local_ast(dlm, res, lock);

		spin_lock(&dlm->ast_lock);

		/* possible that another ast was queued while
		 * we were delivering the last one */
		if (!list_empty(&lock->ast_list)) {
			mlog(0, "aha another ast got queued while "
			     "we were finishing the last one.  will "
			     "keep the ast_pending flag set.\n");
		} else
			lock->ast_pending = 0;

		/* drop the extra ref.
		 * this may drop it completely. */
		dlm_lock_put(lock);
		dlm_lockres_release_ast(dlm, res);
	}

	while (!list_empty(&dlm->pending_basts)) {
		lock = list_entry(dlm->pending_basts.next,
				  struct dlm_lock, bast_list);
		/* get an extra ref on lock */
		dlm_lock_get(lock);
		res = lock->lockres;

		BUG_ON(!lock->bast_pending);

		/* get the highest blocked lock, and reset */
		spin_lock(&lock->spinlock);
		BUG_ON(lock->ml.highest_blocked <= LKM_IVMODE);
		hi = lock->ml.highest_blocked;
		lock->ml.highest_blocked = LKM_IVMODE;
		spin_unlock(&lock->spinlock);

		/* remove from list (including ref) */
		list_del_init(&lock->bast_list);
		dlm_lock_put(lock);
		spin_unlock(&dlm->ast_lock);

		mlog(0, "delivering a bast for this lockres "
		     "(blocked = %d\n", hi);

		if (lock->ml.node != dlm->node_num) {
			ret = dlm_send_proxy_bast(dlm, res, lock, hi);
			if (ret < 0)
				mlog_errno(ret);
		} else
			dlm_do_local_bast(dlm, res, lock, hi);

		spin_lock(&dlm->ast_lock);

		/* possible that another bast was queued while
		 * we were delivering the last one */
		if (!list_empty(&lock->bast_list)) {
			mlog(0, "aha another bast got queued while "
			     "we were finishing the last one.  will "
			     "keep the bast_pending flag set.\n");
		} else
			lock->bast_pending = 0;

		/* drop the extra ref.
		 * this may drop it completely. */
		dlm_lock_put(lock);
		dlm_lockres_release_ast(dlm, res);
	}
	wake_up(&dlm->ast_wq);
	spin_unlock(&dlm->ast_lock);
}


#define DLM_THREAD_TIMEOUT_MS (4 * 1000)
#define DLM_THREAD_MAX_DIRTY  100
#define DLM_THREAD_MAX_ASTS   10

static int dlm_thread(void *data)
{
	struct dlm_lock_resource *res;
	struct dlm_ctxt *dlm = data;
	unsigned long timeout = msecs_to_jiffies(DLM_THREAD_TIMEOUT_MS);

	mlog(0, "dlm thread running for %s...\n", dlm->name);

	while (!kthread_should_stop()) {
		int n = DLM_THREAD_MAX_DIRTY;

		/* dlm_shutting_down is very point-in-time, but that
		 * doesn't matter as we'll just loop back around if we
		 * get false on the leading edge of a state
		 * transition. */
		dlm_run_purge_list(dlm, dlm_shutting_down(dlm));

		/* We really don't want to hold dlm->spinlock while
		 * calling dlm_shuffle_lists on each lockres that
		 * needs to have its queues adjusted and AST/BASTs
		 * run.  So let's pull each entry off the dirty_list
		 * and drop dlm->spinlock ASAP.  Once off the list,
		 * res->spinlock needs to be taken again to protect
		 * the queues while calling dlm_shuffle_lists.  */
		spin_lock(&dlm->spinlock);
		while (!list_empty(&dlm->dirty_list)) {
			int delay = 0;
			res = list_entry(dlm->dirty_list.next,
					 struct dlm_lock_resource, dirty);

			/* peel a lockres off, remove it from the list,
			 * unset the dirty flag and drop the dlm lock */
			BUG_ON(!res);
			dlm_lockres_get(res);

			spin_lock(&res->spinlock);
			/* We clear the DLM_LOCK_RES_DIRTY state once we shuffle lists below */
			list_del_init(&res->dirty);
			spin_unlock(&res->spinlock);
			spin_unlock(&dlm->spinlock);
			/* Drop dirty_list ref */
			dlm_lockres_put(res);

		 	/* lockres can be re-dirtied/re-added to the
			 * dirty_list in this gap, but that is ok */

			spin_lock(&res->spinlock);
			if (res->owner != dlm->node_num) {
				__dlm_print_one_lock_resource(res);
				mlog(ML_ERROR, "inprog:%s, mig:%s, reco:%s, dirty:%s\n",
				     res->state & DLM_LOCK_RES_IN_PROGRESS ? "yes" : "no",
				     res->state & DLM_LOCK_RES_MIGRATING ? "yes" : "no",
				     res->state & DLM_LOCK_RES_RECOVERING ? "yes" : "no",
				     res->state & DLM_LOCK_RES_DIRTY ? "yes" : "no");
			}
			BUG_ON(res->owner != dlm->node_num);

			/* it is now ok to move lockreses in these states
			 * to the dirty list, assuming that they will only be
			 * dirty for a short while. */
			BUG_ON(res->state & DLM_LOCK_RES_MIGRATING);
			if (res->state & (DLM_LOCK_RES_IN_PROGRESS |
					  DLM_LOCK_RES_RECOVERING)) {
				/* move it to the tail and keep going */
				res->state &= ~DLM_LOCK_RES_DIRTY;
				spin_unlock(&res->spinlock);
				mlog(0, "delaying list shuffling for in-"
				     "progress lockres %.*s, state=%d\n",
				     res->lockname.len, res->lockname.name,
				     res->state);
				delay = 1;
				goto in_progress;
			}

			/* at this point the lockres is not migrating/
			 * recovering/in-progress.  we have the lockres
			 * spinlock and do NOT have the dlm lock.
			 * safe to reserve/queue asts and run the lists. */

			mlog(0, "calling dlm_shuffle_lists with dlm=%s, "
			     "res=%.*s\n", dlm->name,
			     res->lockname.len, res->lockname.name);

			/* called while holding lockres lock */
			dlm_shuffle_lists(dlm, res);
			res->state &= ~DLM_LOCK_RES_DIRTY;
			spin_unlock(&res->spinlock);

			dlm_lockres_calc_usage(dlm, res);

in_progress:

			spin_lock(&dlm->spinlock);
			/* if the lock was in-progress, stick
			 * it on the back of the list */
			if (delay) {
				spin_lock(&res->spinlock);
				__dlm_dirty_lockres(dlm, res);
				spin_unlock(&res->spinlock);
			}
			dlm_lockres_put(res);

			/* unlikely, but we may need to give time to
			 * other tasks */
			if (!--n) {
				mlog(0, "throttling dlm_thread\n");
				break;
			}
		}

		spin_unlock(&dlm->spinlock);
		dlm_flush_asts(dlm);

		/* yield and continue right away if there is more work to do */
		if (!n) {
			cond_resched();
			continue;
		}

		wait_event_interruptible_timeout(dlm->dlm_thread_wq,
						 !dlm_dirty_list_empty(dlm) ||
						 kthread_should_stop(),
						 timeout);
	}

	mlog(0, "quitting DLM thread\n");
	return 0;
}
