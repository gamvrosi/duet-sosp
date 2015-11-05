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
 * Fetches up to itreq items. The number of items fetched is returned (or -1
 * for error). Items are checked against the bitmap, and discarded if they have
 * been marked; this is possible because an insertion could have happened
 * between the last fetch and the last mark.
 */
int duet_fetch(__u8 taskid, struct duet_item *items, __u16 *count)
{
	int idx = 0;
	struct duet_task *task = duet_find_task(taskid);
	if (!task) {
		printk(KERN_ERR "duet_fetch: invalid taskid (%d)\n", taskid);
		return -1;
	}

	/* We'll either run out of items, or grab itreq items. */
again:
	if (hash_fetch(task, &items[idx]))
		goto done;

	duet_dbg(KERN_INFO "duet_fetch: sending (ino%lu, idx%lu, %x)\n",
		items[idx].ino, items[idx].idx, items[idx].state);

	idx++;
	if (idx < *count)
		goto again;

done:
	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	*count = idx;
	return 0;
}
EXPORT_SYMBOL_GPL(duet_fetch);

/* Handle an event. We're in RCU context so whatever happens, stay awake! */
void duet_hook(__u8 evtcode, void *data)
{
	struct page *page = (struct page *)data;
	struct inode *inode;
	struct duet_task *cur;

	/* Duet must be online, and the page must belong to a valid mapping */
	if (!duet_online() || !page || !page_mapping(page) ||
		!page_mapping(page)->host)
		return;

	inode = page_mapping(page)->host;

	/* Verify that the inode does not belong to a special file */
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

		/* For file tasks, use the inode bitmap to filter out event */
		if (cur->is_file && (bittree_check(&cur->bittree, inode->i_ino,
															1, cur) == 1))
			continue;

		/* Update the hash table */
		if (hash_add(cur, inode->i_ino, page->index, evtcode, 0))
			printk(KERN_ERR "duet: hash table add failed\n");
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(duet_hook);
