/*
 * Copyright (C) 2015 George Amvrosiadis.  All rights reserved.
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
#include <asm/types.h>
#include <stddef.h>
#include "rbtree.h"

#define MAX_ITEMS	256	/* From ioctl.h */

//#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#define container_of(ptr, type, member) ({				\
		const typeof( ((type *)0)->member  ) *__mptr = (ptr);	\
		(type *)( (char *)__mptr - offsetof(type,member)  ); })

//#define DUET_DEBUG
#ifdef DUET_DEBUG
#define duet_dbg(...) fprintf(stdout, __VA_ARGS__)
#else
#define duet_dbg(...)
#endif /* DUET_DEBUG */

/*
 * Duet can be either state- or event-based. State-based Duet monitors changes
 * in the page cache, specifically whether a page EXISTS and whether it has
 * been MODIFIED. Event-based Duet monitors events that have happened on a page,
 * which include all events in the lifetime of a cache page: ADDED, REMOVED,
 * DIRTY, FLUSHED.
 * Add and remove events are triggered when a page __descriptor__ is inserted or
 * removed from the page cache. Modification events are triggered when the page
 * is dirtied (nb: during writes, pages are added, then dirtied), and flush
 * events are triggered when a page is marked for writeback.
 */
#define DUET_PAGE_ADDED		(1 << 0)
#define DUET_PAGE_REMOVED	(1 << 1)
#define DUET_PAGE_DIRTY		(1 << 2)
#define DUET_PAGE_FLUSHED	(1 << 3)
#define DUET_EVENT_BASED	(1 << 4)
#define DUET_CACHE_STATE	(1 << 5)

#define DUET_PAGE_MODIFIED	(DUET_PAGE_DIRTY | DUET_PAGE_FLUSHED)
#define DUET_PAGE_EXISTS	(DUET_PAGE_ADDED | DUET_PAGE_REMOVED)
#define DUET_PAGE_EVENTS	(DUET_PAGE_MODIFIED | DUET_PAGE_EXISTS)

/*
 * Item struct returned for processing. For both state- and event- based duet,
 * we return 4 bits, for page addition, removal, dirtying, and flushing. The
 * acceptable combinations, however, will differ based on what the task has
 * subscribed for.
 */
struct duet_item {
	unsigned long ino;
	unsigned long idx;
	__u8 state;
};

/*
 * InodeTree structure. Two red-black trees, one sorted by the number of pages
 * in memory, the other sorted by inode number.
 */
struct inode_tree {
	struct rb_root sorted;
	struct rb_root inodes;

	/* scratch space for fetched items */
	struct duet_item itm[256];
};

/* InodeTree interface functions */
void itree_init(struct inode_tree *itree);
int itree_update(struct inode_tree *itree, __u8 taskid);
int itree_fetch(struct inode_tree *itree, __u8 taskid, char *path);
void itree_teardown(struct inode_tree *itree);

int duet_register(__u8 *tid, const char *name, __u32 bitrange, __u8 evtmask,
		const char *path);
int duet_deregister(int taskid);
int duet_fetch(int taskid, int itmreq, struct duet_item *items, int *num);
int duet_getpath(int taskid, unsigned long ino, char *path);
int duet_mark(int taskid, __u64 idx, __u32 num);
int duet_unmark(int taskid, __u64 idx, __u32 num);
int duet_check(int taskid, __u64 idx, __u32 num);
int duet_debug_printbit(int taskid);
