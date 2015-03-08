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
#include <stdio.h>
//#include <unistd.h>
//#include <string.h>
#include <stdlib.h>
//#include <errno.h>
#include "duet.h"

#define ITREE_DEBUG

#ifdef ITREE_DEBUG
#define itree_dbg(...)	fprintf(stderr, __VA_ARGS__)
#else
#define itree_dbg(...)
#endif

struct itree_node {
	struct rb_node	inodes_node;
	struct rb_node	sorted_node;
	unsigned long	ino;
	unsigned long	inmem;	/* pages currently in memory */
};

static void print_itree(struct inode_tree *itree)
{
	struct itree_node *cur, *tmp;

	fprintf(stdout, "itree: printing inodes tree\n");
	rbtree_postorder_for_each_entry_safe(cur, tmp, &itree->inodes, inodes_node)
		fprintf(stdout, "\tinode %lu: %lu pages in memory\n", cur->ino, cur->inmem);

	fprintf(stdout, "itree: printing sorted tree\n");
	rbtree_postorder_for_each_entry_safe(cur, tmp, &itree->sorted, sorted_node)
		fprintf(stdout, "\tinode %lu: %lu pages in memory\n", cur->ino, cur->inmem);
}

void itree_init(struct inode_tree *itree)
{
	itree->sorted = RB_ROOT;
	itree->inodes = RB_ROOT;
}

static struct itree_node *itnode_init(unsigned long ino, unsigned long inmem)
{
	struct itree_node *itnode = NULL;

	itnode = calloc(1, sizeof(*itnode));
	if (!itnode)
		return NULL;

	RB_CLEAR_NODE(&itnode->inodes_node);
	RB_CLEAR_NODE(&itnode->sorted_node);
	itnode->ino = ino;
	itnode->inmem = inmem;
	return itnode;
}

/*
 * Updates inode tree. First, looks into the inodes tree for the inmem count,
 * and updates the tree. Then, it repeated the process for the sorted tree,
 * using the inmem count. Returns 1 on failure.
 */
