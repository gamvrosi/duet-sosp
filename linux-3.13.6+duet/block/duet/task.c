/*
 * Copyright (C) 2014 George Amvrosiadis.  All rights reserved.
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
 * To synchronize access to the task list and the per-task RB-trees, without
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
 * Cleaners are found in functions: duet_shutdown, duet_task_deregister
 * Insertion functions such as duet_task_register are fine as is
 */

static struct duet_rbnode *dnode_init(struct duet_task *task, __u64 lbn)
{
	struct duet_rbnode *dnode = NULL;

#ifdef CONFIG_DUET_BMAP_STATS
	(task->curnodes)++;
	if (task->curnodes > task->maxnodes) {
		task->maxnodes = task->curnodes;
		printk(KERN_INFO "duet: Task #%d (%s) has %llu nodes in its RBBT.\n"
			"      That's %llu bytes.\n", task->id, task->name,
			task->maxnodes, task->maxnodes * task->bmapsize);
	}
#endif /* CONFIG_DUET_BMAP_STATS */
	
	dnode = kzalloc(sizeof(*dnode), GFP_NOFS);
	if (!dnode)
		return NULL;

	dnode->bmap = kzalloc(task->bmapsize, GFP_NOFS);
	if (!dnode->bmap) {
		kfree(dnode);
		return NULL;
	}

	RB_CLEAR_NODE(&dnode->node);
	dnode->lbn = lbn;
	return dnode;
}

