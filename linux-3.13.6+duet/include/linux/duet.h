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

#include <linux/rbtree.h>

#ifdef CONFIG_DUET_DEBUG
#define duet_dbg(...)	printk(__VA_ARGS__)
#else
#define duet_dbg(...)
#endif

/*
 * Duet can be either state- and/or event-based.
 * Event-based Duet monitors events that have happened on a page, which include
 * all events in the lifetime of a cache page: ADDED, REMOVED, DIRTY, FLUSHED.
 * Add and remove events are triggered when a page __descriptor__ is inserted or
 * removed from the page cache. Modification events are triggered when the page
 * is dirtied (nb: during writes, pages are added, then dirtied), and flush
 * events are triggered when a page is marked for writeback.
 * State-based Duet monitors changes in the page cache. Registering for EXISTS
 * events means that fetch will be returning ADDED or REMOVED events if the
 * state of the page changes since the last fetch (i.e. the two events cancel
 * each other out). Registering for MODIFIED events means that fetch will be
 * returning DIRTY or FLUSHED events if the state of the page changes since the
 * last fetch.
 */
#define DUET_PAGE_ADDED		0x0001
#define DUET_PAGE_REMOVED	0x0002
#define DUET_PAGE_DIRTY		0x0004
#define DUET_PAGE_FLUSHED	0x0008
#define DUET_PAGE_MODIFIED	0x0010
#define DUET_PAGE_EXISTS	0x0020

#define DUET_IN_ACCESS		0x0040
#define DUET_IN_ATTRIB		0x0080
#define DUET_IN_WCLOSE		0x0100
#define DUET_IN_RCLOSE		0x0200
#define DUET_IN_CREATE		0x0400
#define DUET_IN_DELETE		0x0800
#define DUET_IN_MODIFY		0x1000
#define DUET_IN_MOVED		0x2000
#define DUET_IN_OPEN		0x4000

/* Used only for page state */
#define DUET_MASK_VALID		0x8000

/* Used only during registration */
#define DUET_REG_SBLOCK		0x8000
#define DUET_FILE_TASK		0x10000	/* we register a 32-bit flag due to this */

/* Some macros, to make our lives easier */
#define DUET_IN_EVENTS		(DUET_IN_ACCESS | DUET_IN_ATTRIB | DUET_IN_WCLOSE | \
							 DUET_IN_RCLOSE | DUET_IN_CREATE | DUET_IN_DELETE | \
							 DUET_IN_MODIFY | DUET_IN_MOVED | DUET_IN_OPEN)

/* Some structures to communicate file events to Duet */
struct duet_move_data {
	struct inode *target;
	struct inode *old_dir;
	struct inode *new_dir;
};

/*
 * Item struct returned for processing. We return 6 bits. For state-based duet,
 * we mark a page if it EXISTS or is MODIFIED. For event-based duet, we mark a
 * page for page addition, removal, dirtying, and flushing. The acceptable
 * combinations, however, will differ based on what the task has subscribed for.
 */
struct duet_item {
	unsigned long ino;
	unsigned long idx;
	__u16 state;
};

/*
 * InodeTree structure. Two red-black trees, one sorted by the number of pages
 * in memory, the other sorted by inode number.
 */
struct inode_tree {
	struct rb_root sorted;
	struct rb_root inodes;
};

/* Framework interface functions */
int duet_register(char *path, __u32 regmask, __u32 bitrange, const char *name,
		  __u8 *taskid);
int duet_deregister(__u8 taskid);
int duet_fetch(__u8 taskid, struct duet_item *items, __u16 *count);
int duet_check_done(__u8 taskid, __u64 idx, __u32 count);
int duet_set_done(__u8 taskid, __u64 idx, __u32 count);
int duet_unset_done(__u8 taskid, __u64 idx, __u32 count);
int duet_online(void);

/* Framework debugging functions */
int duet_print_bitmap(__u8 taskid);
int duet_print_events(__u8 taskid);

/* Hook functions that trasmit event info to the framework core */
typedef void (duet_hook_t) (__u16, void *);
void duet_hook(__u16 evtcode, void *data);
extern duet_hook_t *duet_hook_fp;

/* InodeTree interface functions */
typedef int (itree_get_inode_t)(void *, unsigned long, struct inode **);
void itree_init(struct inode_tree *itree);
int itree_update(struct inode_tree *itree, __u8 taskid,
	itree_get_inode_t *itree_get_inode, void *ctx);
int itree_fetch(struct inode_tree *itree, __u8 taskid, struct inode **inode,
	itree_get_inode_t *itree_get_inode, void *ctx);
void itree_teardown(struct inode_tree *itree);

#endif /* _DUET_H */
