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

duet_hook_t *duet_hook_fp = NULL;
EXPORT_SYMBOL(duet_hook_fp);

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

	duet_dbg(KERN_INFO "duet_fetch: sending (uuid%llu, ino%lu, idx%lu, %x)\n",
		items[idx].uuid, DUET_UUID_INO(items[idx].uuid), items[idx].idx,
		items[idx].state);

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

static int process_dir_inode(struct duet_task *task, struct inode *inode,
	int was_removed)
{
	struct radix_tree_iter iter;
	void **slot;
	__u16 state;
	unsigned long long uuid = DUET_GET_UUID(inode);

	/* If the file is done, don't bother sending events. We probably didn't
	 * receive any to begin with */
	if (bittree_check_done_bit(&task->bittree, uuid, 1))
		return 0;

	/* Go through all pages of this inode */
	rcu_read_lock();
	radix_tree_for_each_slot(slot, &inode->i_mapping->page_tree, &iter, 0) {
		struct page *page = radix_tree_deref_slot(slot);
		if (unlikely(!page))
			continue;

		state = was_removed ? DUET_PAGE_REMOVED : DUET_PAGE_ADDED;
		hash_add(task, uuid, page->index, state, 1);
	}
	rcu_read_unlock();

	return 0;
}

/*
 * Scan through the page cache for inodes falling under a given directory.
 * Used when a directory is moved inside/outside the task's scope and we need
 * to generate Added/Removed (depending on was_removed value) events to keep
 * task updated.
 */
static int scan_cached_dir(struct duet_task *task, struct inode *dir_inode,
	int was_removed)
{
	unsigned int loop;
	struct hlist_head *head;
	struct inode *inode = NULL;
	struct duet_bittree inodetree;
	struct dentry *tmp, *dir_dentry = NULL;

	bittree_init(&inodetree, 1, 0);

	/* No hard links allowed for dirs, so just grab first dentry */
	if (!hlist_empty(&dir_inode->i_dentry)) {
		hlist_for_each_entry(tmp, &dir_inode->i_dentry, d_alias) {
			if (!(IS_ROOT(tmp) && (tmp->d_flags & DCACHE_DISCONNECTED))) {
				dir_dentry = tmp;
				break;
			}
		}
	}

	if (!dir_dentry)
		printk(KERN_INFO "duet: dir cache scan failed\n");
	printk(KERN_INFO "duet: dir cache scan started\n");

	loop = 0;
again:
	for (; loop < (1U << *duet_i_hash_shift); loop++) {
		head = *duet_inode_hashtable + loop;
		spin_lock(duet_inode_hash_lock);

		/* Process this hash bucket */
		hlist_for_each_entry(inode, head, i_hash) {
			if (inode->i_sb != task->f_sb)
				continue;

			/* If we haven't seen this inode before, and it's a
			 * descendant of the dir we're moving, process it. */
			if (bittree_check(&inodetree, DUET_GET_UUID(inode), 1, NULL) != 1) {
				spin_lock(&inode->i_lock);
				if (inode->i_state & DUET_INODE_FREEING) {
					unsigned long long uuid = DUET_GET_UUID(inode);
					spin_unlock(&inode->i_lock);
					spin_unlock(duet_inode_hash_lock);
					bittree_set_done(&inodetree, uuid, 1);
				} else {
					__iget(inode);
					spin_unlock(&inode->i_lock);
					spin_unlock(duet_inode_hash_lock);

					/* Check that the inode falls under our dir */
					if (!d_find_path(inode, dir_dentry, 0, NULL, 0, NULL))
						process_dir_inode(task, inode, was_removed);
					bittree_set_done(&inodetree, DUET_GET_UUID(inode), 1);
					iput(inode);
				}
				goto again;
			}
		}

		spin_unlock(duet_inode_hash_lock);
	}

	printk(KERN_INFO "duet: dir cache scan finished\n");
	bittree_destroy(&inodetree);

	return 0;
}

