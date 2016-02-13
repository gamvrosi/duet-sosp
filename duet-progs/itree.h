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
#ifndef _ITREE_H
#define _ITREE_H

#include "duet.h"
#include "rbtree.h"

/*
 * InodeTree structure. Two red-black trees, one sorted by the number of pages
 * in memory, the other sorted by inode number.
 */
struct inode_tree {
	struct rb_root sorted;
	struct rb_root inodes;
	struct duet_item buf[DUET_MAX_ITEMS];	/* buffer for fetched items */
};

/* InodeTree interface functions */
void itree_init(struct inode_tree *itree);
int itree_update(struct inode_tree *itree, __u8 taskid, int duet_fd);
int itree_fetch(struct inode_tree *itree, __u8 taskid, int duet_fd, char *path,
	unsigned long long *uuid, long long *inmem);
void itree_teardown(struct inode_tree *itree);

#endif /* _ITREE_H */
