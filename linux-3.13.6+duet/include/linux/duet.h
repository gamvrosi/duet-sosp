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

/*
 * Task-specific event handling function. Should handle all event types
 * registered in the event mask. Arguments:
 *   __u8 taskid
 *   __u8 event_code
 *   void *owner: {struct block_device *bdev, struct inode *inode}
 *   __u64 offt: {p-offt, i-offt}
 *   __u32 len: {p-len, i-len}
 *   void *data: {struct bio *bio, struct page *page}
 *   int data_type: DUET_DATA_{BIO, BH, PAGE}
 *   void *private
 */
typedef void (duet_event_handler_t) (__u8, __u8, void *, __u64, __u32, void *,
								int, void *);

/*
 * Framework hook function. Should transmit hook information to the core of
 * the framework for passing to interested tasks. Arguments: __u8 event_type,
 * __u8 hook_type, void *hook_data
 */
typedef void (duet_hook_t) (__u8, __u8, void *);

/* Return data types: we need this to access the data at the handler */
enum {
	DUET_DATA_BIO = 1,		/* data points to a bio */
	DUET_DATA_BH,			/* data points to a buffer head */
	DUET_DATA_PAGE,			/* data points to a struct page */
	DUET_DATA_BLKREQ,		/* data points to a struct request */
};

/* Hook types: these change depending on what we're hooking on */
enum {
	DUET_SETUP_HOOK_BA = 1,		/* async: submit_bio */
	DUET_SETUP_HOOK_BW_START,	/* sync: submit_bio_wait (before) */
	DUET_SETUP_HOOK_BW_END,		/* sync: submit_bio_wait (after) */
	DUET_SETUP_HOOK_BH,		/* async: submit_bh */
	DUET_SETUP_HOOK_PAGE,		/* struct page hook after the event */
	DUET_SETUP_HOOK_BLKREQ_INIT,	/* block layer data request initiation */
	DUET_SETUP_HOOK_BLKREQ_DONE,	/* block layer data request completion */
};

/* The special hook data struct needed for the darned submit_bio_wait */
struct duet_bw_hook_data {
	struct bio	*bio;
	__u64		offset;
};

/* bio flag */
#define BIO_DUET        22      /* duet has seen this bio */

/* Hook codes: basically, these determine the event type */
#ifdef CONFIG_DUET_BTRFS
/*
 * BTRFS_READ (resp. BTRFS_WRITE) are expected to be triggered at the critical
 * path of btrfs, when a bio is sent as part of a read (resp. write). In the
 * case of the BW_START hook type, control is transferred to the framework
 * before the bio is dispatched. In the case of all the other hooks, control is
 * transferred to the handler after the bio callback completes, but while
 * interrupts are still raised. This is done to ensure that the bio will not be
 * freed while we operate on it, and it is suggested that intended work be
 * deferred using an appropriate mechanism (e.g. a work queue).
 */
#define DUET_EVENT_BTRFS_READ	(1 << 0)
#define DUET_EVENT_BTRFS_WRITE	(1 << 1)
#endif /* CONFIG_DUET_BTRFS */

#ifdef CONFIG_DUET_CACHE
/*
 * CACHE_INSERT (resp. CACHE_REMOVE) are expected to be triggered when a page
 * __descriptor__ is inserted in (resp. removed from) the page cache. Control
 * is transferred to the handler after the operation completes, with interrupts
 * restored. Note that the page will still be locked in the case of insertion,
 * as its data will not be current (until IO completes and unlocks it).
 */
#define DUET_EVENT_CACHE_INSERT	(1 << 2)
#define DUET_EVENT_CACHE_REMOVE	(1 << 3)
#define DUET_EVENT_CACHE_MODIFY	(1 << 4)
#endif /* CONFIG_DUET_CACHE */

#ifdef CONFIG_DUET_BLOCK
/*
 * BLOCK_READ and BLOCK_WRITE are expected to be triggered when a block request
 * is added to a device's dispatch queue. We replace the callback in the struct
 * request with a callback to Duet
 */
#define DUET_EVENT_BLKREQ_INIT	(1 << 5)
#define DUET_EVENT_BLKREQ_DONE	(1 << 6)
#endif /* CONFIG_DUET_BLOCK */

/* Core interface functions */
int duet_task_register(__u8 *taskid, const char *name, __u32 blksize,
	__u32 bmapsize, __u8 event_mask, duet_event_handler_t event_handler,
	void *privdata);
int duet_task_deregister(__u8 taskid);
int duet_is_online(void);

/*
 * For the following functions, start can be found from struct block_device
 * like so: bdev->bd_part->start_sect << 9
 */
int duet_chk_done(__u8 taskid, __u64 start, __u64 lbn, __u32 len);
int duet_chk_todo(__u8 taskid, __u64 start, __u64 lbn, __u32 len);
int duet_mark_done(__u8 taskid, __u64 start, __u64 lbn, __u32 len);
int duet_mark_todo(__u8 taskid, __u64 start, __u64 lbn, __u32 len);
int duet_print_rbt(__u8 taskid);

/* Hook-related functions */
void duet_hook(__u8 event_type, __u8 hook_type, void *hook_data);
void duet_bh_endio(struct buffer_head *bh, int uptodate);

#endif /* _DUET_H */
