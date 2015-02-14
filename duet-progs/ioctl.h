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

#define MAX_TASKS	32
#define MAX_ITEMS	256
#define MAX_NAME	128
#define DUET_IOC_MAGIC	0xDE

/* ioctl codes */
#define DUET_START	1
#define DUET_STOP	2
#define DUET_REGISTER	3
#define DUET_DEREGISTER	4
#define DUET_MARK	5
#define DUET_UNMARK	6
#define DUET_CHECK	7
#define DUET_PRINTBIT	8
#define DUET_PRINTITEM	9

/* notification model codes */
#define MODEL_ADD	1	/* only ADD events */
#define MODEL_REM	2	/* only REM events */
#define MODEL_BOTH	3	/* both ADD/REM events */
#define MODEL_DIFF	4	/* difference of ADD/REM events */
#define MODEL_AXS	5	/* data accesses based on ADD/MOD */

/* Item struct returned for processing */
struct duet_item {
	unsigned long	ino;
	unsigned long	idx;
	__u8		evt;	/* added, removed, modified? */
};

/* We return up to MAX_ITEMS at a time (9b each). */
struct duet_ioctl_fetch_args {
	__u8 			tid;		/* in */
	__u16 			num;		/* in/out */
	struct duet_item	itm[MAX_ITEMS];	/* out */
};

struct duet_ioctl_list_args {
	__u8 	tid[MAX_TASKS];			/* out */
	char 	tnames[MAX_TASKS][MAX_NAME];	/* out */
	__u32 	bitrange[MAX_TASKS];		/* out */
	__u8	nmodel[MAX_TASKS];		/* out */
};

struct duet_ioctl_cmd_args {
	__u8 	cmd_flags;			/* in */
	__u8 	tid;				/* in/out */
	__u8 	ret;				/* out */
	union {
		/* Registration args */
		struct {
			__u8 	nmodel;		/* in */
			__u32 	bitrange;	/* in */
			char 	name[MAX_NAME];	/* in */
		};
		/* (Un)marking and checking args */
		struct {
			__u32 	itmnum;		/* in */
			__u64 	itmidx;		/* in */
		};
	};	
};

#define DUET_IOC_CMD	_IOWR(DUET_IOC_MAGIC, 1, struct duet_ioctl_cmd_args)
#define DUET_IOC_TLIST	_IOWR(DUET_IOC_MAGIC, 2, struct duet_ioctl_list_args)
#define DUET_IOC_FETCH	_IOWR(DUET_IOC_MAGIC, 3, struct duet_ioctl_fetch_args)

#endif /* _DUET_IOCTL_H */
