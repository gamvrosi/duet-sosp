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
#include "duet.h"

/* NB: some definitions have been moved to duet.h */
#define DUET_MAX_TASKS	15
#define DUET_IOC_MAGIC	0xDE

/* ioctl codes */
enum duet_ioctl_codes {
	DUET_START = 1,
	DUET_STOP,
	DUET_REGISTER,
	DUET_DEREGISTER,
	DUET_SET_DONE,
	DUET_UNSET_DONE,
	DUET_CHECK_DONE,
	DUET_PRINTBIT,
	DUET_PRINTITEM,
	DUET_GET_PATH,
};

/* We return up to MAX_ITEMS at a time (9b each). */
struct duet_ioctl_fetch_args {
	__u8 			tid;			/* in */
	__u16 			num;			/* in/out */
	struct duet_item	itm[DUET_MAX_ITEMS];	/* out */
};

struct duet_ioctl_list_args {
	__u8 	tid[DUET_MAX_TASKS];			/* out */
	char 	tnames[DUET_MAX_TASKS][DUET_MAX_NAME];	/* out */
	__u8	is_file[DUET_MAX_TASKS];		/* out */
	__u32 	bitrange[DUET_MAX_TASKS];		/* out */
	__u16	evtmask[DUET_MAX_TASKS];		/* out */
};

struct duet_ioctl_cmd_args {
	__u8 	cmd_flags;				/* in */
	__u8 	tid;					/* in/out */
	__u8 	ret;					/* out */
	union {
		/* Registration args */
		struct {
			__u32 	regmask;		/* in */
			__u32 	bitrange;		/* in */
			char 	name[DUET_MAX_NAME];	/* in */
			char	path[DUET_MAX_PATH];	/* in */
		};
		/* (Un)marking and checking args */
		struct {
			__u32 	itmnum;			/* in */
			__u64 	itmidx;			/* in */
		};
		/* ino -> path args */
		struct {
			unsigned long c_ino;		/* in */
			char cpath[DUET_MAX_PATH];	/* out */
		};
	};	
};

#define DUET_IOC_CMD	_IOWR(DUET_IOC_MAGIC, 1, struct duet_ioctl_cmd_args)
#define DUET_IOC_TLIST	_IOWR(DUET_IOC_MAGIC, 2, struct duet_ioctl_list_args)
#define DUET_IOC_FETCH	_IOWR(DUET_IOC_MAGIC, 3, struct duet_ioctl_fetch_args)

#endif /* _DUET_IOCTL_H */
