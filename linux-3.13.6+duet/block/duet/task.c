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
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include "common.h"

/*
 * To synchronize access to the task list and structures without compromising
 * scalability, a two-level approach is used. At the task list level, which is
 * rarely updated, RCU is used. For the task structures, we use traditional
 * reference counting. The two techniques are interweaved to achieve overall
 * consistency.
 */

static int process_inode(struct duet_task *task, struct inode *inode)
{
	struct radix_tree_iter iter;
	void **slot;
	__u8 state;

	/* Go through all pages of this inode */
	rcu_read_lock();
	radix_tree_for_each_slot(slot, &inode->i_mapping->page_tree, &iter, 0) {
		struct page *page = radix_tree_deref_slot(slot);
		if (unlikely(!page))
			continue;

		state = DUET_PAGE_ADDED;
		if (PageDirty(page))
			state |= DUET_PAGE_DIRTY;
		hash_add(task, inode->i_ino, page->index, state, 1);
	}
	rcu_read_unlock();

	return 0;
}

/* Scan through the page cache, and populate the task's tree. */
static int scan_page_cache(struct duet_task *task)
{
	unsigned int loop;
	struct hlist_head *head;
	struct inode *inode = NULL;
	struct duet_bittree inodetree;

	bittree_init(&inodetree, 1, 0);
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
			if (bittree_check(&inodetree, inode->i_ino, 1, NULL) != 1) {
				spin_lock(&inode->i_lock);
				__iget(inode);
				spin_unlock(&inode->i_lock);

				spin_unlock(duet_inode_hash_lock);
				process_inode(task, inode);
				bittree_set_done(&inodetree, inode->i_ino, 1);
				iput(inode);
				goto again;
			}
		}

		spin_unlock(duet_inode_hash_lock);
	}

	printk(KERN_INFO "duet: page cache scan finished\n");
	bittree_destroy(&inodetree);

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

/* Do a preorder print of the BitTree */
int duet_print_bitmap(__u8 taskid)
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
EXPORT_SYMBOL_GPL(duet_print_bitmap);

/* Do a preorder print of the global hash table */
int duet_print_events(__u8 taskid)
{
	struct duet_task *task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	hash_print(task);

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return 0;
}
EXPORT_SYMBOL_GPL(duet_print_events);

/* Checks whether items in the [idx, idx+count) range are done */
int duet_check_done(__u8 taskid, __u64 idx, __u32 count)
{
	int ret = 0;
	struct duet_task *task;

	if (!duet_online())
		return -1;

	task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	ret = bittree_check(&task->bittree, idx, count, task);

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return ret;
}
EXPORT_SYMBOL_GPL(duet_check_done);

/* Unmarks items in the [idx, idx+count) range, i.e. not done */
int duet_unset_done(__u8 taskid, __u64 idx, __u32 count)
{
	int ret = 0;
	struct duet_task *task;

	if (!duet_online())
		return -1;

	task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	ret = bittree_unset_done(&task->bittree, idx, count);

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return ret;
}
EXPORT_SYMBOL_GPL(duet_unset_done);

/* Mark items in the [idx, idx+count) range, i.e. done */
int duet_set_done(__u8 taskid, __u64 idx, __u32 count)
{
	int ret = 0;
	struct duet_task *task;

	if (!duet_online())
		return -1;

	task = duet_find_task(taskid);
	if (!task)
		return -ENOENT;

	ret = bittree_set_done(&task->bittree, idx, count);

	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return ret;
}
EXPORT_SYMBOL_GPL(duet_set_done);

/* Properly allocate and initialize a task struct */
static int duet_task_init(struct duet_task **task, const char *name,
	__u8 evtmask, __u32 bitrange, struct super_block *f_sb,
	struct dentry *p_dentry)
{
	*task = kzalloc(sizeof(**task), GFP_KERNEL);
	if (!(*task))
		return -ENOMEM;

	(*task)->pathbuf = kzalloc(4096, GFP_KERNEL);
	if (!(*task)->pathbuf) {
		printk(KERN_ERR "duet: failed to allocate pathbuf for task\n");
		kfree(*task);
		return -ENOMEM;
	}

	(*task)->id = 1;
	memcpy((*task)->name, name, MAX_NAME);
	atomic_set(&(*task)->refcount, 0);
	INIT_LIST_HEAD(&(*task)->task_list);
	init_waitqueue_head(&(*task)->cleaner_queue);

	/* Is this a file or a block task? */
	(*task)->is_file = ((evtmask & DUET_FILE_TASK) ? 1 : 0);

	/* Initialize bitmap tree */
	if (!bitrange)
		bitrange = 4096;
	bittree_init(&(*task)->bittree, bitrange, (*task)->is_file);

	/* Initialize hash table bitmap */
	spin_lock_init(&(*task)->bbmap_lock);
	(*task)->bucket_bmap = kzalloc(sizeof(unsigned long) *
		BITS_TO_LONGS(duet_env.itm_hash_size), GFP_KERNEL);
	if (!(*task)->bucket_bmap) {
		printk(KERN_ERR "duet: failed to allocate bucket bitmap\n");
		kfree((*task)->pathbuf);
		kfree(*task);
		return -ENOMEM;
	}

