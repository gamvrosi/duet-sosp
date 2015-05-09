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
#include <linux/workqueue.h>

#define MAX_NAME	128
#define MAX_TASKS	15

enum {
	DUET_STATUS_OFF = 0,
	DUET_STATUS_ON,
	DUET_STATUS_INIT,
	DUET_STATUS_CLEAN,
};

struct bmap_rbnode {
	__u64		idx;
	struct rb_node	node;
	__u8		*bmap;
};

struct item_hnode {
	struct hlist_bl_node	node;
	struct duet_item	item;
	__u8			refcount;
	__u8			state[MAX_TASKS];
};

struct duet_task {
	__u8			id;
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
	__u32			bitrange;	/* range per bmap bit */
	__u32			bmapsize;	/* bytes per bmap */
	spinlock_t		bittree_lock;
	struct rb_root		bittree;
#ifdef CONFIG_DUET_STATS
	__u64			stat_bit_cur;	/* Cur # of BitTree nodes */
	__u64			stat_bit_max;	/* Max # of BitTree nodes */
#endif /* CONFIG_DUET_STATS */
};

struct evtwork {
	struct work_struct 	work;

	__u8			evt;
	unsigned long 		ino;
	unsigned long 		idx;
	struct super_block	*isb;
};

struct duet_info {
	atomic_t		status;

	/* 
	 * List or registered tasks, protected by a mutex so we can safely walk
	 * it to find handlers without worrying about add/remove operations
	 */
	struct mutex		task_list_mutex;
	struct list_head	tasks;

	/* Workqueue of events not processed yet */
	spinlock_t		evtwq_lock;
	struct workqueue_struct	*evtwq;

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
extern char *d_get_path(struct inode *cnode, struct dentry *p_dentry,
			char *buf, int len);

/* XXX: bmap.c */
__u32 duet_bmap_count(__u8 *bmap, __u32 byte_len);
void duet_bmap_print(__u8 *bmap, __u32 byte_len);
int duet_bmap_set(__u8 *bmap, __u32 bmap_bytelen, __u64 first_byte,
		__u32 blksize, __u64 req_byte, __u32 req_bytelen, __u8 set);
int duet_bmap_chk(__u8 *bmap, __u32 bmap_bytelen, __u64 first_byte,
		__u32 blksize, __u64 req_byte, __u32 req_bytelen, __u8 set);

/* hash.c */
int hash_init(void);
int hash_add(struct duet_task *task, unsigned long ino, unsigned long idx,
	__u8 evtmask, short replace);
int hash_fetch(struct duet_task *task, struct duet_item *itm);
void hash_print(struct duet_task *task);

/* task.c -- not in linux/duet.h */
struct duet_task *duet_find_task(__u8 taskid);
void duet_task_dispose(struct duet_task *task);

/* ioctl.c */
int duet_bootstrap(void);
int duet_shutdown(void);
long duet_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* bittree.c */
void bnode_dispose(struct bmap_rbnode *bnode, struct rb_node *rbnode,
		struct rb_root *root, struct duet_task *task);
inline int bittree_check(struct rb_root *bittree, __u32 range, __u32 bmapsize,
			__u64 idx, __u32 num, struct duet_task *task);
inline int bittree_mark(struct rb_root *bittree, __u32 range, __u32 bmapsize,
			__u64 idx, __u32 num, struct duet_task *task);
inline int bittree_unmark(struct rb_root *bittree, __u32 range, __u32 bmapsize,
			__u64 idx, __u32 num, struct duet_task *task);
int bittree_print(struct duet_task *task);

#endif /* _COMMON_H */
