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

#define DUET_BMAP_SET	(1<<0)
#define DUET_BMAP_CHK	(1<<1)

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

	if (bofft + blen >= (bstart + (DUET_BITS_PER_NODE * bgran)))
		return -1;

	/* Convert range to bitmap granularity */
	do_div(bofft, bgran);
	if (do_div(blen, bgran))
		blen++;

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

	if (bofft64 + blen32 >= (bstart + (DUET_BITS_PER_NODE * bgran)))
		return -1;

	/* Convert range to bitmap granularity */
	do_div(bofft64, bgran);
	if (do_div(blen32, bgran))
		blen32++;

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
static struct bmap_rbnode *bnode_init(struct duet_bittree *bittree, __u64 idx)
{
	struct bmap_rbnode *bnode = NULL;

#ifdef CONFIG_DUET_STATS
	if (bittree) {
		(bittree->statcur)++;
		if (bittree->statcur > bittree->statmax) {
			bittree->statmax = bittree->statcur;
			printk(KERN_INFO "duet: %llu nodes (%llu bytes) in BitTree.\n",
				bittree->statmax,
				bittree->statmax * DUET_BITS_PER_NODE / 8);
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

static void bnode_dispose(struct bmap_rbnode *bnode, struct rb_node *rbnode,
	struct duet_bittree *bittree)
{
#ifdef CONFIG_DUET_STATS
	if (bittree)
		(bittree->statcur)--;
#endif /* CONFIG_DUET_STATS */
	rb_erase(rbnode, &bittree->root);
	kfree(bnode->bmap);
	kfree(bnode);
}

/*
 * Looks through the bitmap nodes storing the given range. It then sets, clears,
 * or checks all bits. It further inserts/removes nodes as needed.
 *
 * If DUET_BMAP_CHK flag is set:
 * - a return value of 1 means the entire range was found set/cleared
 * - a return value of 0 means the entire range was not found set/cleared
 * - a return value of -1 denotes an error
 * If DUET_BMAP_CHK flag is not set:
 * - a return value of 0 means the entire range was updated properly
 * - a return value of -1 denotes an error
 */
static int __update_tree(struct duet_bittree *bittree, __u64 start, __u32 len,
	__u8 flags)
{
	int found, ret;
	__u64 node_offt, div_rem;
	__u32 node_len;
	struct rb_node **link, *parent;
	struct bmap_rbnode *bnode = NULL;
	unsigned long iflags;

	local_irq_save(iflags);
	spin_lock(&bittree->lock);
	duet_dbg(KERN_INFO "duet: %s%s: start %llu, len %u\n",
		(flags & DUET_BMAP_CHK) ? "checking if " : "marking as ",
		(flags & DUET_BMAP_SET) ? "set" : "cleared", start, len);

	div64_u64_rem(start, bittree->range * DUET_BITS_PER_NODE, &div_rem);
	node_offt = start - div_rem;

	while (len) {
		/* Look up BitTree node */
		found = 0;
		link = &(bittree->root).rb_node;
		parent = NULL;

		while (*link) {
			parent = *link;
			bnode = rb_entry(parent, struct bmap_rbnode, node);

			if (bnode->idx > node_offt) {
				link = &(*link)->rb_left;
			} else if (bnode->idx < node_offt) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}

		duet_dbg(KERN_DEBUG "duet: node starting at %llu %sfound\n",
			node_offt, found ? "" : "not ");

		/*
		 * Take appropriate action based on whether we found the node
		 * and whether we plan to update (SET), or only check it (CHK).
		 *
		 *   !CHK  |       Found            !Found      |
		 *  -------+------------------------------------+
		 *    SET  |     Set Bits     |  Init new node  |
		 *         |------------------+-----------------|
		 *   !SET  | Clear (dispose?) |     Nothing     |
		 *  -------+------------------------------------+
		 *
		 *    CHK  |       Found            !Found      |
		 *  -------+------------------------------------+
		 *    SET  |    Check Bits    |  Return false   |
		 *         |------------------+-----------------|
		 *   !SET  |    Check Bits    |    Continue     |
		 *  -------+------------------------------------+
		 */

		/* Trim len to this node */
		node_len = min(start + len, node_offt +
				(bittree->range * DUET_BITS_PER_NODE)) - start;

		if (flags & DUET_BMAP_SET) {
			if (!found && !(flags & DUET_BMAP_CHK)) {
				/* Insert the new node */
				bnode = bnode_init(bittree, node_offt);
				if (!bnode) {
					ret = -1;
					goto done;
				}

				rb_link_node(&bnode->node, parent, link);
				rb_insert_color(&bnode->node, &bittree->root);

			} else if (!found && (flags & DUET_BMAP_CHK)) {
				/* Looking for set bits, node didn't exist */
				ret = 0;
				goto done;
			}

			/* Set the bits */
			if (!(flags & DUET_BMAP_CHK) &&
			    duet_bmap_set(bnode->bmap, bnode->idx,
					  bittree->range, start, node_len, 1)) {
				/* Something went wrong */
				ret = -1;
				goto done;

			/* Check the bits */
			} else if (flags & DUET_BMAP_CHK) {
				ret = duet_bmap_chk(bnode->bmap, bnode->idx,
					bittree->range, start, node_len, 1);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

		} else if (found) {
			/* Clear the bits */
			if (!(flags & DUET_BMAP_CHK) &&
			    duet_bmap_set(bnode->bmap, bnode->idx,
					  bittree->range, start, node_len, 0)) {
				/* Something went wrong */
				ret = -1;
				goto done;

			/* Check the bits */
			} else if (flags & DUET_BMAP_CHK) {
				ret = duet_bmap_chk(bnode->bmap, bnode->idx,
					bittree->range, start, node_len, 0);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

			/* Dispose of the node if empty */
			if (!(flags & DUET_BMAP_CHK) &&
			    bitmap_empty(bnode->bmap, DUET_BITS_PER_NODE))
				bnode_dispose(bnode, parent, bittree);
		}

		len -= node_len;
		start += node_len;
		node_offt = start;
	}

	/* If we managed to get here, then everything worked as planned.
	 * Return 0 for success in the case that CHK is not set, or 1 for
	 * success when CHK is set. */
	if (!(flags & DUET_BMAP_CHK))
		ret = 0;
	else
		ret = 1;

done:
	if (ret == -1)
		printk(KERN_ERR "duet: blocks were not %s %s\n",
			(flags & DUET_BMAP_CHK) ? "found" : "marked",
			(flags & DUET_BMAP_SET) ? "set" : "unset");
	spin_unlock(&bittree->lock);
	local_irq_restore(iflags);
	return ret;
}

inline int bittree_check(struct duet_bittree *bittree, __u64 start, __u32 len)
{
	return __update_tree(bittree, start, len, DUET_BMAP_SET|DUET_BMAP_CHK);
}

inline int bittree_mark(struct duet_bittree *bittree, __u64 start, __u32 len)
{
	return __update_tree(bittree, start, len, DUET_BMAP_SET);
}

inline int bittree_unmark(struct duet_bittree *bittree, __u64 start, __u32 len)
{
	return __update_tree(bittree, start, len, 0);
}

int bittree_print(struct duet_task *task)
{
	struct bmap_rbnode *bnode = NULL;
	struct rb_node *node;
	unsigned long flags;

	spin_lock(&task->bittree.lock);
	printk(KERN_INFO "duet: Printing global hash table\n");
	node = rb_first(&task->bittree.root);
	while (node) {
		bnode = rb_entry(node, struct bmap_rbnode, node);

		/* Print node information */
		printk(KERN_INFO "duet: Node key = %llu\n", bnode->idx);
		printk(KERN_INFO "duet:   Bits set: %d out of %d\n",
			bitmap_weight(bnode->bmap, DUET_BITS_PER_NODE),
			DUET_BITS_PER_NODE);

		node = rb_next(node);
	}
	spin_unlock(&task->bittree.lock);

	local_irq_save(flags);
	spin_lock(&task->bbmap_lock);
	printk(KERN_INFO "duet: Task #%d bitmap has %d out of %lu bits set\n",
		task->id, bitmap_weight(task->bucket_bmap,
		duet_env.itm_hash_size), duet_env.itm_hash_size);
	spin_unlock(&task->bbmap_lock);
	local_irq_restore(flags);

	return 0;
}

void bittree_init(struct duet_bittree *bittree, __u32 range)
{
	bittree->range = range;
	spin_lock_init(&bittree->lock);
	bittree->root = RB_ROOT;
#ifdef CONFIG_DUET_STATS
	bittree->statcur = bittree->statmax = 0;
#endif /* CONFIG_DUET_STATS */
}

void bittree_destroy(struct duet_bittree *bittree)
{
	struct rb_node *rbnode;
	struct bmap_rbnode *bnode;

	while (!RB_EMPTY_ROOT(&bittree->root)) {
		rbnode = rb_first(&bittree->root);
		bnode = rb_entry(rbnode, struct bmap_rbnode, node);
		bnode_dispose(bnode, rbnode, bittree);
	}
}
