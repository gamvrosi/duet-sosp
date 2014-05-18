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

#ifndef _DUET_IOCTL_H
#define _DUET_IOCTL_H
#include <linux/ioctl.h>
#include "common.h"

#define DUET_IOCTL_MAGIC 0xDE

/* status ioctl flags */
#define DUET_STATUS_START (1 << 0)
#define DUET_STATUS_STOP  (1 << 1)

/* tasks ioctl flags */
#define DUET_TASKS_LIST		(1 << 0)
#define DUET_TASKS_REGISTER	(1 << 1)
#define DUET_TASKS_DEREGISTER	(1 << 2)

/* debug ioctl flags */
#define DUET_DEBUG_ADDBLK	(1 << 0)
#define DUET_DEBUG_RMBLK	(1 << 1)
#define DUET_DEBUG_CHKBLK	(1 << 2)
#define DUET_DEBUG_PRINTRBT	(1 << 3)

struct duet_ioctl_status_args {
	__u8 cmd_flags;		/* in */
};

#define MAX_TASKS 32
struct duet_ioctl_tasks_args {
	__u8 cmd_flags;					/* in */
	__u8 taskid[MAX_TASKS];				/* in/out */
	__u32 blksize[MAX_TASKS];			/* in/out */
	__u32 bmapsize[MAX_TASKS];			/* in/out */
	char task_names[MAX_TASKS][TASK_NAME_LEN];	/* in/out */
};

struct duet_ioctl_debug_args {
	__u8 cmd_flags;		/* in */
	__u8 taskid;		/* in */
	__u64 offset;		/* in */
	__u32 len;		/* in */
	__u8 unset;		/* in */
	__u8 ret;		/* out */
};

#define DUET_IOC_STATUS _IOW(DUET_IOCTL_MAGIC, 1, \
				struct duet_ioctl_status_args)
#define DUET_IOC_TASKS _IOWR(DUET_IOCTL_MAGIC, 2, \
				struct duet_ioctl_tasks_args)
#define DUET_IOC_DEBUG _IOWR(DUET_IOCTL_MAGIC, 3, \
				struct duet_ioctl_debug_args)

#endif /* _DUET_IOCTL_H */