	/* Do some sanity checking on event mask. */
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

	(*task)->evtmask = evtmask & (~DUET_FILE_TASK);
	(*task)->f_sb = f_sb;
	(*task)->p_dentry = p_dentry;

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
	struct duet_item itm;

	/* Dispose of the bitmap tree */
	bittree_destroy(&task->bittree);

	/* Dispose of hash table entries, bucket bitmap */
	while (!hash_fetch(task, &itm));
	kfree(task->bucket_bmap);

	if (task->p_dentry)
		dput(task->p_dentry);
	kfree(task->pathbuf);
	kfree(task);
}

/* Registers a user-level task. Must also prep path. */
int __register_utask(char *path, __u8 evtmask, __u32 bitrange, const char *name,
	__u8 *taskid)
{
	int ret, fd;
	struct list_head *last;
	struct duet_task *cur, *task = NULL;
	struct file *file;
	mm_segment_t old_fs;
	struct dentry *dentry = NULL;
	struct super_block *sb;

	/* First, open the path we were given */
	old_fs = get_fs();
	set_fs(KERNEL_DS);

	fd = sys_open(path, O_RDONLY, 0644);
	if (fd < 0) {
		printk(KERN_ERR "duet_register: failed to open %s\n", path);
		ret = -EINVAL;
		goto reg_done;
	}

	file = fget(fd);
	if (!file) {
		printk(KERN_ERR "duet_register: failed to get %s\n", path);
		ret = -EINVAL;
		goto reg_close;
	}

	if (!file->f_inode) {
		printk(KERN_ERR "duet_register: no inode for %s\n", path);
		ret = -EINVAL;
		goto reg_put;
	}

	if (!S_ISDIR(file->f_inode->i_mode)) {
		printk(KERN_ERR "duet_register: %s is not a dir\n", path);
		ret = -EINVAL;
		goto reg_put;
	}

	if (!(dentry = d_find_alias(file->f_inode))) {
		printk(KERN_ERR "duet_register: no dentry for %s\n", path);
		ret = -EINVAL;
		goto reg_put;
	}

	sb = file->f_inode->i_sb;

	if (strnlen(name, MAX_NAME) == MAX_NAME) {
		printk(KERN_ERR "duet_register: task name too long\n");
		ret = -EINVAL;
		goto reg_put;
	}

	ret = duet_task_init(&task, name, evtmask, bitrange, sb, dentry);
	if (ret) {
		printk(KERN_ERR "duet_register: failed to initialize task\n");
		ret = -EINVAL;
		goto reg_put;
	}

	/*
	 * Find a free task id for the new task. Tasks are sorted by id, so that
	 * we can find the smallest free id in one traversal (look for a gap).
	 */
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

	printk(KERN_INFO "duet: registered %s (ino %lu, sb %p)\n",
		path, file->f_inode->i_ino, sb);

reg_put:
	fput(file);
reg_close:
	sys_close(fd);
reg_done:
	set_fs(old_fs);
	return ret;
}

/* Registers a kernel task. No path prep required */
int __register_ktask(char *path, __u8 evtmask, __u32 bitrange, const char *name,
	__u8 *taskid)
{
	int ret;
	struct list_head *last;
	struct duet_task *cur, *task = NULL;
	struct super_block *sb;

	sb = (struct super_block *)path;

	if (strnlen(name, MAX_NAME) == MAX_NAME) {
		printk(KERN_ERR "duet_register: task name too long\n");
		return -EINVAL;
	}

	ret = duet_task_init(&task, name, evtmask, bitrange, sb, NULL);
	if (ret) {
		printk(KERN_ERR "duet_register: failed to initialize task\n");
		return -EINVAL;
	}

	/*
	 * Find a free task id for the new task. Tasks are sorted by id, so that
	 * we can find the smallest free id in one traversal (look for a gap).
	 */
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

	printk(KERN_INFO "duet: registered kernel task (sb %p)\n", sb);

	return ret;
}

int duet_register(char *path, __u8 evtmask, __u32 bitrange, const char *name,
	__u8 *taskid)
{
	int ret;

	if (evtmask & DUET_REG_SBLOCK)
		ret = __register_ktask(path, evtmask, bitrange, name, taskid);
	else
		ret = __register_utask(path, evtmask, bitrange, name, taskid);

	return ret;
}
EXPORT_SYMBOL_GPL(duet_register);

int duet_deregister(__u8 taskid)
{
	struct duet_task *cur;

	/* Find the task in the list, then dispose of it */
	mutex_lock(&duet_env.task_list_mutex);
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		if (cur->id == taskid) {
#ifdef CONFIG_DUET_STATS
			hash_print(cur);
			bittree_print(cur);
#endif /* CONFIG_DUET_STATS */
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
