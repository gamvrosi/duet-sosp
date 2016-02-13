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
#ifndef _DUET_H
#define _DUET_H

#include <asm/types.h>
#include <stddef.h>

#define DUET_MAX_ITEMS	512
#define DUET_MAX_PATH	1024
#define DUET_MAX_NAME	128

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

/* Used only during registration */
#define DUET_REG_SBLOCK		0x8000
#define DUET_FILE_TASK		0x10000	/* we register a 32-bit flag due to this */

#define DUET_UUID_INO(uuid)	((unsigned long)(uuid & 0xffffffff))
#define DUET_UUID_GEN(uuid)	((unsigned long)(uuid >> 32))

/*
 * Item struct returned for processing. For both state- and event- based duet,
 * we return 4 bits, for page addition, removal, dirtying, and flushing. The
 * acceptable combinations, however, will differ based on what the task has
 * subscribed for.
 */
struct duet_item {
	unsigned long long	uuid;
	unsigned long		idx;
	__u16			state;
};

int open_duet_dev(void);
void close_duet_dev(int duet_fd);

int duet_register(int duet_fd, const char *path, __u32 regmask, __u32 bitrange,
	const char *name, int *tid);
int duet_deregister(int duet_fd, int tid);
int duet_fetch(int duet_fd, int tid, struct duet_item *items, int *count);
int duet_check_done(int duet_fd, int tid, __u64 idx, __u32 count);
int duet_set_done(int duet_fd, int tid, __u64 idx, __u32 count);
int duet_unset_done(int duet_fd, int tid, __u64 idx, __u32 count);
int duet_get_path(int duet_fd, int tid, unsigned long long uuid, char *path);
int duet_debug_printbit(int duet_fd, int tid);
int duet_task_list(int duet_fd);

#endif /* _DUET_H */
