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
#include <stdlib.h>
#include "duet.h"
#include "itree.h"

//#define ITREE_DEBUG

#ifdef ITREE_DEBUG
#define itree_dbg(...)	fprintf(stderr, __VA_ARGS__)
#else
#define itree_dbg(...)
#endif

struct itree_node {
	struct rb_node		inodes_node;
	struct rb_node		sorted_node;
	unsigned long long	uuid;
	long long		inmem;	/* % pages currently in memory */
};

static void print_itree(struct inode_tree *itree)
{
	struct itree_node *cur, *tmp;

	fprintf(stdout, "itree: printing inodes tree\n");
	rbtree_postorder_for_each_entry_safe(cur, tmp, &itree->inodes, inodes_node)
		fprintf(stdout, "\tuuid %lld, inode %ld: %lld pages in memory\n",
			cur->uuid, DUET_UUID_INO(cur->uuid), cur->inmem);

	fprintf(stdout, "itree: printing sorted tree\n");
	rbtree_postorder_for_each_entry_safe(cur, tmp, &itree->sorted, sorted_node)
		fprintf(stdout, "\tuuid %lld, inode %ld: %lld pages in memory\n",
			cur->uuid, DUET_UUID_INO(cur->uuid), cur->inmem);
}

void itree_init(struct inode_tree *itree)
{
	itree->sorted = RB_ROOT;
	itree->inodes = RB_ROOT;
}

static struct itree_node *itnode_init(unsigned long long uuid, unsigned long inmem)
{
	struct itree_node *itnode = NULL;

	itnode = calloc(1, sizeof(*itnode));
	if (!itnode)
		return NULL;

	RB_CLEAR_NODE(&itnode->inodes_node);
	RB_CLEAR_NODE(&itnode->sorted_node);
	itnode->uuid = uuid;
	itnode->inmem = inmem;
	return itnode;
}

/*
 * Updates inode tree. First, looks into the inodes tree for the inmem count,
 * and updates the tree. Then, it repeated the process for the sorted tree,
 * using the inmem count. Returns 1 on failure.
 */
static int update_itree_one(struct inode_tree *itree, unsigned long long uuid,
	long long count)
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
		if (cur->uuid > uuid) {
			link = &(*link)->rb_left;
		} else if (cur->uuid < uuid) {
			link = &(*link)->rb_right;
		} else {
			found = 1;
			break;
		}
	}

	itree_dbg("itree: %s inode tree: (u%llu,p%lu) => (u%llu,p%lu)\n",
		found ? "updating" : "inserting", found ? cur->uuid : 0,
		found ? cur->inmem : 0, uuid, count);

	if (found) {
		/* Remove the old node from the sorted tree */
		rb_erase(&cur->sorted_node, &itree->sorted);
		RB_CLEAR_NODE(&cur->sorted_node);
		cur->inmem += count;

		/* If inode is gone, delete from inode tree too */
		if (!cur->inmem) {
			rb_erase(&cur->inodes_node, &itree->inodes);
			itree_dbg("itree: removed uuid %llu, inode %lu\n", uuid,
				DUET_UUID_INO(uuid));
			free(cur);
			return 0;
		}

		/* Prepare to reinsert to sorted tree */
		itnode = cur;
	} else {
		/* Create the node */
		itnode = itnode_init(uuid, count);
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
			if (cur->uuid > itnode->uuid) {
				link = &(*link)->rb_left;
			} else if (cur->uuid < itnode->uuid) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	itree_dbg("itree: %s sorted tree: (u%llu,p%lu) => (u%llu,p%lu)\n",
		found ? "updating" : "inserting", found ? cur->uuid : 0,
		found ? cur->inmem : 0, itnode->uuid, itnode->inmem);

	if (found) {
		fprintf(stderr, "itree: node (u%lld,p%lld) already in sorted "
				"itree (bug!)\n", itnode->uuid, itnode->inmem);
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
int itree_update(struct inode_tree *itree, __u8 taskid, int duet_fd)
{
	int i, itret, ret=0, last_was_processed = 0;
	long long count;
	unsigned long long last_uuid = 0;
	struct duet_item *buf = itree->buf;

again:
	itret = DUET_MAX_ITEMS;
	if (duet_fetch(duet_fd, taskid, buf, &itret)) {
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
		if (!(buf[i].state & (DUET_PAGE_ADDED | DUET_PAGE_REMOVED)))
			continue;

		itree_dbg("itree: uuid=%llu, ino=%lu, evt=%s\n", itm[i].uuid,
			DUET_UUID_INO(itm[i].uuid),
			itm[i].state & DUET_PAGE_ADDED ? "ADD" : "REM");

		/* Update the trees */
		if (last_uuid != buf[i].uuid) {
			if (last_uuid && count && !last_was_processed &&
			    update_itree_one(itree, last_uuid, count)) {
				fprintf(stderr, "itree: failed to update itree node\n");
				ret = 1;
				goto out;
			}
			last_was_processed = 0; //duet_check(duet_fd, taskid, buf[i].ino, 1);
			last_uuid = buf[i].uuid;
			count = 0;
		}

		count += (buf[i].state & DUET_PAGE_ADDED ? 1 : -1);
	}

	if (count && !last_was_processed && update_itree_one(itree, last_uuid, count)) {
		fprintf(stderr, "itree: failed to update itree node\n");
		ret = 1;
		goto out;
	}

	/* There might be more events to fetch */
	if (itret == DUET_MAX_ITEMS)
		goto again;
out:
	return ret;
}

/*
 * Get the first inode from the sorted tree, then remove from both. Use
 * itree_get_inode function to retrieve the inode. Returns 1 if any
 * errors occurred, otherwise the inode is returned with its refcount
 * updated.
 */
int itree_fetch(struct inode_tree *itree, __u8 taskid, int duet_fd, char *path,
	unsigned long long *uuid, long long *inmem)
{
	int ret = 0;
	struct rb_node *rbnode;
	struct itree_node *itnode;

	*uuid = 0;
	path[0] = '\0';
again:
	if (RB_EMPTY_ROOT(&itree->sorted))
		return 0;

	/* Grab last node in the sorted tree, and remove from both trees */
	rbnode = rb_last(&itree->sorted);
	itnode = rb_entry(rbnode, struct itree_node, sorted_node);
	rb_erase(&itnode->sorted_node, &itree->sorted);
	rb_erase(&itnode->inodes_node, &itree->inodes);

	*uuid = itnode->uuid;
	*inmem = itnode->inmem;
	free(itnode);

	itree_dbg("itree: fetch picked uuid %llu, inode %lu\n", *uuid,
		DUET_UUID_INO(*uuid));

	/* Check if we've processed it before */
	if (duet_check_done(duet_fd, taskid, *uuid, 1) == 1)
		goto again;

	itree_dbg("itree: fetching uuid %llu, inode %lu\n", *uuid,
		DUET_UUID_INO(*uuid));

	/* Get the path for this inode */
	if (duet_get_path(duet_fd, taskid, *uuid, path)) {
		//fprintf(stderr, "itree: inode path not found\n");
		goto again;
	}

	/* If this isn't a child, mark to avoid, and retry */
	if (path[0] == '\0') {
		//duet_set_done(duet_fd, taskid, *uuid, 1);
		//itree_dbg("itree: marking uuid %llu, ino %lu for task %u to avoid\n",
		//	*uuid, DUET_UUID_INO(*uuid), taskid);
		goto again;
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
