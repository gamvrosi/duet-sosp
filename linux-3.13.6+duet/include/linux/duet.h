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
#ifndef _DUET_H
#define _DUET_H

/* Notification models */
enum {
	DUET_MODEL_ADD = 1,		/* only ADD events */
	DUET_MODEL_REM = 2,		/* only REM events */
	DUET_MODEL_BOTH = 3,		/* both ADD/REM events */
	DUET_MODEL_DIFF = 4,		/* difference of ADD/REM events */
	DUET_MODEL_AXS = 5,		/* data accesses based on ADD/MOD */
};

/* Item struct returned for processing */
struct duet_item {
	unsigned long ino;
	unsigned long idx;
	__u8 evt;			/* added, removed, modified? */
};

/*
 * Event types: ADD (resp. REM) are triggered when a page __descriptor__ is
 * inserted in (resp. about to be removed from) the page cache. MOD is produced
 * when the page is dirtied (nb: no event is produced when a page is cleaned).
 */
#define DUET_EVT_ADD	(1 << 0)
#define DUET_EVT_REM	(1 << 1)
#define DUET_EVT_MOD	(1 << 2)

/* Framework interface functions */
int duet_register(__u8 *taskid, const char *name, __u8 nmodel, __u32 bitrange,
		  void *owner);
int duet_deregister(__u8 taskid);
int duet_online(void);
int duet_check(__u8 taskid, __u64 idx, __u32 num);
int duet_mark(__u8 taskid, __u64 idx, __u32 num);
int duet_unmark(__u8 taskid, __u64 idx, __u32 num);
int duet_fetch(__u8 taskid, __u16 req, struct duet_item *items, __u16 *ret);

/* InodeTree interface functions */
// TODO: int create_itree
// TODO: int update_itree
// TODO: int teardown_itree

/* Framework debugging functions */
int duet_print_bittree(__u8 taskid);
int duet_print_itmtree(__u8 taskid);

/* Hook functions that trasmit event info to the framework core */
void duet_hook(__u8 evtcode, void *data);
typedef void (duet_hook_t) (__u8, void *);

#endif /* _DUET_H */
