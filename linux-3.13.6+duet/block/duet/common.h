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
#include <linux/rculist.h>
#include <linux/duet.h>

#define MAX_NAME	128

enum {
	DUET_STATUS_OFF = 0,
	DUET_STATUS_ON,
	DUET_STATUS_INIT,
	DUET_STATUS_CLEAN,
};

struct bmap_rbnode {
	__u64			idx;
	struct rb_node		node;
	__u8			*bmap;
};

struct item_rbnode {
	struct rb_node		node;
	struct duet_item	*item;
};

/* Notification model masks */
#define DUET_MODEL_ADD_MASK	(DUET_EVT_ADD)
#define DUET_MODEL_REM_MASK	(DUET_EVT_REM)
#define DUET_MODEL_BOTH_MASK	(DUET_EVT_ADD | DUET_EVT_REM)
#define DUET_MODEL_DIFF_MASK	(DUET_EVT_ADD | DUET_EVT_REM)
#define DUET_MODEL_AXS_MASK	(DUET_EVT_ADD | DUET_EVT_MOD)

struct duet_task {
	__u8			id;
	char			name[MAX_NAME];
	struct list_head	task_list;
	wait_queue_head_t	cleaner_queue;
	atomic_t		refcount;
	__u8			nmodel;		/* Notification model */
	struct super_block	*sb;		/* Filesystem of task (opt) */

	/* BitTree -- progress bitmap tree */
	__u32			bitrange;	/* range per bmap bit */
	__u32			bmapsize;	/* bytes per bmap */
	spinlock_t		bittree_lock;
	struct rb_root		bittree;
#ifdef CONFIG_DUET_TREE_STATS
	__u64			stat_bit_cur;	/* Cur # of BitTree nodes */
	__u64			stat_bit_max;	/* Max # of BitTree nodes */
#endif /* CONFIG_DUET_TREE_STATS */

	/* ItemTree -- item events tree */
	__u8			itmtype;	/* pages or inodes? */
	spinlock_t		itm_lock;
	//spinlock_t		itm_hook_lock;
	struct rb_root		itmtree;
#ifdef CONFIG_DUET_TREE_STATS
	__u64			stat_itm_cur;	/* Cur # of ItemTree nodes */
	__u64			stat_itm_max;	/* Max # of ItemTree nodes */
#endif /* CONFIG_DUET_TREE_STATS */
};

struct duet_info {
	atomic_t		status;

	/* 
	 * List or registered tasks, protected by a mutex so we can safely walk
	 * it to find handlers without worrying about add/remove operations
	 */
	struct mutex		task_list_mutex;
	struct list_head	tasks;
};

extern struct duet_info duet_env;
extern duet_hook_t *duet_hook_cache_fp;

/* bmap.c */
__u32 duet_bmap_count(__u8 *bmap, __u32 byte_len);
void duet_bmap_print(__u8 *bmap, __u32 byte_len);
int duet_bmap_set(__u8 *bmap, __u32 bmap_bytelen, __u64 first_byte,
	__u32 blksize, __u64 req_byte, __u32 req_bytelen, __u8 set);
int duet_bmap_chk(__u8 *bmap, __u32 bmap_bytelen, __u64 first_byte,
	__u32 blksize, __u64 req_byte, __u32 req_bytelen, __u8 set);

/* task.c -- not in linux/duet.h */
struct duet_task *duet_find_task(__u8 taskid);
void duet_task_dispose(struct duet_task *task);

/* ioctl.c */
int duet_bootstrap(void);
int duet_shutdown(void);
long duet_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* rbtrees.c */
struct bmap_rbnode *bnode_init(struct duet_task *task, __u64 idx);
void bnode_dispose(struct bmap_rbnode *bnode, struct rb_node *rbnode,
	struct rb_root *root);
struct item_rbnode *tnode_init(struct duet_task *task, unsigned long ino,
	unsigned long idx, __u8 evt);
void tnode_dispose(struct item_rbnode *tnode, struct rb_node *rbnode,
	struct rb_root *root);

#endif /* _COMMON_H */
