/*
 * Copyright (C) 2014-2015 George Amvrosiadis.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
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
 * TODO: Update duet_fetch to match SOSP submission format
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include "common.h"

/*
 * The framework implements two models that define how we update the page state
 * when a new event occurs: the state-based, and the event-based model.
 * Page state is retained in the global hash table.
 *
 * The state-based model allows subscription to PAGE_EXISTS and PAGE_MODIFIED
 * events, which report whether the existence or modification state of the page
 * has **changed** since the last time the task was told about it.
 *
 * The event-based model is simpler. It just reports all the event types that
 * have occurred on a page, since the last time the task was informed. Supported
 * events include: PAGE_ADDED, PAGE_DIRTY, PAGE_REMOVED, and PAGE_FLUSHED.
 */

/*
 * Fetches up to itreq items. The number of items fetched is given by itret.
 * Items are checked against the bitmap, and discarded if they have been marked;
 * this is possible because an insertion could have happened between the last
 * fetch and the last mark.
 */
int duet_fetch(__u8 taskid, __u16 itreq, struct duet_item *items, __u16 *itret)
{
	struct duet_task *task = duet_find_task(taskid);
	if (!task) {
		printk(KERN_ERR "duet_fetch: invalid taskid (%d)\n", taskid);
		return 1;	
	}

	/* We'll either run out of items, or grab itreq items. */
	*itret = 0;

again:
	if (!hash_fetch(task, &items[*itret]))
		goto done;

	duet_dbg(KERN_INFO "duet_fetch: sending (ino%lu, idx%lu, %x)\n",
		items[*itret].ino, items[*itret].idx, items[*itret].state);

	(*itret)++;
	if (*itret < itreq)
		goto again;

done:
	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return 0;
}
EXPORT_SYMBOL_GPL(duet_fetch);

/* Handle an event. We're in RCU context so whatever happens, stay awake! */
void duet_hook(__u8 evtcode, void *data)
{
	struct page *page = (struct page *)data;
	struct inode *inode;
	struct duet_task *cur;
	unsigned long flags;
	int ret;

	/* Duet must be online, and the page must belong to a valid mapping */
	if (!duet_online() || !page || !page_mapping(page) ||
		!page_mapping(page)->host)
		return;

	inode = page_mapping(page)->host;

	/* Verify that the inode does not belong to a special file */
	/* XXX: Is this absolutely necessary? */
	if (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode)) {
		duet_dbg(KERN_INFO "duet: event not on regular file\n");
		return;
	}

	if (!inode->i_ino) {
		printk(KERN_ERR "duet: inode not initialized\n");
		return;
	}

	/* Look for tasks interested in this event type and invoke callbacks */
	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		/* Verify that the event refers to the fs we're interested in */
		if (cur->f_sb && cur->f_sb != inode->i_sb) {
			duet_dbg(KERN_INFO "duet: event sb not matching\n");
			continue;
		}

		/* Use the inode bitmap to filter this event out, if needed */
		if (cur->evtmask & DUET_USE_IMAP) {
			spin_lock_irqsave(&cur->bittree_lock, flags);
			ret = bittree_check(&cur->bittree, cur->bitrange,
					inode->i_ino, 1, cur);
			spin_unlock_irqrestore(&cur->bittree_lock, flags);

			if (ret == 1)
				continue;
		}

		/* Update the hash table */
		local_irq_save(flags);
		if (hash_add(cur, inode->i_ino, page->index, evtcode, 0))
			printk(KERN_ERR "duet: hash table add failed\n");
		local_irq_restore(flags);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(duet_hook);
