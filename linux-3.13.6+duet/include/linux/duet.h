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

#ifdef CONFIG_DUET_DEBUG
#define duet_dbg(...)	printk(__VA_ARGS__)
#else
#define duet_dbg(...)
#endif

/*
 * Event types: ADD (resp. REM) are triggered when a page __descriptor__ is
 * inserted in (resp. about to be removed from) the page cache. MOD is triggered
 * when the page is dirtied (nb: during writes, pages are added, then dirtied).
 * Finally, FLS is triggered when a page is marked to be flushed.
 */
#define DUET_EVT_ADD	(1 << 0)
#define DUET_EVT_REM	(1 << 1)
#define DUET_EVT_MOD	(1 << 2)
#define DUET_EVT_FLS	(1 << 3)

/* Page states. Up-to-date is implied by absence. */
#define DUET_PAGE_ADD		(DUET_EVT_ADD)
#define DUET_PAGE_REM		(DUET_EVT_REM)
#define DUET_PAGE_MOD		(DUET_EVT_MOD)
#define DUET_PAGE_FLS		(DUET_EVT_FLS)
#define DUET_PAGE_ADD_MOD	(DUET_EVT_ADD | DUET_EVT_MOD)

/* Item struct returned for processing */
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
};

/* Framework interface functions */
int duet_register(__u8 *taskid, const char *name, __u8 evtmask, __u32 bitrange,
		  void *owner);
int duet_deregister(__u8 taskid);
int duet_online(void);
int duet_check(__u8 taskid, __u64 idx, __u32 num);
int duet_mark(__u8 taskid, __u64 idx, __u32 num);
int duet_unmark(__u8 taskid, __u64 idx, __u32 num);
int duet_fetch(__u8 taskid, __u16 req, struct duet_item *items, __u16 *ret);

/* Framework debugging functions */
int duet_print_bittree(__u8 taskid);
int duet_print_itmtree(__u8 taskid);

/* Hook functions that trasmit event info to the framework core */
typedef void (duet_hook_t) (__u8, void *);
void duet_hook(__u8 evtcode, void *data);

/* InodeTree interface functions */
typedef int (itree_get_inode_t)(void *, unsigned long, struct inode **);
void itree_init(struct inode_tree *itree);
int itree_update(struct inode_tree *itree, __u8 taskid,
	itree_get_inode_t *itree_get_inode, void *ctx);
int itree_fetch(struct inode_tree *itree, __u8 taskid, struct inode **inode,
	itree_get_inode_t *itree_get_inode, void *ctx);
void itree_teardown(struct inode_tree *itree);

#endif /* _DUET_H */
