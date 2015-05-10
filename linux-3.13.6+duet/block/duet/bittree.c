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
 * Generic function to (un)mark a bitmap. We need information on the bitmap,
 * and information on the byte range it represents, and the byte range that
 * needs to be (un)marked.
 * A return value of 0 implies success, while -1 implies failure.
 */
static int duet_bmap_set(unsigned long *bmap, __u64 first_byte, __u32 bitrange,
	__u64 req_byte, __u32 req_len, __u8 set)
{
	__u64 start;
	__u32 len;

	/* TODO: Examine to check if it would mark a block which is only
	 * partially included in the range */
	start = (req_byte - first_byte);
	do_div(start, bitrange);

	len = req_len;
	if (do_div(len, bitrange))
		len++;

	if (start + len >= (first_byte + (DUET_BITS_PER_NODE * bitrange)))
		return -1;

	if (set)
		bitmap_set(bmap, (unsigned int)start, (int)len);
	else
		bitmap_clear(bmap, (unsigned int)start, (int)len);

	return 0;
}

/* checks bits [start, start + num) for bmap, to ensure they match 'set' */
static int duet_bmap_chk_bits(unsigned long *bmap, unsigned int start, int len,
	__u8 set)
{
	unsigned long *p = bmap + BIT_WORD(start);
	const unsigned int size = start + len;
	int bits_to_chk = BITS_PER_LONG - (start % BITS_PER_LONG);
	unsigned long mask_to_chk = BITMAP_FIRST_WORD_MASK(start);

	while (len - bits_to_chk >= 0) {
		if (set && !(((*p) & mask_to_chk) == mask_to_chk))
			return 0;
		else if (!set && !((~(*p) & mask_to_chk) == mask_to_chk))
			return 0;
		len -= bits_to_chk;
		bits_to_chk = BITS_PER_LONG;
		mask_to_chk = ~0UL;
		p++;
	}
	if (len) {
		mask_to_chk &= BITMAP_LAST_WORD_MASK(size);
		if (set && !(((*p) & mask_to_chk) == mask_to_chk))
			return 0;
		else if (!set && !((~(*p) & mask_to_chk) == mask_to_chk))
			return 0;
	}

	return 1;
}

/* Returns 1, if all bytes in [req_bytes, req_bytes+req_bytelen) are set/reset,
 * 0 otherwise. If an error occurs, then -1 is returned. */
static int duet_bmap_chk(unsigned long *bmap, __u64 first_byte, __u32 bitrange,
	__u64 req_byte, __u32 req_len, __u8 set)
{
	__u64 start;
	__u32 len;

	start = (req_byte - first_byte);
	do_div(start, bitrange);

	len = req_len;
	if (do_div(len, bitrange))
		len++;

	if (start + len >= (first_byte + (DUET_BITS_PER_NODE * bitrange)))
		return -1;

	return duet_bmap_chk_bits(bmap, (unsigned int)start, (int)len, set);
}

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

	mutex_lock(&task->bittree_lock);
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
	mutex_unlock(&task->bittree_lock);

	return 0;
}