static void dnode_dispose(struct duet_rbnode *dnode, struct rb_node *rbnode,
	struct rb_root *root)
{
	rb_erase(rbnode, root);
	kfree(dnode->bmap);
	kfree(dnode);
}

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
static int bmaptree_chkupd(struct duet_task *task, __u64 start, __u64 lbn,
			__u32 len, __u8 set, __u8 chk)
{
	int ret, found;
	__u64 cur_lbn, node_lbn, lbn_gran, cur_len, rlbn, div_rem;
	__u32 i, rem_len;
	struct rb_node **link, *parent;
	struct duet_rbnode *dnode = NULL;

	/* The lbn given is relative to the beginning of the partition.
	 * Make it absolute to the beginning of the block device. */
	rlbn = lbn + start;

#ifdef CONFIG_DUET_DEBUG
	printk(KERN_INFO "duet: chkupd on task #%d for range [%llu, %llu] "
		"(set=%u, chk=%u)\n", task->id, rlbn, rlbn+len-1, set, chk);
#endif /* CONFIG_DUET_DEBUG */

	cur_lbn = rlbn;
	rem_len = len;
	lbn_gran = task->blksize * task->bmapsize * 8;
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
	mutex_lock(&task->bmaptree_mutex);

	while (rem_len) {
		/* Look up node_lbn */
		found = 0;
		link = &task->bmaptree.rb_node;
		parent = NULL;

		while (*link) {
			parent = *link;
			dnode = rb_entry(parent, struct duet_rbnode, node);

			if (dnode->lbn > node_lbn) {
				link = &(*link)->rb_left;
			} else if (dnode->lbn < node_lbn) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}

#ifdef CONFIG_DUET_DEBUG
		printk(KERN_DEBUG "duet: node with LBN %llu %sfound\n",
			node_lbn, found ? "" : "not ");
#endif /* CONFIG_DUET_DEBUG */

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
				dnode = dnode_init(task, node_lbn);
				if (!dnode) {
					ret = -1;
					goto done;
				}

				rb_link_node(&dnode->node, parent, link);
				rb_insert_color(&dnode->node, &task->bmaptree);
			} else if (!found && chk) {
				/* Looking for set bits, node didn't exist */
				ret = 0;
				goto done;
			}

			/* Set the bits */
			if (!chk && duet_bmap_set(dnode->bmap, task->bmapsize,
			    dnode->lbn, task->blksize, cur_lbn, cur_len, 1)) {
				/* We got -1, something went wrong */
				ret = -1;
				goto done;
			/* Check the bits */
			} else if (chk) {
				ret = duet_bmap_chk(dnode->bmap, task->bmapsize,
					dnode->lbn, task->blksize, cur_lbn,
					cur_len, 1);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

		} else if (found) {
			/* Clear the bits */
			if (!chk && duet_bmap_set(dnode->bmap, task->bmapsize,
			    dnode->lbn, task->blksize, cur_lbn, cur_len, 0)) {
				/* We got -1, something went wrong */
				ret = -1;
				goto done;
			/* Check the bits */
			} else if (chk) {
				ret = duet_bmap_chk(dnode->bmap, task->bmapsize,
					dnode->lbn, task->blksize, cur_lbn,
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
					if (dnode->bmap[i]) {
						ret = 1;
						break;
					}
				}

				if (!ret) {
					dnode_dispose(dnode, parent,
						&task->bmaptree);
#ifdef CONFIG_DUET_BMAP_STATS
				        (task->curnodes)--;
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
	mutex_unlock(&task->bmaptree_mutex);
	return ret;
}

/* Find task and increment its refcount */
static struct duet_task *find_task(__u8 taskid)
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

#ifdef CONFIG_DUET_BMAP_STATS
static int bmaptree_trim(struct duet_task *task, __u64 end)
{
        struct rb_node *rbnode;
        struct duet_rbnode *dnode;

        while (!RB_EMPTY_ROOT(&task->bmaptree)) {
                rbnode = rb_first(&task->bmaptree);
                dnode = rb_entry(rbnode, struct duet_rbnode, node);

		/* Check whether we can dispose of this node */
		if (dnode->lbn + (task->bmapsize * 8) <= end)
			dnode_dispose(dnode, rbnode, &task->bmaptree);
		else
			break;
        }
	mutex_unlock(&task->bmaptree_mutex);

	return 0;
}
#endif /* CONFIG_DUET_BMAP_STATS */

static int bmaptree_print(struct duet_task *task)
{
	struct duet_rbnode *dnode = NULL;
	struct rb_node *node;
	__u32 bits_on;

	mutex_lock(&task->bmaptree_mutex);
	printk(KERN_INFO "duet: Printing RBBT for task #%d\n", task->id);
	node = rb_first(&task->bmaptree);
	while (node) {
		dnode = rb_entry(node, struct duet_rbnode, node);

		/* Print node information */
		printk(KERN_INFO "duet: Node key = %llu\n", dnode->lbn);
		bits_on = duet_bmap_count(dnode->bmap, task->bmapsize);
		printk(KERN_INFO "duet:   Bits set: %u out of %u\n", bits_on,
			task->bmapsize * 8);

		node = rb_next(node);
	}
	mutex_unlock(&task->bmaptree_mutex);

	return 0;
}

static int duet_mark_chkupd(__u8 taskid, __u64 start, __u64 lbn, __u32 len,
			__u8 set, __u8 chk)
{
	int ret = 0;
	struct duet_task *task;

	task = find_task(taskid);
	if (!task)
		return -ENOENT;

	ret = bmaptree_chkupd(task, start, lbn, len, set, chk);

	if (ret == -1) {
		printk(KERN_ERR "duet: blocks were not %s as %s for task %d\n",
			chk ? "found" : "set", set ? "marked" : "unmarked",
			task->id);
	}

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return ret;
}

#ifdef CONFIG_DUET_BMAP_STATS
/* Trim down unnecessary nodes from the RBBT */
int duet_trim_rbbt(__u8 taskid, __u64 end)
{
	struct duet_task *task;

	task = find_task(taskid);
	if (!task)
		return -ENOENT;

	if (bmaptree_trim(task, end)) {
		printk(KERN_ERR "duet: failed to trim tree for task %d\n",
			task->id);
		return -1;
	}

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return 0;
}
EXPORT_SYMBOL_GPL(duet_trim_rbbt);
#endif /* CONFIG_DUET_BMAP_STATS */

/* Do a preorder print of the red-black bitmap tree */
int duet_print_rbt(__u8 taskid)
{
	struct duet_task *task;

	task = find_task(taskid);
	if (!task)
		return -ENOENT;

	if (bmaptree_print(task)) {
		printk(KERN_ERR "duet: failed to print tree for task %d\n",
			task->id);
		return -1;
	}

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return 0;
}
EXPORT_SYMBOL_GPL(duet_print_rbt);

/* Checks the blocks in the range from lbn to lbn+len are done */
int duet_chk_done(__u8 taskid, __u64 start, __u64 lbn, __u32 len)
{
	if (!duet_is_online())
		return -1;
	return duet_mark_chkupd(taskid, start, lbn, len, 1, 1);
}
EXPORT_SYMBOL_GPL(duet_chk_done);

/* Checks the blocks in the range from lbn to lbn+len as todo */
int duet_chk_todo(__u8 taskid, __u64 start, __u64 lbn, __u32 len)
{
	if (!duet_is_online())
		return -1;
	return duet_mark_chkupd(taskid, start, lbn, len, 0, 1);
}
EXPORT_SYMBOL_GPL(duet_chk_todo);

/* Marks the blocks in the range from lbn to lbn+len as done */
int duet_mark_done(__u8 taskid, __u64 start, __u64 lbn, __u32 len)
{
	if (!duet_is_online())
		return -1;
	return duet_mark_chkupd(taskid, start, lbn, len, 1, 0);
}
EXPORT_SYMBOL_GPL(duet_mark_done);

/* Unmarks the blocks in the range from lbn to lbn+len as done */
int duet_mark_todo(__u8 taskid, __u64 start, __u64 lbn, __u32 len)
{
	if (!duet_is_online())
		return -1;
	return duet_mark_chkupd(taskid, start, lbn, len, 0, 0);
}
EXPORT_SYMBOL_GPL(duet_mark_todo);

/* Disposes of the red-black bitmap tree */
static void bmaptree_dispose(struct rb_root *root)
{
	struct rb_node *rbnode;
	struct duet_rbnode *dnode;

	while (!RB_EMPTY_ROOT(root)) {
		rbnode = rb_first(root);
		dnode = rb_entry(rbnode, struct duet_rbnode, node);
		dnode_dispose(dnode, rbnode, root);
	}
}

static void echo_handler(__u8 taskid, __u8 event_code, void *owner, __u64 offt,
			__u32 len, void *data, int data_type, void *privdata)
{
	printk(KERN_DEBUG "duet: echo_handler called\n"
		"duet: taskid = %u, event_code = %u, lbn = %llu, len = %u, "
		"data_type = %d\n", taskid, event_code, offt, len, data_type);
}

/* Properly allocate and initialize a task struct */
static int duet_task_init(struct duet_task **task, const char *name,
	__u32 blksize, __u32 bmapsize, __u8 event_mask,
	duet_event_handler_t event_handler, void *privdata)
{
	*task = kzalloc(sizeof(**task), GFP_NOFS);
	if (!(*task))
		return -ENOMEM;

	(*task)->id = 1;
	memcpy((*task)->name, name, TASK_NAME_LEN);
	INIT_LIST_HEAD(&(*task)->task_list);
	init_waitqueue_head(&(*task)->cleaner_queue);
	atomic_set(&(*task)->refcount, 0);

	if (blksize)
		(*task)->blksize = blksize;
	else
		(*task)->blksize = 4096;

	if (bmapsize)
		(*task)->bmapsize = bmapsize;
	else
		(*task)->bmapsize = 32768;

	mutex_init(&(*task)->bmaptree_mutex);
	(*task)->bmaptree = RB_ROOT;
	(*task)->privdata = privdata;
	(*task)->event_mask = event_mask;

	if (event_handler)
		(*task)->event_handler = event_handler;
	else
		(*task)->event_handler = echo_handler;

	return 0;
}

/* Properly dismantle and dispose of a task struct.
 * At this point we've guaranteed that noone else is accessing the
 * task struct, so we don't need any locks */
void duet_task_dispose(struct duet_task *task)
{
	bmaptree_dispose(&task->bmaptree);
	mutex_destroy(&task->bmaptree_mutex);
	kfree(task);
}

int duet_task_register(__u8 *taskid, const char *name, __u32 blksize,
	__u32 bmapsize, __u8 event_mask, duet_event_handler_t event_handler,
	void *privdata)
{
	int ret;
	struct list_head *last;
	struct duet_task *cur, *task = NULL;

	if (strnlen(name, TASK_NAME_LEN) == TASK_NAME_LEN) {
		printk(KERN_ERR "duet: error -- task name too long\n");
		return -EINVAL;
	}

	ret = duet_task_init(&task, name, blksize, bmapsize, event_mask,
		event_handler, privdata);
	if (ret) {
		printk(KERN_ERR "duet: failed to initialize task\n");
		return ret;
	}

	/* Find a free task id for the new task.
	 * Tasks are sorted by id, so that we can find the smallest
	 * free id in one traversal (just look for a gap). We don't
	 * want other ioctls touching this list, so we lock it
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
EXPORT_SYMBOL_GPL(duet_task_register);

int duet_task_deregister(__u8 taskid)
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
EXPORT_SYMBOL_GPL(duet_task_deregister);