static int update_itree_one(struct inode_tree *itree, unsigned long ino,
	unsigned long count)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;
	struct itree_node *cur, *itnode;

	if (!count) {
		fprintf(stderr, "itree: update_itree_one go zero count\n");
		return 1;
	}

	/* First go through the inodes tree */
	link = &itree->inodes.rb_node;
	while (*link) {
		parent = *link;
		cur = rb_entry(parent, struct itree_node, inodes_node);

		/* We order based on inode number alone */
		if (cur->ino > ino) {
			link = &(*link)->rb_left;
		} else if (cur->ino < ino) {
			link = &(*link)->rb_right;
		} else {
			found = 1;
			break;
		}
	}

	fprintf(stderr, "itree: %s inode tree: (i%lu,p%lu) => (i%lu,p%lu)\n",
		found ? "updating" : "inserting", found ? cur->ino : 0,
		found ? cur->inmem : 0, ino, count); //DEBUG

	if (found) {
		/* Remove the old node from the sorted tree */
		rb_erase(&cur->sorted_node, &itree->sorted);
		RB_CLEAR_NODE(&cur->sorted_node);
		cur->inmem += count;

		/* If inode is gone, delete from inode tree too */
		if (!cur->inmem) {
			rb_erase(&cur->inodes_node, &itree->inodes);
			itree_dbg("itree: removed inode %lu\n", ino);
			free(cur);
			return 0;
		}

		/* Prepare to reinsert to sorted tree */
		itnode = cur;
	} else {
		/* Create the node */
		itnode = itnode_init(ino, count);
		if (!itnode) {
			fprintf(stderr, "itree: itnode alloc failed\n");
			return 1;
		}

		/* Insert node in inode tree, then in sorted tree */
		rb_link_node(&itnode->inodes_node, parent, link);
		rb_insert_color(&itnode->inodes_node, &itree->inodes);
	}

	/* Now go through the sorted tree to (re)insert itnode */
	found = 0;
	parent = NULL;
	link = &itree->sorted.rb_node;
	while (*link) {
		parent = *link;
		cur = rb_entry(parent, struct itree_node, sorted_node);

		/* We order based on the inmem pages, then the inode number */
		if (cur->inmem > itnode->inmem) {
			link = &(*link)->rb_left;
		} else if (cur->inmem < itnode->inmem) {
			link = &(*link)->rb_right;
		} else {
			if (cur->ino > itnode->ino) {
				link = &(*link)->rb_left;
			} else if (cur->ino < itnode->ino) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	fprintf(stderr, "itree: %s sorted tree: (i%lu,p%lu) => (i%lu,p%lu)\n",
		found ? "updating" : "inserting", found ? cur->ino : 0,
		found ? cur->inmem : 0, itnode->ino, itnode->inmem); //DEBUG

	if (found) {
		fprintf(stderr, "itree: node (i%lu,p%lu) already in sorted "
				"itree (bug!)\n", itnode->ino, itnode->inmem);
		print_itree(itree);
		rb_erase(&itnode->inodes_node, &itree->inodes);
		free(itnode);
		return 1;
	} else {
		rb_link_node(&itnode->sorted_node, parent, link);
		rb_insert_color(&itnode->sorted_node, &itree->sorted);
	}

	return 0;
}

/* Process all pending page events for given task, and update the inode tree */
int itree_update(struct inode_tree *itree, __u8 taskid)
{
	int i, itret, ret=0;
	struct duet_item *itm;
	unsigned long count, last_ino = 0;

	itm = calloc(MAX_ITEMS, sizeof(struct duet_item));
	if (!itm) {
		fprintf(stderr, "itree: failed to allocate item buffer\n");
		return 1;
	}

again:
	if (duet_fetch(taskid, MAX_ITEMS, itm, &itret)) {
		fprintf(stderr, "itree: duet_fetch failed\n");
		ret = 1;
		goto out;
	}

	/* If there were no events, we're done */
	if (!itret)
		goto out;

	count = 0;
	for (i = 0; i < itret; i++) {
		/* We only process add/remove events, skip processed inodes */
		if (!(itm[i].state & (DUET_PAGE_ADDED | DUET_PAGE_REMOVED)) ||
		    duet_check(taskid, itm[i].ino, 1) == 1)
			continue;

		itree_dbg("itree: ino=%lu, evt=%s\n", itm[i].ino,
			itm[i].state & DUET_PAGE_ADDED ? "ADD" : "REM");

		/* Update the trees */
		if (last_ino != itm[i].ino) {
			if (last_ino && count &&
			    update_itree_one(itree, last_ino, count)) {
				fprintf(stderr, "itree: failed to update itree node\n");
				ret = 1;
				goto out;
			}
			count = 0;
		}

		count += (itm[i].state & DUET_PAGE_ADDED ? 1 : -1);
		last_ino = itm[i].ino;
	}

	if (count && update_itree_one(itree, last_ino, count)) {
		fprintf(stderr, "itree: failed to update itree node\n");
		ret = 1;
		goto out;
	}

	/* There might be more events to fetch */
	if (itret == MAX_ITEMS)
		goto again;
out:
	free(itm);
	return ret;
}

/*
 * Get the first inode from the sorted tree, then remove from both. Use
 * itree_get_inode function to retrieve the inode. Returns 1 if any
 * errors occurred, otherwise the inode is returned with its refcount
 * updated.
 */
int itree_fetch(struct inode_tree *itree, __u8 taskid, char *path)
{
	int ret = 0;
	struct rb_node *rbnode;
	struct itree_node *itnode;
	unsigned long ino;

again:
	if (RB_EMPTY_ROOT(&itree->sorted))
		return 0;

	/* Grab last node in the sorted tree, and remove from both trees */
	rbnode = rb_last(&itree->sorted);
	itnode = rb_entry(rbnode, struct itree_node, sorted_node);
	rb_erase(&itnode->sorted_node, &itree->sorted);
	rb_erase(&itnode->inodes_node, &itree->inodes);

	ino = itnode->ino;
	free(itnode);

	itree_dbg("itree: fetch picked inode %lu\n", ino);

	/* Check if we've processed it before */
	if (duet_check(taskid, ino, 1) == 1)
		goto again;

	itree_dbg("itree: fetching inode %lu\n", ino);

	/* Get the path for this inode */
	if (duet_getpath(taskid, ino, path)) {
		fprintf(stderr, "itree: inode path not found\n");
		ret = 1;
	}

	return ret;
}

void itree_teardown(struct inode_tree *itree)
{
	struct rb_node *rbnode;
	struct itree_node *itnode;

	while (!RB_EMPTY_ROOT(&itree->inodes)) {
		rbnode = rb_first(&itree->inodes);
		rb_erase(rbnode, &itree->inodes);
	}

	while (!RB_EMPTY_ROOT(&itree->sorted)) {
		rbnode = rb_first(&itree->sorted);
		itnode = rb_entry(rbnode, struct itree_node, sorted_node);
		rb_erase(rbnode, &itree->sorted);
		free(itnode);
	}
}
