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
 * Task-specific hook handling function. Should handle all hook codes
 * registered in the hook_mask. Arguments: __u8 taskid, __u8 hook_code,
 * __u64 lbn, __u32 len, void *private
 */
typedef void (duet_hook_handler_t) (__u8, __u8, __u64, __u32, void *);

/* Hook types */
enum {
	DUET_SETUP_HOOK_BA = 1,		/* async: submit_bio */
	DUET_SETUP_HOOK_BW_START,	/* sync: submit_bio_wait (before) */
	DUET_SETUP_HOOK_BW_END,		/* sync: submit_bio_wait (after) */
	DUET_SETUP_HOOK_BH,		/* async: submit_bh */
};

/* bio flag */
#define BIO_DUET        22      /* duet has seen this bio */

/* Hook codes */
#ifdef CONFIG_DUET_BTRFS
#define DUET_HOOK_BTRFS_READ	(1 << 0)
#define DUET_HOOK_BTRFS_WRITE	(1 << 1)
#endif /* CONFIG_DUET_BTRFS */

/* Core interface functions */
int duet_task_register(__u8 *taskid, const char *name, __u32 blksize,
	__u32 bmapsize, __u8 hook_mask, duet_hook_handler_t hook_handler);
int duet_task_deregister(__u8 taskid);

/* Hook-related functions */
void duet_hook(__u8 hook_code, __u8 hook_type, void *hook_data);

#endif /* _DUET_H */
