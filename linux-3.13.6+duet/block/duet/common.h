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
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/rbtree.h>
#include <linux/rculist.h>
#include <linux/duet.h>

#define TASK_NAME_LEN	128

#ifdef CONFIG_DUET_DEBUG
#define duet_dbg(...)	printk(__VA_ARGS__)
#else
#define duet_dbg(...)
#endif

#ifdef CONFIG_DUET_CACHE
extern void (*duet_hook_cache_fp)(__u8, __u8, void *);
#endif /* CONFIG_DUET_CACHE */
#ifdef CONFIG_DUET_SCHED
extern void (*duet_hook_blk_fp)(__u8, __u8, void *);
#endif /* CONFIG_DUET_SCHED */
#ifdef CONFIG_DUET_FS
extern void (*duet_hook_fs_fp)(__u8, __u8, void *);
#endif /* CONFIG_DUET_FS */

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

struct duet_task {
	__u8			id;
	char			name[TASK_NAME_LEN];
	struct list_head	task_list;
	wait_queue_head_t	cleaner_queue;
	atomic_t		refcount;
	__u8			evtmask;	/* Event codes of interest */
	union {
		/* Filesystem or device this task is operating on */
		struct super_block	*sb;
		struct block_device	*bdev;
	};

	/* BitTree -- progress bitmap tree */
	__u32			bitrange;	/* range per bmap bit */
	__u32			bmapsize;	/* bytes per bmap */
	spinlock_t		bittree_lock;
	struct rb_root		bittree;
#ifdef CONFIG_DUET_TREE_STATS
	__u64			stat_bit_cur;	/* Cur # of BitTree nodes */
	__u64			stat_bit_max;	/* Max # of BitTree nodes */
#endif /* CONFIG_DUET_TREE_STATS */
#if 0
	__u64			min_idx;	/* Min lbn/inode of interest */
	__u64			max_idx;	/* Max lbn/inode of interest */
#endif /* 0 */

	/* ItemTree -- item events tree */
	__u8			itmtype;
	spinlock_t		itm_inner_lock;
	spinlock_t		itm_outer_lock;
	union {
		struct rb_root		itmtree;	/* Page tree */
		struct list_head	itmlist;	/* bio list */
	};
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

/* hook.c */
int duet_dispose_item(struct duet_item *itm, __u8 type);

/* ioctl.c */
int duet_bootstrap(void);
int duet_shutdown(void);
long duet_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif /* _COMMON_H */
