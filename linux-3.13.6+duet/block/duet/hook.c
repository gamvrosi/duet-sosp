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
 * Fetches up to itreq items from the ItemTree. The number of items fetched is
 * given by itret. Items are checked against the bitmap, and discarded if they
 * have been marked; this is possible because an insertion could have happened
 * between the last fetch and the last mark.
 */
int duet_fetch(__u8 taskid, __u16 itreq, struct duet_item *items, __u16 *itret)
{
	struct rb_node *rbnode;
	struct item_rbnode *tnode;
	struct duet_task *task = duet_find_task(taskid);

	if (!task) {
		printk(KERN_ERR "duet_fetch: invalid taskid (%d)\n", taskid);
		return 1;	
	}

	/*
	 * We'll either run out of items, or grab itreq items.
	 * We also skip the outer lock. Suck it interrupts.
	 */
	*itret = 0;

again:
	spin_lock_irq(&task->itm_lock);

	/* Grab first item from the tree */
	if (!RB_EMPTY_ROOT(&task->itmtree)) {
		rbnode = rb_first(&task->itmtree);
		tnode = rb_entry(rbnode, struct item_rbnode, node);
		rb_erase(rbnode, &task->itmtree);
		spin_unlock_irq(&task->itm_lock);
	} else {
		spin_unlock_irq(&task->itm_lock);
		goto done;
	}

	/* Copy fields off to items array, if we've subscribed to this item */
	if (task->evtmask & tnode->item->state) {
		items[*itret].ino = tnode->item->ino;
		items[*itret].idx = tnode->item->idx;
		items[*itret].state = tnode->item->state;

		duet_dbg(KERN_INFO "duet_fetch: sending (ino%lu, idx%lu, %x)\n",
			items[*itret].ino, items[*itret].idx,
			items[*itret].state);

		(*itret)++;
	}

	tnode_dispose(tnode, NULL, NULL);
	if (*itret < itreq)
		goto again;

done:
	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return 0;
}
EXPORT_SYMBOL_GPL(duet_fetch);

/*
 * The framework implements an event model that defines how we update the page
 * state when a new event happens. The state of the page is returned by fetch,
 * which returns the page back to the up-to-date state. Note that this model
 * relies on the basic assumption that when the session starts, we know the
 * initial state of each page in the cache (but haven't told the task). We
 * achieve that by scanning the page cache at registration time (check task.c).
 * Although we implement this model for all tasks, different tasks can subscribe
 * to only a subset of the events (ADD, MOD, or REM).
 *
 *  +---------+  fetch,ADD  +------------+     ADD       +-------+   Page
 *  | Page    |------------>| Page       |-------------->| Page  |<- - - - -
 *  | removed |<------------| up-to-date |<--------------| added |  Scanner
 *  +---------+     REM     +------------+  fetch,REM    +-------+
 *       ^                     ^ |      ^                    |
 *       |               fetch | | MOD  | fetch,REM          | MOD
 *       |                     | v      +------+             v
 *       |   REM             +----------+      |      +--------------+   Page
 *       +-------------------| Page     |      +------| Page added   |<- - - - -
 *                           | modified |             | and modified |  Scanner
 *                           +----------+             +--------------+
 *                               ^ |                        ^ |
 *                               | | MOD                    | | MOD
 *                               +-+                        +-+
 *
 * Pages that are not up-to-date are put in a red-black tree, so that we can
 * find them in O(logn) time. Indexing is based on inode number (good enough
 * when we look at one file system at a time), and the index of the page within
 * said inode.
 */
static void duet_handle_event(struct duet_task *task, __u8 evtcode,
	struct page *page)
{
	int found = 0;
	struct rb_node *node = NULL;
	struct item_rbnode *tnode = NULL;
	struct inode *inode;

	BUG_ON(!page_mapping(page));

	inode = page_mapping(page)->host;

	if (!inode) {
		duet_dbg(KERN_INFO "duet: address mapping host is NULL\n");
		return;
	}

	/* Verify that the event refers to the fs we're interested in */
	if (task->sb && task->sb != inode->i_sb) {
		duet_dbg(KERN_INFO "duet: event not on fs of interest\n");
		return;
	}

