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

static struct bmap_rbnode *bnode_init(__u32 bmapsize, __u64 idx,
	struct duet_task *task)
{
	struct bmap_rbnode *bnode = NULL;

#ifdef CONFIG_DUET_TREE_STATS
	if (task) {
		(task->stat_bit_cur)++;
		if (task->stat_bit_cur > task->stat_bit_max) {
			task->stat_bit_max = task->stat_bit_cur;
			printk(KERN_INFO "duet: Task#%d (%s): %llu nodes in BitTree.\n"
				"      That's %llu bytes.\n", task->id, task->name,
				task->stat_bit_max,task->stat_bit_max * task->bmapsize);
		}
	}
#endif /* CONFIG_DUET_TREE_STATS */
	
	bnode = kzalloc(sizeof(*bnode), GFP_ATOMIC);
	if (!bnode)
		return NULL;

	bnode->bmap = kzalloc(bmapsize, GFP_ATOMIC);
	if (!bnode->bmap) {
		kfree(bnode);
		return NULL;
	}

	RB_CLEAR_NODE(&bnode->node);
	bnode->idx = idx;
	return bnode;
}

void bnode_dispose(struct bmap_rbnode *bnode, struct rb_node *rbnode,
	struct rb_root *root, struct duet_task *task)
{
#ifdef CONFIG_DUET_BMAP_STATS
	if (task)
		(task->stat_bit_cur)--;
#endif /* CONFIG_DUET_BMAP_STATS */
	rb_erase(rbnode, root);
	kfree(bnode->bmap);
	kfree(bnode);
}

/*
 * Looks through the bmaptree for the node that should be responsible for
 * the given LBN. Starting from that node, it marks all LBNs until lbn+len
 * (or unmarks them depending on the value of set, or just checks whether
 * they are set or not based on the value of chk), spilling over to
 * subsequent nodes and inserting them if needed.
 * If chk is set then:
 * - a return value of 1 means all relevant LBNs are marked as expected
 * - a return value of 0 means some of the LBNs were not marked as expected
 * - a return value of -1 denotes the occurrence of an error
 * If chk is not set then:
 * - a return value of 0 means all LBNs were marked properly
 * - a return value of -1 denotes the occurrence of an error
 */
