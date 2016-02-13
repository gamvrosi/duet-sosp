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

#include "common.h"

#define get_ratio(ret, n, d)	\
	do {			\
		ret = n * 100;	\
		do_div(ret, d);	\
	} while (0);

struct itree_node {
	struct rb_node		inodes_node;
	struct rb_node		sorted_node;
	unsigned long long	uuid;
	__u8			inmem;	/* % pages currently in memory */
};

static void print_itree(struct inode_tree *itree)
{
	unsigned long last_uuid;
	__u8 last_inmem;
	struct itree_node *cur, *tmp;

	last_uuid = last_inmem = 0;
	printk(KERN_INFO "itree: printing inodes tree\n");
	rbtree_postorder_for_each_entry_safe(cur, tmp, &itree->inodes, inodes_node) {
		printk(KERN_INFO "\tuuid %llu (ino %lu), mem %u, rch %p, lch %p, pclr %lu\n",
			cur->uuid, DUET_UUID_INO(cur->uuid), cur->inmem,
			cur->inodes_node.rb_right, cur->inodes_node.rb_left,
			cur->inodes_node.__rb_parent_color);
		if (last_uuid == cur->uuid && last_inmem == cur->inmem)
			break;
		last_uuid = cur->uuid;
		last_inmem = cur->inmem;
	}

	last_uuid = last_inmem = 0;
	printk(KERN_INFO "itree: printing sorted tree\n");
	rbtree_postorder_for_each_entry_safe(cur, tmp, &itree->sorted, sorted_node) {
		printk(KERN_INFO "\tuuid %llu (ino %lu), mem %u, rch %p, lch %p, pclr %lu\n",
			cur->uuid, DUET_UUID_INO(cur->uuid), cur->inmem,
			cur->sorted_node.rb_right, cur->sorted_node.rb_left,
			cur->sorted_node.__rb_parent_color);
		if (last_uuid == cur->uuid && last_inmem == cur->inmem)
			break;
		last_uuid = cur->uuid;
		last_inmem = cur->inmem;
	}
}

void itree_init(struct inode_tree *itree)
{
	itree->sorted = RB_ROOT;
	itree->inodes = RB_ROOT;
}
EXPORT_SYMBOL_GPL(itree_init);

static struct itree_node *itnode_init(unsigned long long uuid, __u8 inmem)
{
	struct itree_node *itnode = NULL;

	itnode = kmalloc(sizeof(*itnode), GFP_KERNEL);
	if (!itnode)
		return NULL;

	RB_CLEAR_NODE(&itnode->inodes_node);
	RB_CLEAR_NODE(&itnode->sorted_node);
	itnode->uuid = uuid;
	itnode->inmem = inmem;
	return itnode;
}

/* Removes an inode from the inode trees. Returns 1 if node was not found. */
static int remove_itree_one(struct inode_tree *itree, unsigned long long uuid)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;
	struct itree_node *cur;

	/* Find through the inodes tree */
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

	if (!found)
		return 1;

	duet_dbg(KERN_ERR "itree: removing uuid %llu (ino %lu)\n",
		uuid, DUET_UUID_INO(uuid));

	/* Remove from both the inodes and sorted tree */
	rb_erase(&cur->inodes_node, &itree->inodes);
	rb_erase(&cur->sorted_node, &itree->sorted);
	kfree(cur);

	return 0;
}

/*
 * Updates inode tree. First, looks into the inodes tree for the inmem count,
 * and updates the tree. Then, it repeated the process for the sorted tree,
 * using the inmem count. Returns 1 on failure.
 */
