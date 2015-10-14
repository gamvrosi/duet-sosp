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

#define MAX_ITEMS	512	/* From ioctl.h */

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
#define DUET_PAGE_MODIFIED	(1 << 4)
#define DUET_PAGE_EXISTS	(1 << 5)

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

int open_duet_dev(void);
void close_duet_dev(int duet_fd);

int duet_register(int duet_fd, const char *path, __u8 evtmask, __u32 bitrange,
	const char *name, int *tid);
int duet_deregister(int duet_fd, int tid);
int duet_fetch(int duet_fd, int tid, struct duet_item *items, int *count);
int duet_check_done(int duet_fd, int tid, __u64 idx, __u32 count);
int duet_set_done(int duet_fd, int tid, __u64 idx, __u32 count);
int duet_unset_done(int duet_fd, int tid, __u64 idx, __u32 count);
int duet_get_path(int duet_fd, int tid, unsigned long ino, char *path);
int duet_debug_printbit(int duet_fd, int tid);
