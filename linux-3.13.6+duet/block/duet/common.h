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
#ifndef _COMMON_H
#define _COMMON_H

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/list_bl.h>
#include <linux/bitmap.h>
#include <linux/rculist.h>
#include <linux/duet.h>

#define MAX_NAME		128
#define MAX_TASKS		15
#define DUET_BITS_PER_NODE	(32768 * 8)	/* 32KB bitmaps */

enum {
	DUET_STATUS_OFF = 0,
	DUET_STATUS_ON,
	DUET_STATUS_INIT,
	DUET_STATUS_CLEAN,
};

/*
 * Red-black bitmap tree node.
 * Represents the range starting from idx. For block tasks, only the done
 * bitmap is used. For file tasks, the relv (relevant) is also used, and the
 * following semantics apply:
 * - !RELV && !DONE: The item has not been encountered yet
 * - !RELV &&  DONE: The item is not relevant to the task
 * -  RELV && !DONE: The item is relevant, but not processed
 * -  RELV &&  DONE: The item is relevant, and has already been processed
 */
struct bmap_rbnode {
	__u64		idx;
	struct rb_node	node;
	unsigned long	*relv;
	unsigned long	*done;
};

struct item_hnode {
	struct hlist_bl_node	node;
	struct duet_item	item;
	__u8			refcount;
	__u8			state[MAX_TASKS];
};

struct duet_bittree {
	__u8			is_file;	/* Task type, as in duet_task */
	__u32			range;
	spinlock_t		lock;
	struct rb_root		root;
#ifdef CONFIG_DUET_STATS
	__u64			statcur;	/* Cur # of BitTree nodes */
	__u64			statmax;	/* Max # of BitTree nodes */
#endif /* CONFIG_DUET_STATS */
};

struct duet_task {
	__u8			id;
	__u8			is_file;	/* Task type: set if file task */
	char			name[MAX_NAME];
	struct list_head	task_list;
	wait_queue_head_t	cleaner_queue;
	atomic_t		refcount;
	__u8			evtmask;	/* Mask of subscribed events */
	char			*pathbuf;	/* Buffer for getpath */

	/* Optional heuristics to filter the events received */
	struct super_block	*f_sb;		/* Filesystem of task */
	struct dentry		*p_dentry;	/* Parent dentry */
	__u8			use_imap;	/* Use the inode bitmap */

	/* Hash table bucket bitmap */
	spinlock_t		bbmap_lock;
	unsigned long		*bucket_bmap;

	/* BitTree -- progress bitmap tree */
	struct duet_bittree	bittree;
};

struct duet_info {
	atomic_t		status;

	/*
	 * Access to the task list is synchronized via a mutex. However, any
	 * operations that are on-going for a task (e.g. fetch) will increase
	 * its refcount. This refcount is consulted when disposing of the task.
	 */
	struct mutex		task_list_mutex;
	struct list_head	tasks;

	/* ItemTable -- Global page state hash table */
	struct hlist_bl_head	*itm_hash_table;
	unsigned long		itm_hash_size;
	unsigned long		itm_hash_shift;
	unsigned long		itm_hash_mask;
#ifdef CONFIG_DUET_STATS
	unsigned long		itm_stat_lkp;	/* total lookups per request */
	unsigned long		itm_stat_num;	/* number of node requests */
#endif /* CONFIG_DUET_STATS */
};

extern struct duet_info duet_env;
extern duet_hook_t *duet_hook_cache_fp;
extern unsigned int *duet_i_hash_shift;
extern struct hlist_head **duet_inode_hashtable;
extern spinlock_t *duet_inode_hash_lock;
extern int d_find_path(struct inode *cnode, struct dentry *p_dentry,
			int getpath, char *buf, int len, char **p);

/* hash.c */
int hash_init(void);
int hash_add(struct duet_task *task, unsigned long ino, unsigned long idx,
	__u8 evtmask, short in_scan);
int hash_fetch(struct duet_task *task, struct duet_item *itm);
void hash_print(struct duet_task *task);

/* task.c -- not in linux/duet.h */
struct duet_task *duet_find_task(__u8 taskid);
void duet_task_dispose(struct duet_task *task);

/* ioctl.c */
int duet_bootstrap(void);
int duet_shutdown(void);
long duet_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int duet_find_path(struct duet_task *task, unsigned long inum, int getpath,
	char *path);

/* bittree.c */
int bittree_check(struct duet_bittree *bt, __u64 idx, __u32 len,
	struct duet_task *task);
inline int bittree_set_done(struct duet_bittree *bt, __u64 idx, __u32 len);
inline int bittree_unset_done(struct duet_bittree *bt, __u64 idx, __u32 len);
/*inline int bittree_set_relevance(struct duet_bittree *bt, __u64 idx, __u32 len,
	int is_relevant);*/

int bittree_print(struct duet_task *task);
void bittree_init(struct duet_bittree *bittree, __u32 range);
void bittree_destroy(struct duet_bittree *bittree);

#endif /* _COMMON_H */