/* Handle an event. We're in RCU context so whatever happens, stay awake! */
void duet_hook(__u16 evtcode, void *data)
{
	struct page *page = NULL;
	struct inode *inode = NULL;
	struct duet_move_data *mdata = NULL;
	struct duet_task *cur;
	unsigned long page_idx = 0;
	unsigned long long uuid = 0;
	int p_old, p_new;

	/* Duet must be online */
	if (!duet_online())
		return;

	/* Populate the necessary data structures according to event type */
	if (evtcode & DUET_IN_EVENTS) {
		/* Handle file event */
		switch (evtcode) {
			case DUET_IN_DELETE:
				inode = (struct inode *)data;
				break;
			case DUET_IN_MOVED:
				mdata = (struct duet_move_data *)data;
				inode = mdata->target;
				break;
			default:
				duet_dbg(KERN_INFO "duet: event code %x not supported\n",
					evtcode);
				return;
		}
	} else {
		/* Handle page event */
		page = (struct page *)data;

		/* Duet must be online, and the page must belong to a valid mapping */
		if (!page || !page_mapping(page)) {
			duet_dbg(KERN_ERR "duet: dropped event %x due to NULL mapping\n",
				evtcode);
			return;
		}

		inode = page_mapping(page)->host;
		page_idx = page->index;
	}

	/* Check that we're referring to an actual inode and get its UUID */
	if (!inode)
		return;

	uuid = DUET_GET_UUID(inode);

	/* Verify that the inode does not belong to a special file */
	if (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode)) {
		//duet_dbg(KERN_INFO "duet: event not on regular file\n");
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
			//duet_dbg(KERN_INFO "duet: event sb not matching\n");
			continue;
		}

		duet_dbg(KERN_INFO "duet: received event %x on (uuid %llu, inode %lu, "
				"offt %lu)\n", evtcode, uuid, inode->i_ino, page_idx);

		/* Handle some file task specific events */
		if (cur->is_file) {
			switch (evtcode) {
			case DUET_IN_DELETE:
				/* Reset state for this inode */
				bittree_clear_bits(&cur->bittree, uuid, 1);
				continue;
			case DUET_IN_MOVED:
				/* Case 1: Sanity checking */
				if (!(mdata->old_dir) || !(mdata->new_dir))
					continue;

				/* Case 2: Same parent directory */
				if (mdata->old_dir == mdata->new_dir)
					continue;

				/* Check whether old and new parents are in task scope */
				p_old = do_find_path(cur, mdata->old_dir, 0, NULL);
				p_new = do_find_path(cur, mdata->new_dir, 0, NULL);
				if (p_old == -1 || p_new == -1) {
					printk(KERN_ERR "duet: can't determine parent dir relevance\n");
					continue;
				}

				/*
				 * Case 3: Move constrained outside/inside task scope?
				 * Nothing to do.
				 */

				/* Case 4: Item was moved outside task scope */
				if (!p_old && p_new) {
					if (!S_ISDIR(inode->i_mode)) {
						/* Item is a file. Unmark relevant bit */
						bittree_unset_relv(&cur->bittree, uuid, 1);
						process_dir_inode(cur, inode, 1);
					} else {
						/* Item is a dir. Clear seen, relevant bitmaps */
						bittree_clear_bitmap(&cur->bittree,
							BMAP_SEEN | BMAP_RELV);
						scan_cached_dir(cur, inode, 1);
					}
				}

				/* Case 5: Item was moved inside task scope */
				if (p_old && !p_new) {
					if (!S_ISDIR(inode->i_mode)) {
						/* Item is a file. Mark the relevant bit */
						bittree_set_relv(&cur->bittree, uuid, 1);
						process_dir_inode(cur, inode, 0);
					} else {
						/* Item is a dir. Clear seen bitmap only */
						bittree_clear_bitmap(&cur->bittree, BMAP_SEEN);
						scan_cached_dir(cur, inode, 0);
					}
				}
				continue;
			}

			/* Use the inode bitmap to filter out event if applicable */
			if (bittree_check_inode(&cur->bittree, cur, inode) == 1)
				continue;
		}

		/* Update the hash table */
		if (hash_add(cur, uuid, page_idx, evtcode, 0))
			printk(KERN_ERR "duet: hash table add failed\n");
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(duet_hook);
