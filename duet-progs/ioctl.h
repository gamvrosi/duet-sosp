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
#ifndef _DUET_IOCTL_H
#define _DUET_IOCTL_H

#include <asm/types.h>
#include <sys/ioctl.h>

#define MAX_TASKS 32
#define MAX_ITEMS 128
#define TASK_NAME_LEN 128
#define DUET_IOCTL_MAGIC 0xDE

/* ioctl codes */
#define DUET_START		1
#define DUET_STOP		2
#define DUET_REGISTER		3
#define DUET_DEREGISTER		4
#define DUET_MARK		5
#define DUET_UNMARK		6
#define DUET_CHECK		7
#define DUET_FETCH		8
#define DUET_PRINTBIT		9
#define DUET_PRINTITEM		10

/* item types */
#define DUET_ITM_BLOCK		1
#define DUET_ITM_PAGE		2
#define DUET_ITM_INODE		3

struct duet_ioctl_item {
	__u64 itmnum;
	__u8 evttype;	
};

struct duet_ioctl_list_args {
	__u8 tid[MAX_TASKS];			/* out */
	char tnames[MAX_TASKS][TASK_NAME_LEN];	/* out */
	__u32 bitrange[MAX_TASKS];		/* out */
	__u32 bmapsize[MAX_TASKS];		/* out */
	__u8 evtmask[MAX_TASKS];		/* out */
	__u8 itmtype[MAX_TASKS];		/* out */
};

struct duet_ioctl_cmd_args {
	__u8 cmd_flags;					/* in */
	__u8 tid;					/* in/out */
	__u8 ret;					/* out */

	__u8 evtmask;					/* in */
	__u8 itmtype;					/* in */
	__u32 bitrange;					/* in */
	__u32 itmnum;					/* in/out */
	__u64 itmidx;					/* in */
	char tname[TASK_NAME_LEN];			/* in */
	struct duet_ioctl_item items[MAX_ITEMS];	/* out */
};

#define DUET_IOC_CMD	_IOWR(DUET_IOCTL_MAGIC, 1, struct duet_ioctl_cmd_args)
#define DUET_IOC_TLIST	_IOWR(DUET_IOCTL_MAGIC, 2, struct duet_ioctl_list_args)

#endif /* _DUET_IOCTL_H */
