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
#include <linux/pagemap.h>
#include "common.h"

/*
 * Synchronization of the task list
 * --------------------------------
 *
 * To synchronize access to the task list and the per-task trees, without
 * compromising scalability, a two-level approach is used. At the task list
 * level, RCU is used. For the task structures, we use traditional reference
 * counting. The two techniques are interweaved to achieve overall consistency.
 *
 *           Updater                     Cleaner
 *           -------                     -------
 * 
 *       rcu_read_lock()               write_lock()
 *           incref                   list_del_rcu()
 *      rcu_read_unlock()             write_unlock()
 *                                  synchronize_rcu()
 *     mutex_lock(rbtree)          wait(refcount == 0)
 *      --make changes--               kfree(task)
 *    mutex_unlock(rbtree)
 *   decref & test == zero
 *     --> wakeup(cleaners)
 *
 * Updaters are found in functions: find_task, bmaptree_set, bmaptree_clear
 * Cleaners are found in functions: duet_shutdown, duet_deregister
 * Insertion functions such as duet_register are fine as is
 */

static int process_inode(struct duet_task *task, struct inode *inode)
{
	struct radix_tree_iter iter;
	void **slot;
	__u8 state;

	/* Go through all pages of this inode */
	rcu_read_lock();
	radix_tree_for_each_slot(slot, &inode->i_mapping->page_tree, &iter, 0) {
		struct page *page;

		page = radix_tree_deref_slot(slot);
		if (unlikely(!page))
			continue;

		spin_lock(&task->itm_lock);
		state = DUET_PAGE_ADDED;
		if (PageDirty(page))
			state |= DUET_PAGE_DIRTY;
		state &= task->evtmask;
		itmtree_insert(task, inode->i_ino, page->index, state, 1);
		spin_unlock(&task->itm_lock);
	}
	rcu_read_unlock();

	return 0;
}

/* Scan through the page cache, and populate the task's tree. */
static int scan_page_cache(struct duet_task *task)
{
	unsigned int loop;
	struct rb_node *rbnode;
	struct hlist_head *head;
	struct bmap_rbnode *bnode;
	struct inode *inode = NULL;
	struct rb_root inodetree = RB_ROOT;

	printk(KERN_INFO "duet: page cache scan started\n");
	loop = 0;
again:
	for (; loop < (1U << *duet_i_hash_shift); loop++) {
		head = *duet_inode_hashtable + loop;
		spin_lock(duet_inode_hash_lock);

		/* Process this hash bucket */
		hlist_for_each_entry(inode, head, i_hash) {
			if (inode->i_sb != task->f_sb)
				continue;

			/* If we haven't seen this inode before, process it. */
			if (bittree_check(&inodetree, 1, 32768, inode->i_ino, 1,
			    NULL) != 1) {
				spin_lock(&inode->i_lock);
				__iget(inode);
				spin_unlock(&inode->i_lock);

				spin_unlock(duet_inode_hash_lock);
				process_inode(task, inode);
				bittree_mark(&inodetree, 1, 32768, inode->i_ino,
						1, NULL);
				iput(inode);
				goto again;
			}
		}

		spin_unlock(duet_inode_hash_lock);
	}

	/* Dispose of the BitTree */
	while (!RB_EMPTY_ROOT(&inodetree)) {
		rbnode = rb_first(&inodetree);
		bnode = rb_entry(rbnode, struct bmap_rbnode, node);
		bnode_dispose(bnode, rbnode, &inodetree, NULL);
	}

	printk(KERN_INFO "duet: page cache scan finished\n");

	return 0;
}

/* Find task and increment its refcount */
struct duet_task *duet_find_task(__u8 taskid)
{
	struct duet_task *cur, *task = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		if (cur->id == taskid) {
			task = cur;
			atomic_inc(&task->refcount);
			break;
		}
	}
	rcu_read_unlock();

	return task;
}

/* TODO */
static int itmtree_print(struct duet_task *task)
{
	printk(KERN_INFO "duet: ItemTree printing not implemented\n");
	return 0;
}

static int bittree_print(struct duet_task *task)
{
	struct bmap_rbnode *bnode = NULL;
	struct rb_node *node;
	__u32 bits_on;

	mutex_lock(&task->bittree_lock);
	printk(KERN_INFO "duet: Printing BitTree for task #%d\n", task->id);
	node = rb_first(&task->bittree);
	while (node) {
		bnode = rb_entry(node, struct bmap_rbnode, node);

		/* Print node information */
		printk(KERN_INFO "duet: Node key = %llu\n", bnode->idx);
		bits_on = duet_bmap_count(bnode->bmap, task->bmapsize);
		printk(KERN_INFO "duet:   Bits set: %u out of %u\n", bits_on,
			task->bmapsize * 8);

		node = rb_next(node);
	}
	mutex_unlock(&task->bittree_lock);

	return 0;
}

