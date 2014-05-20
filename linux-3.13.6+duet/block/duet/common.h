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

#ifndef _COMMON_H
#define _COMMON_H
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/rbtree.h>
#include <linux/rculist.h>
#include <linux/duet.h>

#define TASK_NAME_LEN 128

enum {
	DUET_STATUS_OFF = 0,
	DUET_STATUS_ON,
	DUET_STATUS_INIT,
	DUET_STATUS_CLEAN,
};

struct duet_rbnode {
	__u64		lbn;
	struct rb_node	node;
	__u8		*bmap;
};

struct duet_task {
	__u8			id;
	char			name[TASK_NAME_LEN];
	struct list_head	task_list;
	wait_queue_head_t	cleaner_queue;
	atomic_t		refcount;

	/* bitmap tree */
	__u32			blksize;		/* bytes per bmap bit */
	__u32			bmapsize;		/* bytes per bmap */
	struct block_device	*bdev;			/* task block device */
	struct mutex		bmaptree_mutex;
	struct rb_root		bmaptree;

	/* hook handling */
	__u8			hook_mask;
	duet_hook_handler_t	*hook_handler;
	void			*privdata;
};

struct duet_info {
	atomic_t		status;

	/*
	 * all the registered tasks in the framework, protected by a mutex
	 * so we can safely walk the list to find handlers without worrying
	 * about add/remove operations
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
void duet_task_dispose(struct duet_task *task);
int duet_print_rbt(__u8 taskid);

/* ioctl.c */
int duet_bootstrap(void);
int duet_shutdown(void);
long duet_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif /* _COMMON_H */
