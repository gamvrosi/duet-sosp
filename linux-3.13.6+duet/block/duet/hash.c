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
/*
 * TODO: Implement bulk update for multiple tasks on one item
 */

#include <linux/mm.h>
#include <linux/bootmem.h>
#include <linux/hash.h>
#include "common.h"

static unsigned long hash(unsigned long ino, unsigned long idx)
{
	unsigned long tmp;

	tmp = (idx * ino ^ (GOLDEN_RATIO_PRIME + idx)) / L1_CACHE_BYTES;
	tmp = tmp ^ ((tmp ^ GOLDEN_RATIO_PRIME) >> duet_env.itm_hash_shift);
	return tmp & duet_env.itm_hash_mask;
}

int hash_init(void)
{
	duet_env.itm_hash_table = alloc_large_system_hash("Page event table",
					sizeof(struct hlist_bl_head), 0,
					ilog2(totalram_pages), 0,
					&duet_env.itm_hash_shift,
					&duet_env.itm_hash_mask, 0, 0);
	if (!duet_env.itm_hash_table)
		return 1;

	memset(duet_env.itm_hash_table, 0,
		sizeof(struct hlist_bl_head) << duet_env.itm_hash_shift);
	return 0;
}
 
/* Add one event into the hash table */
int hash_add(struct duet_task *task, unsigned long ino, unsigned long idx,
	__u8 evtmask, short replace)
{
	short found = 0;
	unsigned long bnum;
	struct hlist_bl_head *b;
	struct hlist_bl_node *n, *t;
	struct item_hnode *itnode;

	evtmask &= task->evtmask;

	/* Get the bucket */
	bnum = hash(ino, idx);
	b = duet_env.itm_hash_table + bnum;
	hlist_bl_lock(b);

	/* Lookup the item in the bucket */
	hlist_bl_for_each_entry_safe(itnode, n, t, b, node) {
#ifdef CONFIG_DUET_STATS
		duet_env.itm_stat_lkp++;
#endif /* CONFIG_DUET_STATS */
		if ((itnode->item).ino == ino && (itnode->item).idx == idx) {
			found = 1;
			break;
		}
	}

#ifdef CONFIG_DUET_STATS
	duet_env.itm_stat_num++;
#endif /* CONFIG_DUET_STATS */
	duet_dbg(KERN_DEBUG "duet: %s hash node (ino%lu, idx%lu)\n",
		found ? (replace ? "replacing" : "updating") : "inserting",
		ino, idx);

	if (found) {
		/* Avoid up'ing refcount if we're just updating the mask */
		if (!(itnode->state[task->id] & DUET_MASK_VALID))
			itnode->refcount++;

		if (replace || !(itnode->state[task->id] & DUET_MASK_VALID)) {
			itnode->state[task->id] = evtmask | DUET_MASK_VALID;
			goto check_dispose;
		}

		itnode->state[task->id] |= evtmask | DUET_MASK_VALID;

		/* Negate previous events and remove if needed */
		if (task->evtmask & DUET_PAGE_EXISTS) {
			if ((itnode->state[task->id] & DUET_PAGE_ADDED) &&
			    (itnode->state[task->id] & DUET_PAGE_REMOVED))
				itnode->state[task->id] &= ~(DUET_PAGE_ADDED |
							DUET_PAGE_REMOVED);
		}

		if (task->evtmask & DUET_PAGE_MODIFIED) {
			if ((itnode->state[task->id] & DUET_PAGE_DIRTY) &&
			    (itnode->state[task->id] & DUET_PAGE_FLUSHED))
				itnode->state[task->id] &= ~(DUET_PAGE_DIRTY |
							DUET_PAGE_FLUSHED);
		}

check_dispose:
		if ((itnode->state[task->id] == DUET_MASK_VALID) &&
		    (itnode->refcount == 1)) {
			hlist_bl_del(&itnode->node);
			kfree(itnode);
		}
	} else if (!found) {
		if (!evtmask)
			goto done;

		itnode = kzalloc(sizeof(struct hlist_bl_node), GFP_ATOMIC);
		if (!itnode) {
			printk(KERN_ERR "duet: failed to allocate hash node\n");
			return 1;
		}

		(itnode->item).ino = ino;
		(itnode->item).idx = idx;
		itnode->state[task->id] = evtmask | DUET_MASK_VALID;
		itnode->refcount++;

		hlist_bl_add_head(&itnode->node, b);
	}

	/* Update bitmap (in any case) */
	spin_lock(&task->bbmap_lock);
	set_bit(bnum, task->bucket_bmap);
	spin_unlock(&task->bbmap_lock);
done:
	hlist_bl_unlock(b);
	return 0;
}

