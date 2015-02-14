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

#if 0
#define get_ratio(ret, n, d)	\
	do {			\
		ret = n * 100;	\
		do_div(ret, d);	\
	} while (0);

/*
 * Inserts a node in an ItemTree of inodes. Assumes relevant locks have been
 * obtained. Returns 1 on failure.
 */
static int duet_item_insert_inode(struct duet_task *task, struct duet_item *itm)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;
	struct duet_item *cur;

	link = &task->itmtree.rb_node;

	while (*link) {
		parent = *link;
		cur = rb_entry(parent, struct duet_item, node);

		/* We order based on (inmem_ratio, inode) */
		if (cur->inmem > itm->inmem) {
			link = &(*link)->rb_left;
		} else if (cur->inmem < itm->inmem) {
			link = &(*link)->rb_right;
		} else {
			/* Found inmem_ratio, look for btrfs_ino */
			if (cur->ino > itm->ino) {
				link = &(*link)->rb_left;
			} else if (cur->ino < itm->ino) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	duet_dbg(KERN_DEBUG "duet: %s insert inode (ino%lu, r%u)\n",
		found ? "won't" : "will", itm->ino, itm->inmem);

	if (found)
		goto out;

	/* Insert node in tree */
	rb_link_node(&itm->node, parent, link);
	rb_insert_color(&itm->node, &task->itmtree);

out:
	return found;
}

/*
 * This handles ADD, MOD and REM events for an inode tree. Indexing is based on
 * the ratio of pages in memory, and the inode number as seen by VFS.
 */
static void duet_handle_inode(struct duet_task *task, __u8 evtcode, __u64 idx,
	struct page *page)
{
	int found = 0;
	struct rb_node *node = NULL;
	struct duet_item *itm = NULL;
	struct inode *inode = page->mapping->host;
	u8 cur_inmem_ratio, new_inmem_ratio;
	u64 inmem_pages, total_pages;

	if (!(evtcode & (DUET_EVT_ADD | DUET_EVT_REM | DUET_EVT_MOD))) {
		printk(KERN_ERR "duet: evtcode %d in duet_handle_inode\n",
			evtcode);
		return;
	}

	/* There's nothing we need to do for MOD events */
	if (evtcode == DUET_EVT_MOD)
		return;

	/* Verify that the event refers to the fs we're interested in */
	if (task->sb && task->sb != inode->i_sb) {
		duet_dbg(KERN_INFO "duet: event not on fs of interest\n");
		return;
	}

	/* Verify that we have not seen this inode again */
	if (duet_check(task->id, idx, 1) == 1)
		return;

	/* Calculate the current inmem ratio, and the updated one */
	total_pages = ((i_size_read(inode) - 1) >> PAGE_SHIFT) + 1;
	inmem_pages = page->mapping->nrpages;
	if (evtcode == DUET_EVT_ADD)
		inmem_pages--;

	get_ratio(cur_inmem_ratio, inmem_pages, total_pages);
	get_ratio(new_inmem_ratio,
		inmem_pages + (evtcode == DUET_EVT_ADD ? 1 : -1), total_pages);

	/* First, look up the node in the ItemTree */
	//spin_lock_irq(&task->itm_outer_lock);
	spin_lock_irq(&task->itm_lock);
	node = task->itmtree.rb_node;

	while (node) {
		itm = rb_entry(node, struct duet_item, node);

		/* We order based on (inmem_ratio, inode) */
		if (itm->inmem > cur_inmem_ratio) {
			node = node->rb_left;
		} else if (itm->inmem < cur_inmem_ratio) {
			node = node->rb_right;
		} else {
			/* Found inmem_ratio, look for ino */
			if (itm->ino > inode->i_ino) {
				node = node->rb_left;
			} else if (itm->ino < inode->i_ino) {
				node = node->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	duet_dbg(KERN_DEBUG "d: %s node (ino%lu, r%u)\n",
		found ? "found" : "didn't find", found ? itm->ino : inode->i_ino,
		found ? itm->inmem : cur_inmem_ratio);

	/* If we found it, update it. If not, insert. */
	if (!found && evtcode == DUET_EVT_ADD) {
		itm = duet_item_init(task, inode->i_ino, new_inmem_ratio,
				(void *)inode);
		if (!itm) {
			printk(KERN_ERR "duet: itnode alloc failed\n");
			goto out;
		}

		if (duet_item_insert_inode(task, itm)) {
			printk(KERN_ERR "duet: insert failed\n");
			duet_dispose_item(itm);
		}
	} else if (found && new_inmem_ratio != cur_inmem_ratio) {
		/* Update the itnode */
		itm->inmem = new_inmem_ratio;

		/* Did the number of pages reach zero? Then remove */
		if (!itm->inmem) {
			rb_erase(node, &task->itmtree);
			duet_dispose_item(itm);
			goto out;
		}

		/* The ratio has changed, so erase and reinsert */
		rb_erase(node, &task->itmtree);
		if (duet_item_insert_inode(task, itm)) {
			printk(KERN_ERR "duet: insert failed\n");
			duet_dispose_item(itm);
		}
	}

out:
	spin_unlock_irq(&task->itm_lock);
	//spin_unlock_irq(&task->itm_outer_lock);
}
#endif
