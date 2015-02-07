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

/* Event data types: passed to the framework to populate the NodeTree(s) */
enum {
	DUET_DATA_BIO = 1,		/* data points to a bio */
	DUET_DATA_PAGE,			/* data points to a struct page */
	DUET_DATA_BLKREQ,		/* data points to a struct request */
};

/* Item types: task processing granularity */
enum {
	DUET_ITEM_INODE = 1,		/* will build a NodeTree of inodes */
	DUET_ITEM_PAGE,			/* will build a NodeTree of pages */
	DUET_ITEM_BLOCK,		/* will build a NodeTree of LBAs */
};

/* Hook types: tells framework where in the storage stack the event occurred */
enum {
	DUET_HOOK_BA = 1,		/* async: submit_bio */
	DUET_HOOK_BW_START,		/* sync: submit_bio_wait (before) */
	DUET_HOOK_BW_END,		/* sync: submit_bio_wait (after) */
	DUET_HOOK_PAGE,			/* struct page hook after the event */
	DUET_HOOK_SCHED_INIT,		/* scheduler data request initiation */
	DUET_HOOK_SCHED_DONE,		/* scheduler data request completion */
};

/* The special hook data struct needed for the darned submit_bio_wait */
struct duet_bw_hook_data {
	struct bio	*bio;
	__u64		offset;
};

/* Item struct returned for processing */
struct duet_item {
	__u64	number;	/* inode, block, or page (addr >> size) number */
	__u8	evt;	/* event that triggered item to be returned */
	union {
		struct inode	*inode;
		struct page	*page;
		struct bio	*bio;
	};
};

/* bio flag */
#define BIO_DUET        22      /* duet has seen this bio */

/* Hook codes: basically, these determine the event type */
#ifdef CONFIG_DUET_FS
/*
 * FS_READ (resp. FS_WRITE) are triggered when a bio is sent as part of a read
 * (resp. write), synchronously or asynchronously.
 * Note: until bio callback completes, interrupts are raised.
 */
#define DUET_EVT_FS_READ	(1 << 0)
#define DUET_EVT_FS_WRITE	(1 << 1)
#endif /* CONFIG_DUET_FS */

#ifdef CONFIG_DUET_CACHE
/*
 * CACHE_INSERT (resp. CACHE_REMOVE) are expected to be triggered when a page
 * __descriptor__ is inserted in (resp. removed from) the page cache. Control
 * is transferred to the handler after the operation completes, with interrupts
 * restored. Note that the page will still be locked in the case of insertion,
 * as its data will not be current (until IO completes and unlocks it).
 */
#define DUET_EVT_CACHE_INSERT	(1 << 2)
#define DUET_EVT_CACHE_REMOVE	(1 << 3)
#define DUET_EVT_CACHE_MODIFY	(1 << 4)
#endif /* CONFIG_DUET_CACHE */

#ifdef CONFIG_DUET_SCHED
/*
 * SCHED_INIT and SCHED_DONE are expected to be triggered when a block request
 * or removed from a device's dispatch queue. We replace the callback in the
 * struct request with a callback to the framework.
 */
#define DUET_EVT_SCHED_INIT	(1 << 5)
#define DUET_EVT_SCHED_DONE	(1 << 6)
#endif /* CONFIG_DUET_SCHED */

/* Framework interface functions */
int duet_register(__u8 *taskid, const char *name, __u8 itmtype, __u32 bitrange,
						__u8 evtmask, void *owner);
int duet_deregister(__u8 taskid);
int duet_online(void);
int duet_check(__u8 taskid, __u64 idx, __u32 num);
int duet_mark(__u8 taskid, __u64 idx, __u32 num);
int duet_fetch(__u8 taskid, __u8 itreq, struct duet_item *items, __u8 *itret);
int duet_trim_trees(__u8 taskid, __u64 low, __u64 high);

/* Framework debugging functions */
int duet_print_bittree(__u8 taskid);
int duet_print_itmtree(__u8 taskid);

/* Hook functions that trasmit event info to the framework core */
void duet_hook(__u8 event_type, __u8 hook_type, void *hook_data);
typedef void (duet_hook_t) (__u8, __u8, void *);

#endif /* _DUET_H */
