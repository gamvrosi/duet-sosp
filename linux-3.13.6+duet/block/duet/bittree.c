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

#ifdef CONFIG_DUET_STATS
	if (task) {
		(task->stat_bit_cur)++;
		if (task->stat_bit_cur > task->stat_bit_max) {
			task->stat_bit_max = task->stat_bit_cur;
			printk(KERN_INFO "duet: Task#%d (%s): %llu nodes in BitTree.\n"
				"      That's %llu bytes.\n", task->id, task->name,
				task->stat_bit_max,task->stat_bit_max * task->bmapsize);
		}
	}
#endif /* CONFIG_DUET_STATS */
	
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
#ifdef CONFIG_DUET_STATS
	if (task)
		(task->stat_bit_cur)--;
#endif /* CONFIG_DUET_STATS */
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

int bittree_print(struct duet_task *task)
{
	struct bmap_rbnode *bnode = NULL;
	struct rb_node *node;
	__u32 bits_on;

	spin_lock(&task->bittree_lock);
	printk(KERN_INFO "duet: Printing BitTree for task #%d\n", task->id);
	node = rb_first(&task->bittree);
	while (node) {
		bnode = rb_entry(node, struct bmap_rbnode, node);

		/* Print node information */
		printk(KERN_INFO "duet: Node key = %llu\n", bnode->idx);
		bits_on = duet_bmap_count(bnode->bmap, task->bmapsize);
		printk(KERN_INFO "duet:   Bits set: %u out of %u\n", bits_on,
			task->bmapsize * 8);

		node = rb_next(node);
	}
	spin_unlock(&task->bittree_lock);

	return 0;
}