static int update_itree_one(struct inode_tree *itree, unsigned long long uuid,
	__u8 inmem)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;
	struct itree_node *cur, *itnode;

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

	duet_dbg(KERN_DEBUG "itree: %s inode tree: (u%llu,r%u) => (u%llu,r%u)\n",
		found ? "updating" : "inserting", found ? cur->uuid : 0,
		found ? cur->inmem : 0, uuid, inmem);

	if (found) {
		/* Ensure we're not already up-to-date */
		if (cur->inmem == inmem)
			return 0;

		/* Remove the old node from the sorted tree; we'll reinsert */
		rb_erase(&cur->sorted_node, &itree->sorted);
		RB_CLEAR_NODE(&cur->sorted_node);
		cur->inmem = inmem;
		itnode = cur;
	} else {
		/* Create the node */
		itnode = itnode_init(uuid, inmem);
		if (!itnode) {
			printk(KERN_ERR "itree: itnode alloc failed\n");
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

	duet_dbg(KERN_DEBUG "itree: %s sorted tree: (u%llu,r%u) => (u%llu,r%u)\n",
		found ? "updating" : "inserting", found ? cur->uuid : 0,
		found ? cur->inmem : 0, itnode->uuid, itnode->inmem);

	if (found) {
		printk(KERN_ERR "itree: node (u%llu,r%u) already in sorted "
				"itree (bug!)\n", itnode->uuid, itnode->inmem);
		print_itree(itree);
		rb_erase(&itnode->inodes_node, &itree->inodes);
		kfree(itnode);
		return 1;
	} else {
		rb_link_node(&itnode->sorted_node, parent, link);
		rb_insert_color(&itnode->sorted_node, &itree->sorted);
	}

	return 0;
}

/* Process all pending page events for given task, and update the inode tree */
int itree_update(struct inode_tree *itree, __u8 taskid,
	itree_get_inode_t *itree_get_inode, void *ctx)
{
	__u16 itret = 1;
	struct inode *inode;
	struct duet_item itm;
	unsigned long inmem_pages, total_pages, inmem_ratio;
	unsigned long last_uuid = 0;
	__u8 last_inmem = 0;

	while (1) {
		if (duet_fetch(taskid, &itm, &itret)) {
			printk(KERN_ERR "itree: duet_fetch failed\n");
			return 1;
		}

		/* If there were no events, we're done */
		if (!itret)
			break;

		/*
		 * We only process ADDED and REMOVED events,
		 * and skip over processed inodes.
		 */
		if (!(itm.state & (DUET_PAGE_ADDED | DUET_PAGE_REMOVED)) ||
		    duet_check_done(taskid, itm.uuid, 1) == 1)
			continue;

		/* Get the inode. Should return 1 if the inode is no longer
		 * in the cache, and -1 on any other error. */
		if (itree_get_inode(ctx, DUET_UUID_INO(itm.uuid), &inode)) {
			duet_dbg(KERN_DEBUG "itree: inode not in cache\n");
			remove_itree_one(itree, itm.uuid);
			continue;
		}

		/*
		 * Calculate the current inmem ratio, and the updated one.
		 * Instead of worrying about ADD/REM, just get up-to-date counts
		 */
		if (!i_size_read(inode))
			goto next;
		total_pages = ((i_size_read(inode) - 1) >> PAGE_SHIFT) + 1;
		inmem_pages = inode->i_mapping->nrpages;
		get_ratio(inmem_ratio, inmem_pages, total_pages);

		duet_dbg(KERN_DEBUG "itree: uuid=%llu (ino %lu) total=%lu, inmem=%lu, ratio=%lu\n",
			itm.uuid, DUET_UUID_INO(itm.uuid), total_pages,
			inmem_pages, inmem_ratio);

		if (last_uuid != itm.uuid || (last_uuid == itm.uuid &&
		    last_inmem != inmem_ratio)) {
			if (inmem_ratio &&
			    update_itree_one(itree, itm.uuid, inmem_ratio)) {
				printk(KERN_ERR "itree: failed to update itree node\n");
				iput(inode);
				return 1;
			} else if (!inmem_ratio) {
				remove_itree_one(itree, itm.uuid);
			}
		}

		last_uuid = itm.uuid;
		last_inmem = inmem_ratio;
next:
		iput(inode);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(itree_update);

/*
 * Get the first inode from the sorted tree, then remove from both. Use
 * itree_get_inode function to retrieve the inode. Returns 1 if any
 * errors occurred, otherwise the inode is returned with its refcount
 * updated.
 */
int itree_fetch(struct inode_tree *itree, __u8 taskid, struct inode **inode,
	itree_get_inode_t *itree_get_inode, void *ctx)
{
	int ret = 0;
	struct rb_node *rbnode;
	struct itree_node *itnode;
	unsigned long long uuid;

	*inode = NULL;

again:
	if (RB_EMPTY_ROOT(&itree->sorted))
		return 0;

	/* Grab last node in the sorted tree, and remove from both trees */
	rbnode = rb_last(&itree->sorted);
	itnode = rb_entry(rbnode, struct itree_node, sorted_node);
	rb_erase(&itnode->sorted_node, &itree->sorted);
	rb_erase(&itnode->inodes_node, &itree->inodes);

	uuid = itnode->uuid;
	kfree(itnode);

	duet_dbg(KERN_ERR "itree: fetch uuid %llu, ino %lu\n", uuid,
		DUET_UUID_INO(uuid));

	/* Check if we've processed it before */
	if (duet_check_done(taskid, uuid, 1) == 1)
		goto again;

	printk(KERN_ERR "itree: fetching uuid %llu, ino %lu\n", uuid,
		DUET_UUID_INO(uuid));

	/* Get the actual inode. Should return 1 if the inode is no longer
	 * in the cache, and -1 on any other error. */
	if (itree_get_inode(ctx, DUET_UUID_INO(uuid), inode)) {
		printk(KERN_ERR "itree: inode not found\n");
		ret = 1;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(itree_fetch);

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
		kfree(itnode);
	}
}
EXPORT_SYMBOL_GPL(itree_teardown);
