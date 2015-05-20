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

/*
 * The following two functions are wrappers for the basic bitmap functions.
 * The wrappers translate an arbitrary range of numbers to the range and
 * granularity represented in the bitmap.
 * A bitmap is characterized by a starting offset (bstart), and a granularity
 * per bit (bgran).
 */

/* Sets (or clears) bits in [start, start+len) */
static int duet_bmap_set(unsigned long *bmap, __u64 bstart, __u32 bgran,
	__u64 start, __u32 len, __u8 do_set)
{
	__u64 bofft = start - bstart;
	__u32 blen = len;

	/* Convert range to bitmap granularity */
	do_div(bofft, bgran);
	if (do_div(blen, bgran))
		blen++;

	if (bofft + blen >= (bstart + (DUET_BITS_PER_NODE * bgran)))
		return -1;

	if (do_set)
		bitmap_set(bmap, (unsigned int)bofft, (int)blen);
	else
		bitmap_clear(bmap, (unsigned int)bofft, (int)blen);

	return 0;
}

/* Checks whether *all* bits in [start, start+len) are set (or cleared) */
static int duet_bmap_chk(unsigned long *bmap, __u64 bstart, __u32 bgran,
	__u64 start, __u32 len, __u8 do_set)
{
	__u64 bofft64 = start - bstart;
	__u32 blen32 = len;
	int bits_to_chk, blen;
	unsigned long *p;
	unsigned long mask_to_chk;
	unsigned int size;
	unsigned int bofft;

	/* Convert range to bitmap granularity */
	do_div(bofft64, bgran);
	if (do_div(blen32, bgran))
		blen32++;

	if (bofft64 + blen32 >= (bstart + (DUET_BITS_PER_NODE * bgran)))
		return -1;

	/* Now it is safe to cast these variables */
	bofft = (unsigned int)bofft64;
	blen = (int)blen32;

	/* Check the bits */
	p = bmap + BIT_WORD(bofft);
	size = bofft + blen;
	bits_to_chk = BITS_PER_LONG - (bofft % BITS_PER_LONG);
	mask_to_chk = BITMAP_FIRST_WORD_MASK(bofft);

	while (blen - bits_to_chk >= 0) {
		if (do_set && !(((*p) & mask_to_chk) == mask_to_chk))
			return 0;
		else if (!do_set && !((~(*p) & mask_to_chk) == mask_to_chk))
			return 0;

		blen -= bits_to_chk;
		bits_to_chk = BITS_PER_LONG;
		mask_to_chk = ~0UL;
		p++;
	}

	if (blen) {
		mask_to_chk &= BITMAP_LAST_WORD_MASK(size);
		if (do_set && !(((*p) & mask_to_chk) == mask_to_chk))
			return 0;
		else if (!do_set && !((~(*p) & mask_to_chk) == mask_to_chk))
			return 0;
	}

	return 1;
}

/* Initializes a bitmap tree node */
static struct bmap_rbnode *bnode_init(__u64 idx, struct duet_task *task)
{
	struct bmap_rbnode *bnode = NULL;

#ifdef CONFIG_DUET_STATS
	if (task) {
		(task->stat_bit_cur)++;
		if (task->stat_bit_cur > task->stat_bit_max) {
			task->stat_bit_max = task->stat_bit_cur;
			printk(KERN_INFO "duet: Task#%d (%s): %llu nodes in BitTree.\n"
				"      That's %llu bytes.\n", task->id, task->name,
				task->stat_bit_max, task->stat_bit_max * DUET_BITS_PER_NODE / 8);
		}
	}
#endif /* CONFIG_DUET_STATS */
	
	bnode = kmalloc(sizeof(*bnode), GFP_NOWAIT);
	if (!bnode)
		return NULL;

	bnode->bmap = kzalloc(sizeof(unsigned long) *
			BITS_TO_LONGS(DUET_BITS_PER_NODE), GFP_NOWAIT);
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
 * Looks through the bitmap tree for a node with the given LBN. Starting from
 * that node, it sets (or clears, or checks) all LBNs for the given len,
 * spilling over to subsequent nodes and inserting/removing them if needed.
 *
 * If chk is set then:
 * - a return value of 1 means all relevant LBNs were found set/cleared
 * - a return value of 0 means some of the LBNs were not found set/cleared
 * - a return value of -1 denotes an error occurred
 * If chk is not set then:
 * - a return value of 0 means all LBNs were marked properly
 * - a return value of -1 denotes an error occurred
 */
static int bittree_chkupd(struct rb_root *bittree, __u32 range, __u64 lbn,
	__u32 len, __u8 set, __u8 chk, struct duet_task *task)
{
	int ret, found;
	__u64 cur_lbn, node_lbn, lbn_gran, cur_len, rlbn, div_rem;
	__u32 rem_len;
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
	lbn_gran = range * DUET_BITS_PER_NODE;
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
				bnode = bnode_init(node_lbn, task);
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
			if (!chk && duet_bmap_set(bnode->bmap, bnode->idx,
				range, cur_lbn, cur_len, 1)) {
				/* We got -1, something went wrong */
				ret = -1;
				goto done;
			/* Check the bits */
			} else if (chk) {
				ret = duet_bmap_chk(bnode->bmap, bnode->idx,
						range, cur_lbn, cur_len, 1);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

		} else if (found) {
			/* Clear the bits */
			if (!chk && duet_bmap_set(bnode->bmap, bnode->idx,
				range, cur_lbn, cur_len, 0)) {
				/* We got -1, something went wrong */
				ret = -1;
				goto done;
			/* Check the bits */
			} else if (chk) {
				ret = duet_bmap_chk(bnode->bmap, bnode->idx,
						range, cur_lbn, cur_len, 0);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

			/* Dispose of the node if empty */
			if (!chk && bitmap_empty(bnode->bmap, DUET_BITS_PER_NODE))
				bnode_dispose(bnode, parent, bittree, task);
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

inline int bittree_check(struct rb_root *bittree, __u32 range, __u64 idx,
	__u32 num, struct duet_task *task)
{
	return bittree_chkupd(bittree, range, idx, num, 1, 1, task);
}

inline int bittree_mark(struct rb_root *bittree, __u32 range, __u64 idx,
	__u32 num, struct duet_task *task)
{
	return bittree_chkupd(bittree, range, idx, num, 1, 0, task);
}

inline int bittree_unmark(struct rb_root *bittree, __u32 range, __u64 idx,
	__u32 num, struct duet_task *task)
{
	return bittree_chkupd(bittree, range, idx, num, 0, 0, task);
}

int bittree_print(struct duet_task *task)
{
	struct bmap_rbnode *bnode = NULL;
	struct rb_node *node;
	size_t bits_on;

	spin_lock(&task->bittree_lock);
	printk(KERN_INFO "duet: Printing BitTree for task #%d\n", task->id);
	node = rb_first(&task->bittree);
	while (node) {
		bnode = rb_entry(node, struct bmap_rbnode, node);

		/* Print node information */
		printk(KERN_INFO "duet: Node key = %llu\n", bnode->idx);
		bits_on = bitmap_weight(bnode->bmap, DUET_BITS_PER_NODE);
		printk(KERN_INFO "duet:   Bits set: %zu out of %d\n", bits_on,
			DUET_BITS_PER_NODE);

		node = rb_next(node);
	}
	spin_unlock(&task->bittree_lock);

	return 0;
}