/* Do a preorder print of the BitTree */
int duet_print_bittree(__u8 taskid)
{
	struct duet_task *task;

	task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	if (bittree_print(task)) {
		printk(KERN_ERR "duet: failed to print BitTree for task %d\n",
			task->id);
		return -1;
	}

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return 0;
}
EXPORT_SYMBOL_GPL(duet_print_bittree);

/* Do a preorder print of the ItemTree */
int duet_print_itmtree(__u8 taskid)
{
	struct duet_task *task;

	task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	if (itmtree_print(task)) {
		printk(KERN_ERR "duet: failed to print ItemTree for task %d\n",
			task->id);
		return -1;
	}

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return 0;
}
EXPORT_SYMBOL_GPL(duet_print_itmtree);

/* Checks the blocks in the range from idx to idx+num are done */
int duet_check(__u8 taskid, __u64 idx, __u32 num)
{
	int ret = 0;
	struct duet_task *task;

	if (!duet_online())
		return -1;	


	task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	/*
	 * Obtain RBBT lock. This will slow us down, but only the work queue
	 * items and the maintenance threads should get here, so the foreground
	 * workload should not be affected. Additionally, only a quarter of the
	 * code will be executed at any call (depending on the set, chk, found
	 * flags), and only the calls that are required to add/remove nodes to
	 * the tree will be costly
	 */
	mutex_lock(&task->bittree_lock);
	ret = bittree_check(&task->bittree, task->bitrange, task->bmapsize,
		idx, num, task);
	mutex_unlock(&task->bittree_lock);

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return ret;
}
EXPORT_SYMBOL_GPL(duet_check);

/* Unmarks the blocks in the range from idx to idx+num as pending */
int duet_unmark(__u8 taskid, __u64 idx, __u32 num)
{
	int ret = 0;
	struct duet_task *task;

	if (!duet_online())
		return -1;

	task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	/*
	 * Obtain RBBT lock. This will slow us down, but only the work queue
	 * items and the maintenance threads should get here, so the foreground
	 * workload should not be affected. Additionally, only a quarter of the
	 * code will be executed at any call (depending on the set, chk, found
	 * flags), and only the calls that are required to add/remove nodes to
	 * the tree will be costly
	 */
	mutex_lock(&task->bittree_lock);
	ret = bittree_unmark(&task->bittree, task->bitrange, task->bmapsize,
		idx, num, task);
	mutex_unlock(&task->bittree_lock);

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return ret;
}
EXPORT_SYMBOL_GPL(duet_unmark);

/* Marks the blocks in the range from idx to idx+num as done */
int duet_mark(__u8 taskid, __u64 idx, __u32 num)
{
	int ret = 0;
	struct duet_task *task;

	if (!duet_online())
		return -1;

	task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	/*
	 * Obtain RBBT lock. This will slow us down, but only the work queue
	 * items and the maintenance threads should get here, so the foreground
	 * workload should not be affected. Additionally, only a quarter of the
	 * code will be executed at any call (depending on the set, chk, found
	 * flags), and only the calls that are required to add/remove nodes to
	 * the tree will be costly
	 */
	mutex_lock(&task->bittree_lock);
	ret = bittree_mark(&task->bittree, task->bitrange, task->bmapsize,
		idx, num, task);
	mutex_unlock(&task->bittree_lock);

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return ret;
}
EXPORT_SYMBOL_GPL(duet_mark);

/* Properly allocate and initialize a task struct */
static int duet_task_init(struct duet_task **task, const char *name,
	__u8 evtmask, __u32 bitrange, struct super_block *f_sb,
	struct dentry *p_dentry)
{
	*task = kzalloc(sizeof(**task), GFP_NOFS);
	if (!(*task))
		return -ENOMEM;

	(*task)->id = 1;
	memcpy((*task)->name, name, MAX_NAME);

	atomic_set(&(*task)->refcount, 0);

	spin_lock_init(&(*task)->itm_lock);
	mutex_init(&(*task)->bittree_lock);

	INIT_LIST_HEAD(&(*task)->task_list);
	init_waitqueue_head(&(*task)->cleaner_queue);
	(*task)->bittree = RB_ROOT;
	(*task)->itmtree = RB_ROOT;

