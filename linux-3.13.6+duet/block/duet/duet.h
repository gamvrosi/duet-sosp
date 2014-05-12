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

#ifndef _DUET_H
#define _DUET_H
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/rculist.h>

#define TASK_NAME_LEN 128

struct duet_task {
	struct list_head task_list;

	/* task identification info */
	__u8 id;
	char name[TASK_NAME_LEN];

	/* bitmap tree */
	//struct mutex bmaptree_mutex;
	//struct rb_root bmaptree;

	/*
	 * hook handling -- the handler function prototype is:
	 * hhandler(taskid, hookmask, bytenr, bytelen, privdata)
	 */
	//__u8 hflags;
	//void (*hhandler) (__u8, __u8, __u64, __u32, void *);
};

enum {
	DUET_STATUS_OFF = 0,
	DUET_STATUS_ON,
	DUET_STATUS_INIT,
	DUET_STATUS_CLEAN,
};

struct duet_info {
	atomic_t status;

	/*
	 * all the registered tasks in the framework, protected by a mutex
	 * so we can safely walk the list to find handlers without worrying
	 * about add/remove operations
	 */
	struct mutex task_list_mutex;
	struct list_head tasks;
};

extern struct duet_info duet_env;

void duet_task_dispose(struct duet_task *task);
int duet_task_register(__u8 *taskid, const char *name);
int duet_task_deregister(__u8 taskid);

#endif /* _DUET_H */