/* Fetch one item for a given task. Return found (1), empty (0), error (-1) */
int hash_fetch(struct duet_task *task, struct duet_item *itm)
{
	int found = 0;
	unsigned long bnum;
	struct hlist_bl_head *b;
	struct hlist_bl_node *n, *t;
	struct item_hnode *itnode;

	bnum = find_first_bit(task->bucket_bmap, 1U << duet_env.itm_hash_shift);
	if (bnum == (1U << duet_env.itm_hash_shift))
		return found;
	b = duet_env.itm_hash_table + bnum;

	/* Grab first item from bucket */
	hlist_bl_lock(b);
	if (!b->first) {
		printk(KERN_ERR "duet: empty hash bucket marked in bitmap\n");
		hlist_bl_unlock(b);
		return -1;
	}

#ifdef CONFIG_DUET_STATS
	duet_env.itm_stat_lkp++;
#endif /* CONFIG_DUET_STATS */
	hlist_bl_for_each_entry_safe(itnode, n, t, b, node) {
		if (itnode->state[task->id] & DUET_MASK_VALID) {
			*itm = itnode->item;
			itm->state = itnode->state[task->id] & (~DUET_MASK_VALID);

			itnode->refcount--;
			/* Free or update node */
			if (!itnode->refcount) {
				hlist_bl_del(n);
				kfree(itnode);
			} else {
				itnode->state[task->id] = 0;
			}

			found = 1;
			break;
		}
#ifdef CONFIG_DUET_STATS
		duet_env.itm_stat_lkp++;
#endif /* CONFIG_DUET_STATS */
	}

#ifdef CONFIG_DUET_STATS
	duet_env.itm_stat_num++;
#endif /* CONFIG_DUET_STATS */

	/* Update bitmap (if necessary) */
	if (!b->first) {
		spin_lock(&task->bbmap_lock);
		clear_bit(bnum, task->bucket_bmap);
		spin_unlock(&task->bbmap_lock);
	}

	hlist_bl_unlock(b);
	return found;
}

/* Warning: expensive printing function. Use with care. */
void hash_print(struct duet_task *task)
{
	unsigned long loop, count, start, end, buckets;
	unsigned long long nodes, tnodes;
	struct hlist_bl_head *b;
	struct hlist_bl_node *n;
	struct item_hnode *itnode;

	count = (1U << duet_env.itm_hash_shift) / 100;
	tnodes = nodes = buckets = start = end = 0;
	printk(KERN_INFO "duet: Printing hash table in 100 buckets"
			" (%lu real buckets each)\n", count);
	for (loop = 0; loop < (1U << duet_env.itm_hash_shift); loop++) {
		if (loop - start >= count) {
			printk(KERN_INFO "duet:   Buckets %lu - %lu: %llu nodes (task: %llu)\n",
				start, end, nodes, tnodes);
			start = end = loop;
			nodes = tnodes = 0;
		}

		/* Count bucket nodes */
		b = duet_env.itm_hash_table + loop;
		hlist_bl_lock(b);
		hlist_bl_for_each_entry(itnode, n, b, node) {
			nodes++;
			if (itnode->state[task->id] & DUET_MASK_VALID)
				tnodes++;
		}
		hlist_bl_unlock(b);

		end = loop;
	}

	if (start != loop - 1)
		printk(KERN_INFO "duet:   Buckets %lu - %lu: %llu nodes (task: %llu)\n",
			start, end, nodes, tnodes);

#ifdef CONFIG_DUET_STATS
	printk(KERN_INFO "duet: %lu (%lu/%lu) lookups per request on average\n",
		duet_env.itm_stat_lkp / duet_env.itm_stat_num,
		duet_env.itm_stat_lkp, duet_env.itm_stat_num);
#endif /* CONFIG_DUET_STATS */
}