	/* We no longer expose this at registration. Fixed to 32KB. */
	(*task)->bmapsize = 32768;

	/* Do some sanity checking on event mask */
	if (evtmask & DUET_PAGE_EXISTS) {
		if (evtmask & (DUET_PAGE_ADDED | DUET_PAGE_REMOVED)) {
			printk(KERN_DEBUG "duet: failed to register EXIST events\n");
			goto err;
		}
		evtmask |= (DUET_PAGE_ADDED | DUET_PAGE_REMOVED);
	}

	if (evtmask & DUET_PAGE_MODIFIED) {
		if (evtmask & (DUET_PAGE_DIRTY | DUET_PAGE_FLUSHED)) {
			printk(KERN_DEBUG "duet: failed to register MODIFIED events\n");
			goto err;
		}
		evtmask |= (DUET_PAGE_DIRTY | DUET_PAGE_FLUSHED);
	}

	(*task)->evtmask = evtmask;
	(*task)->f_sb = f_sb;
	(*task)->p_dentry = p_dentry;

	if (bitrange)
		(*task)->bitrange = bitrange;
	else
		(*task)->bitrange = 4096;

	printk(KERN_DEBUG "duet: task registered with evtmask %u", evtmask);
	return 0;
err:
	printk(KERN_ERR "duet: error registering task\n");
	kfree(*task);
	return -EINVAL;
}

/* Properly dismantle and dispose of a task struct.
 * At this point we've guaranteed that noone else is accessing the
 * task struct, so we don't need any locks */
void duet_task_dispose(struct duet_task *task)
{
	struct rb_node *rbnode;
	struct bmap_rbnode *bnode;
	struct item_rbnode *tnode;

	/* Dispose of the BitTree */
	while (!RB_EMPTY_ROOT(&task->bittree)) {
		rbnode = rb_first(&task->bittree);
		bnode = rb_entry(rbnode, struct bmap_rbnode, node);
		bnode_dispose(bnode, rbnode, &task->bittree, NULL);
	}

	/* Dispose of the ItemTree */
	while (!RB_EMPTY_ROOT(&task->itmtree)) {
		rbnode = rb_first(&task->itmtree);
		tnode = rb_entry(rbnode, struct item_rbnode, node);
		tnode_dispose(tnode, rbnode, &task->itmtree);
	}

	if (task->p_dentry)
		dput(task->p_dentry);
	kfree(task);
}

int duet_register(__u8 *taskid, const char *name, __u8 evtmask, __u32 bitrange,
	struct super_block *f_sb, struct dentry *p_dentry)
{
	int ret;
	struct list_head *last;
	struct duet_task *cur, *task = NULL;

	if (strnlen(name, MAX_NAME) == MAX_NAME) {
		printk(KERN_ERR "duet: error -- task name too long\n");
		return -EINVAL;
	}

	ret = duet_task_init(&task, name, evtmask, bitrange, f_sb, p_dentry);
	if (ret) {
		printk(KERN_ERR "duet: failed to initialize task\n");
		return ret;
	}

	/* Find a free task id for the new task.
	 * Tasks are sorted by id, so that we can find the smallest free id in
	 * one traversal (just look for a gap). To do that, we lock it
	 * exclusively; this should be fine just for addition. */
	mutex_lock(&duet_env.task_list_mutex);
	last = &duet_env.tasks;
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		if (cur->id == task->id)
			(task->id)++;
		else if (cur->id > task->id)
			break;

		last = &cur->task_list;
	}
	list_add_rcu(&task->task_list, last);
	mutex_unlock(&duet_env.task_list_mutex);

	/* Now that the task is receiving events, scan the page cache and
	 * populate its ItemTree. */
	scan_page_cache(task);

	*taskid = task->id;
	return 0;
}
EXPORT_SYMBOL_GPL(duet_register);

int duet_deregister(__u8 taskid)
{
	struct duet_task *cur;

	/* Find the task in the list, then dispose of it */
	mutex_lock(&duet_env.task_list_mutex);
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		if (cur->id == taskid) {
			list_del_rcu(&cur->task_list);
			mutex_unlock(&duet_env.task_list_mutex);

			/* Wait until everyone's done with it */
			synchronize_rcu();
			wait_event(cur->cleaner_queue,
				atomic_read(&cur->refcount) == 0);
			duet_task_dispose(cur);

			return 0;
		}
	}
	mutex_unlock(&duet_env.task_list_mutex);

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(duet_deregister);
