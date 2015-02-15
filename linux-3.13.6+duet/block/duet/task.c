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

/*
 * Looks through the bmaptree for the node that should be responsible for
 * the given LBN. Starting from that node, it marks all LBNs until lbn+len
 * (or unmarks them depending on the value of set, or just checks whether
 * they are set or not based on the value of chk), spilling over to
 * subsequent nodes and inserting them if needed.
 * If chk is set then:
 * - a return value of 1 means all relevant LBNs are marked as expected
 * - a return value of 0 means some of the LBNs were not marked as expected
 * - a return value of -1 denotes the occurrence of an error
 * If chk is not set then:
 * - a return value of 0 means all LBNs were marked properly
 * - a return value of -1 denotes the occurrence of an error
 */
static int bittree_chkupd(struct duet_task *task, __u64 lbn, __u32 len,
	__u8 set, __u8 chk)
{
	int ret, found;
	__u64 cur_lbn, node_lbn, lbn_gran, cur_len, rlbn, div_rem;
	__u32 i, rem_len;
	struct rb_node **link, *parent;
	struct bmap_rbnode *bnode = NULL;

	rlbn = lbn;

	duet_dbg(KERN_INFO "duet: task #%d %s%s: lbn%llu, len%llu\n",
		task->id, chk ? "checking if " : "marking as ",
		set ? "set" : "cleared", rlbn, len);

	cur_lbn = rlbn;
	rem_len = len;
	lbn_gran = task->bitrange * task->bmapsize * 8;
	div64_u64_rem(rlbn, lbn_gran, &div_rem);
	node_lbn = rlbn - div_rem;

	/*
	 * Obtain RBBT lock. This will slow us down, but only the work queue
	 * items and the maintenance threads should get here, so the foreground
	 * workload should not be affected. Additionally, only a quarter of the
	 * code will be executed at any call (depending on the set, chk, found
	 * flags), and only the calls that are required to add/remove nodes to
	 * the tree will be costly
	 */
	spin_lock_irq(&task->bittree_lock);

	while (rem_len) {
		/* Look up node_lbn */
		found = 0;
		link = &task->bittree.rb_node;
		parent = NULL;

		while (*link) {
			parent = *link;
			bnode = rb_entry(parent, struct bmap_rbnode, node);

			if (bnode->idx > node_lbn) {
				link = &(*link)->rb_left;
			} else if (bnode->idx < node_lbn) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}

		duet_dbg(KERN_DEBUG "duet: node with LBN %llu %sfound\n",
			node_lbn, found ? "" : "not ");

		/*
		 * Take appropriate action based on whether we found the node
		 * (found), and whether we plan to mark or unmark (set), and
		 * whether we are only checking the values (chk).
		 *
		 *   !Chk  |       Found            !Found      |
		 *  -------+------------------------------------+
		 *    Set  |     Set Bits     |  Init new node  |
		 *         |------------------+-----------------|
		 *   !Set  | Clear (dispose?) |     Nothing     |
		 *  -------+------------------------------------+
		 *
		 *    Chk  |       Found            !Found      |
		 *  -------+------------------------------------+
		 *    Set  |    Check Bits    |  Return false   |
		 *         |------------------+-----------------|
		 *   !Set  |    Check Bits    |    Continue     |
		 *  -------+------------------------------------+
		 */

		/* Find the last LBN on this node */
		cur_len = min(cur_lbn + rem_len,
			node_lbn + lbn_gran) - cur_lbn;

		if (set) {
			if (!found && !chk) {
				/* Insert the new node */
				bnode = bnode_init(task, node_lbn);
				if (!bnode) {
					ret = -1;
					goto done;
				}

				rb_link_node(&bnode->node, parent, link);
				rb_insert_color(&bnode->node, &task->bittree);
			} else if (!found && chk) {
				/* Looking for set bits, node didn't exist */
				ret = 0;
				goto done;
			}

			/* Set the bits */
			if (!chk && duet_bmap_set(bnode->bmap, task->bmapsize,
			    bnode->idx, task->bitrange, cur_lbn, cur_len, 1)) {
				/* We got -1, something went wrong */
				ret = -1;
				goto done;
			/* Check the bits */
			} else if (chk) {
				ret = duet_bmap_chk(bnode->bmap, task->bmapsize,
					bnode->idx, task->bitrange, cur_lbn,
					cur_len, 1);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

		} else if (found) {
			/* Clear the bits */
			if (!chk && duet_bmap_set(bnode->bmap, task->bmapsize,
			    bnode->idx, task->bitrange, cur_lbn, cur_len, 0)) {
				/* We got -1, something went wrong */
				ret = -1;
				goto done;
			/* Check the bits */
			} else if (chk) {
				ret = duet_bmap_chk(bnode->bmap, task->bmapsize,
					bnode->idx, task->bitrange, cur_lbn,
					cur_len, 0);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

			if (!chk) {
				/* Dispose of the node? */
				ret = 0;
				for (i=0; i<task->bmapsize; i++) {
					if (bnode->bmap[i]) {
						ret = 1;
						break;
					}
				}

				if (!ret) {
					bnode_dispose(bnode, parent,
						&task->bittree);
#ifdef CONFIG_DUET_BMAP_STATS
				        (task->stat_bit_cur)--;
#endif /* CONFIG_DUET_BMAP_STATS */
				}
			}
		}

		rem_len -= cur_len;
		cur_lbn += cur_len;
		node_lbn = cur_lbn;
	}

	/* If we managed to get here, then everything worked as planned.
	 * Return 0 for success in the case that chk is not set, or 1 for
	 * success when chk is set. */
	if (!chk)
		ret = 0;
	else
		ret = 1;

done:
	spin_unlock_irq(&task->bittree_lock);
	return ret;
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

	spin_lock_irq(&task->bittree_lock);
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
	spin_unlock_irq(&task->bittree_lock);

	return 0;
}

static int duet_mark_chkupd(__u8 taskid, __u64 idx, __u32 num, __u8 set,
	__u8 chk)
{
	int ret = 0;
	struct duet_task *task;

	task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	ret = bittree_chkupd(task, idx, num, set, chk);
	if (ret == -1)
		printk(KERN_ERR "duet: blocks were not %s %s for task %d\n",
			chk ? "found" : "marked", set ? "set" : "unset",
			task->id);

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return ret;
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
	if (!duet_online())
		return -1;	

	return duet_mark_chkupd(taskid, idx, num, 1, 1);
}
EXPORT_SYMBOL_GPL(duet_check);

/* Unmarks the blocks in the range from idx to idx+num as pending */
int duet_unmark(__u8 taskid, __u64 idx, __u32 num)
{
	if (!duet_online())
		return -1;
	return duet_mark_chkupd(taskid, idx, num, 0, 0);
}
EXPORT_SYMBOL_GPL(duet_unmark);

/* Marks the blocks in the range from idx to idx+num as done */
int duet_mark(__u8 taskid, __u64 idx, __u32 num)
{
	if (!duet_online())
		return -1;
	return duet_mark_chkupd(taskid, idx, num, 1, 0);
}
EXPORT_SYMBOL_GPL(duet_mark);

/* Properly allocate and initialize a task struct */
static int duet_task_init(struct duet_task **task, const char *name,
	__u8 nmodel, __u32 bitrange, void *owner)
{
	*task = kzalloc(sizeof(**task), GFP_NOFS);
	if (!(*task))
		return -ENOMEM;

	(*task)->id = 1;
	memcpy((*task)->name, name, MAX_NAME);

	atomic_set(&(*task)->refcount, 0);

	spin_lock_init(&(*task)->itm_lock);
	//spin_lock_init(&(*task)->itm_outer_lock);
	spin_lock_init(&(*task)->bittree_lock);

	INIT_LIST_HEAD(&(*task)->task_list);
	init_waitqueue_head(&(*task)->cleaner_queue);
	(*task)->bittree = RB_ROOT;
	(*task)->itmtree = RB_ROOT;

	/* We no longer expose this at registration. Fixed to 32KB. */
	(*task)->bmapsize = 32768;

	(*task)->nmodel = nmodel;
	(*task)->sb = (struct super_block *)owner;
	if (bitrange)
		(*task)->bitrange = bitrange;
	else
		(*task)->bitrange = 4096;

	return 0;
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
		bnode_dispose(bnode, rbnode, &task->bittree);
	}

	/* Dispose of the ItemTree */
	while (!RB_EMPTY_ROOT(&task->itmtree)) {
		rbnode = rb_first(&task->itmtree);
		tnode = rb_entry(rbnode, struct item_rbnode, node);
		tnode_dispose(tnode, rbnode, &task->itmtree);
	}

	kfree(task);
}

int duet_register(__u8 *taskid, const char *name, __u8 nmodel, __u32 bitrange,
	void *owner)
{
	int ret;
	struct list_head *last;
	struct duet_task *cur, *task = NULL;

	if (strnlen(name, MAX_NAME) == MAX_NAME) {
		printk(KERN_ERR "duet: error -- task name too long\n");
		return -EINVAL;
	}

	ret = duet_task_init(&task, name, nmodel, bitrange, owner);
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