static int bittree_chkupd(struct rb_root *bittree, __u32 range, __u32 bmapsize,
	__u64 lbn, __u32 len, __u8 set, __u8 chk, struct duet_task *task)
{
	int ret, found;
	__u64 cur_lbn, node_lbn, lbn_gran, cur_len, rlbn, div_rem;
	__u32 i, rem_len;
	struct rb_node **link, *parent;
	struct bmap_rbnode *bnode = NULL;
	__u8 id = 0;

	if (task)
		id = task->id;

	rlbn = lbn;

	duet_dbg(KERN_INFO "duet: task #%d %s%s: lbn%llu, len%u\n", id,
		chk ? "checking if " : "marking as ", set ? "set" : "cleared",
		rlbn, len);

	cur_lbn = rlbn;
	rem_len = len;
	lbn_gran = range * bmapsize * 8;
	div64_u64_rem(rlbn, lbn_gran, &div_rem);
	node_lbn = rlbn - div_rem;

	while (rem_len) {
		/* Look up node_lbn */
		found = 0;
		link = &bittree->rb_node;
		parent = NULL;

		while (*link) {
			parent = *link;
			bnode = rb_entry(parent, struct bmap_rbnode, node);

			if (bnode->idx > node_lbn) {
				link = &(*link)->rb_left;
			} else if (bnode->idx < node_lbn) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}

		duet_dbg(KERN_DEBUG "duet: node with LBN %llu %sfound\n",
			node_lbn, found ? "" : "not ");

		/*
		 * Take appropriate action based on whether we found the node
		 * (found), and whether we plan to mark or unmark (set), and
		 * whether we are only checking the values (chk).
		 *
		 *   !Chk  |       Found            !Found      |
		 *  -------+------------------------------------+
		 *    Set  |     Set Bits     |  Init new node  |
		 *         |------------------+-----------------|
		 *   !Set  | Clear (dispose?) |     Nothing     |
		 *  -------+------------------------------------+
		 *
		 *    Chk  |       Found            !Found      |
		 *  -------+------------------------------------+
		 *    Set  |    Check Bits    |  Return false   |
		 *         |------------------+-----------------|
		 *   !Set  |    Check Bits    |    Continue     |
		 *  -------+------------------------------------+
		 */

		/* Find the last LBN on this node */
		cur_len = min(cur_lbn + rem_len,
			node_lbn + lbn_gran) - cur_lbn;

		if (set) {
			if (!found && !chk) {
				/* Insert the new node */
				bnode = bnode_init(bmapsize, node_lbn, task);
				if (!bnode) {
					ret = -1;
					goto done;
				}

				rb_link_node(&bnode->node, parent, link);
				rb_insert_color(&bnode->node, bittree);
			} else if (!found && chk) {
				/* Looking for set bits, node didn't exist */
				ret = 0;
				goto done;
			}

			/* Set the bits */
			if (!chk && duet_bmap_set(bnode->bmap, bmapsize,
			    bnode->idx, range, cur_lbn, cur_len, 1)) {
				/* We got -1, something went wrong */
				ret = -1;
				goto done;
			/* Check the bits */
			} else if (chk) {
				ret = duet_bmap_chk(bnode->bmap, bmapsize,
					bnode->idx, range, cur_lbn, cur_len, 1);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

		} else if (found) {
			/* Clear the bits */
			if (!chk && duet_bmap_set(bnode->bmap, bmapsize,
			    bnode->idx, range, cur_lbn, cur_len, 0)) {
				/* We got -1, something went wrong */
				ret = -1;
				goto done;
			/* Check the bits */
			} else if (chk) {
				ret = duet_bmap_chk(bnode->bmap, bmapsize,
					bnode->idx, range, cur_lbn, cur_len, 0);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

			if (!chk) {
				/* Dispose of the node? */
				ret = 0;
				for (i=0; i<bmapsize; i++) {
					if (bnode->bmap[i]) {
						ret = 1;
						break;
					}
				}

				if (!ret)
					bnode_dispose(bnode, parent, bittree,
							task);
			}
		}

		rem_len -= cur_len;
		cur_lbn += cur_len;
		node_lbn = cur_lbn;
	}

	/* If we managed to get here, then everything worked as planned.
	 * Return 0 for success in the case that chk is not set, or 1 for
	 * success when chk is set. */
	if (!chk)
		ret = 0;
	else
		ret = 1;

done:
	if (ret == -1)
		printk(KERN_ERR "duet: blocks were not %s %s for task %d\n",
			chk ? "found" : "marked", set ? "set" : "unset", id);
	return ret;
}

inline int bittree_check(struct rb_root *bittree, __u32 range, __u32 bmapsize,
	__u64 idx, __u32 num, struct duet_task *task)
{
	return bittree_chkupd(bittree, range, bmapsize, idx, num, 1, 1, task);
}

inline int bittree_mark(struct rb_root *bittree, __u32 range, __u32 bmapsize,
	__u64 idx, __u32 num, struct duet_task *task)
{
	return bittree_chkupd(bittree, range, bmapsize, idx, num, 1, 0, task);
}

inline int bittree_unmark(struct rb_root *bittree, __u32 range, __u32 bmapsize,
	__u64 idx, __u32 num, struct duet_task *task)
{
	return bittree_chkupd(bittree, range, bmapsize, idx, num, 0, 0, task);
}

static struct item_rbnode *tnode_init(struct duet_task *task, unsigned long ino,
	unsigned long idx, __u8 state)
{
	struct item_rbnode *tnode = NULL;

#ifdef CONFIG_DUET_TREE_STATS
	(task->stat_itm_cur)++;
	if (task->stat_itm_cur > task->stat_itm_max) {
		task->stat_itm_max = task->stat_itm_cur;
		printk(KERN_INFO "duet: Task#%d (%s): %llu nodes in ItmTree.\n",
			task->id, task->name, task->stat_itm_max);
	}
#endif /* CONFIG_DUET_TREE_STATS */

	tnode = kzalloc(sizeof(*tnode), GFP_ATOMIC);
	if (!tnode)
		return NULL;

	tnode->item = kzalloc(sizeof(struct duet_item), GFP_ATOMIC);
	if (!tnode->item) {
		kfree(tnode);
		return NULL;
	}

	RB_CLEAR_NODE(&tnode->node);
	tnode->item->ino = ino;
	tnode->item->idx = idx;
	tnode->item->state = state;
	return tnode;
}

void tnode_dispose(struct item_rbnode *tnode, struct rb_node *rbnode,
	struct rb_root *root)
{
	if (rbnode && root)
		rb_erase(rbnode, root);
	kfree(tnode->item);
	kfree(tnode);
}

/*
 * Creates and inserts an item in the ItemTree. Assumes the relevant locks have
 * been obtained. Returns 1 on failure.
 */
int itmtree_insert(struct duet_task *task, unsigned long ino,
	unsigned long index, __u8 state, __u8 replace)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;
	struct item_rbnode *cur, *tnode;

	/* Create the node */
	tnode = tnode_init(task, ino, index, state);
	if (!tnode) {
		printk(KERN_ERR "duet: tnode alloc failed\n");
		return 1;
	}

	/* Find where to insert it */
	link = &task->itmtree.rb_node;
	while (*link) {
		parent = *link;
		cur = rb_entry(parent, struct item_rbnode, node);

		/* We order based on (inode, page index) */
		if (cur->item->ino > tnode->item->ino) {
			link = &(*link)->rb_left;
		} else if (cur->item->ino < tnode->item->ino) {
			link = &(*link)->rb_right;
		} else {
			/* Found inode, look for index */
			if (cur->item->idx > tnode->item->idx) {
				link = &(*link)->rb_left;
			} else if (cur->item->idx < tnode->item->idx) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	duet_dbg(KERN_DEBUG "duet: %s page node (ino%lu, idx%lu)\n",
		found ? "will not insert" : "will insert",
		tnode->item->ino, tnode->item->idx);

	if (found) {
		if (replace)
			cur->item->state = state;
		else
			tnode_dispose(tnode, NULL, NULL);
	} else {
		/* Insert node in tree */
		rb_link_node(&tnode->node, parent, link);
		rb_insert_color(&tnode->node, &task->itmtree);
	}

	return found;
}
