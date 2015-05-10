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
 * The framework implements two event models defining how we update the page
 * state when a new event happens. The first model allows subscription to
 * PAGE_EXISTS and PAGE_MODIFIED events, which report whether the existence or
 * modification state of the page has **changed** since the last time the task
 * was told about it.
 * The second model is simpler. It just report an OR'ed mask of all the event
 * codes: PAGE_ADDED, PAGE_DIRTY, PAGE_REMOVED, PAGE_FLUSHED that occurred since
 * the last time the page was told.
 * Pages are put in a red-black tree, so that we can find them in O(logn) time.
 * Indexing is based on inode number (good enough when we look at one filesystem
 * at a time), and the index of the page within said inode.
 */

/*
 * Fetches up to itreq items from the ItemTable. The number of items fetched is
 * given by itret. Items are checked against the bitmap, and discarded if they
 * have been marked; this is possible because an insertion could have happened
 * between the last fetch and the last mark.
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

static void __handle_event(struct work_struct *work)
{
	struct evtwork *ework = (struct evtwork *)work;
	struct duet_task *cur;
	int ret;

	/* Look for tasks interested in this event type and invoke callbacks */
	mutex_lock(&duet_env.task_list_mutex);
	list_for_each_entry(cur, &duet_env.tasks, task_list) {
		/* Verify that the event refers to the fs we're interested in */
		if (cur->f_sb && cur->f_sb != ework->isb) {
			duet_dbg(KERN_INFO "duet: event sb not matching\n");
			continue;
		}

		/* Use the inode bitmap to filter this event out, if needed */
		if (cur->evtmask & DUET_USE_IMAP) {
			mutex_lock(&cur->bittree_lock);
			ret = bittree_check(&cur->bittree, cur->bitrange,
					ework->ino, 1, cur);
			mutex_unlock(&cur->bittree_lock);

			if (ret == 1)
				continue;
		}

		/* Update the hash table */
		if (hash_add(cur, ework->ino, ework->idx, ework->evt, 0))
			printk(KERN_ERR "duet: hash table add failed\n");
	}
	mutex_unlock(&duet_env.task_list_mutex);

	kfree(ework);
}

/* Handle an event. We're in RCU context so whatever happens, stay awake! */
void duet_hook(__u8 evtcode, void *data)
{
	struct page *page = (struct page *)data;
	struct evtwork *ework;
	struct inode *inode;
	unsigned long flags;

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

	/* We're good. Now enqueue a work item. */
	ework = (struct evtwork *)kmalloc(sizeof(struct evtwork), GFP_NOWAIT);
	if (!ework) {
		printk(KERN_ERR "duet: failed to allocate work item\n");
		return;
	}

	/* Populate event work struct */
	INIT_WORK((struct work_struct *)ework, __handle_event);
	ework->ino = inode->i_ino;
	ework->idx = page->index;
	ework->evt = evtcode;
	ework->isb = inode->i_sb;

	spin_lock_irqsave(&duet_env.evtwq_lock, flags);
	if (!duet_env.evtwq ||
	    queue_work(duet_env.evtwq, (struct work_struct *)ework) != 1) {
		printk(KERN_ERR "duet: failed to queue up work\n");
		spin_unlock_irq(&duet_env.evtwq_lock);
		kfree(ework);
		return;
	}
	spin_unlock_irqrestore(&duet_env.evtwq_lock, flags);
}
EXPORT_SYMBOL_GPL(duet_hook);