	/* Verify that the inode does not belong to a special file */
	/* XXX: Is this absolutely necessary? */
	if (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode)) {
		duet_dbg(KERN_INFO "duet: event not on regular file\n");
		return;
	}

	/* First, look up the node in the ItemTree */
	//spin_lock_irq(&task->itm_outer_lock);
	spin_lock_irq(&task->itm_lock);
	node = task->itmtree.rb_node;

	while (node) {
		tnode = rb_entry(node, struct item_rbnode, node);

		/* We order based on (inode, page index) */
		if (tnode->item->ino > inode->i_ino) {
			node = node->rb_left;
		} else if (tnode->item->ino < inode->i_ino) {
			node = node->rb_right;
		} else {
			/* Found inode, look for index */
			if (tnode->item->idx > page->index) {
				node = node->rb_left;
			} else if (tnode->item->idx < page->index) {
				node = node->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	duet_dbg(KERN_DEBUG "duet-page: %s node (ino%lu, idx%lu)\n",
		found ? "found" : "didn't find",
		found ? tnode->item->ino : inode->i_ino,
		found ? tnode->item->idx : page->index);

	/* If we found it, update according to our model. Otherwise, insert. */
	if (!found) {
		/* This is easy. The event code coincides with the state we're
		 * transitioning to. */
		if (itmtree_insert(task, inode->i_ino, page->index, evtcode, 0))
			printk(KERN_ERR "duet: itmtree insert failed\n");
	} else if (found) {
		/* What we do depends on what we found */
		switch (tnode->item->state) {
		case DUET_PAGE_ADDED:
			switch (evtcode) {
			case DUET_EVT_ADD:
				printk(KERN_ERR "We got an A event while at the"
						" A state. This is a bug!\n");
				break;
			case DUET_EVT_MOD:
				tnode->item->state = DUET_PAGE_ADDED_MODIFIED;
				break;
			case DUET_EVT_REM:
				tnode_dispose(tnode, node, &task->itmtree);
				break;
			}
			break;

		case DUET_PAGE_REMOVED:
			switch (evtcode) {
			case DUET_EVT_ADD:
				tnode_dispose(tnode, node, &task->itmtree);
				break;
			case DUET_EVT_MOD:
			case DUET_EVT_REM:
				printk(KERN_ERR "We got an %s event while at "
						"the R state. This is a bug!\n",
					evtcode == DUET_EVT_MOD ? "M" : "R");
				break;
			}
			break;

		case DUET_PAGE_ADDED_MODIFIED:
			switch (evtcode) {
			case DUET_EVT_ADD:
				printk(KERN_ERR "We got an A event while at the"
						" A+M state. This is a bug!\n");
				break;
			case DUET_EVT_MOD:
				/* Nothing changes */
				break;
			case DUET_EVT_REM:
				tnode_dispose(tnode, node, &task->itmtree);
				break;
			}
			break;

		case DUET_PAGE_MODIFIED:
			switch (evtcode) {
			case DUET_EVT_ADD:
				printk(KERN_ERR "We got an A event while at the"
						" M state. This is a bug!\n");
				break;
			case DUET_EVT_MOD:
				/* Nothing changes */
				break;
			case DUET_EVT_REM:
				tnode->item->state = DUET_PAGE_REMOVED;
				break;
			}
			break;
		}
	}

	spin_unlock_irq(&task->itm_lock);
	//spin_unlock_irq(&task->itm_outer_lock);
}

void duet_hook(__u8 evtcode, void *data)
{
	struct page *page = (struct page *)data;
	struct duet_task *cur;

	/* Duet must be online, and the page must belong to a valid mapping */
	if (!duet_online() || !page || !page_mapping(page))
		return;

	/* We're in RCU context so whatever happens, stay awake! */
	//duet_dbg(KERN_INFO "duet hook: evt %u, data %p\n", evtcode, data);

	/* Look for tasks interested in this event type and invoke callbacks */
	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list)
		duet_handle_event(cur, evtcode, page);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(duet_hook);
