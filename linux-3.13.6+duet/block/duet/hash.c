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

#include <linux/mm.h>
#include <linux/bootmem.h>
#include <linux/hash.h>
#include "common.h"

#define DUET_NEGATE_EXISTS	(DUET_PAGE_ADDED | DUET_PAGE_REMOVED)
#define DUET_NEGATE_MODIFIED	(DUET_PAGE_DIRTY | DUET_PAGE_FLUSHED)

/*
 * Page state for Duet is retained in a global hash table shared by all tasks.
 * Indexing is based on inode number and the page's offset within said inode.
 */

static unsigned long hash(unsigned long ino, unsigned long idx)
{
	unsigned long tmp;

	tmp = (idx * ino ^ (GOLDEN_RATIO_PRIME + idx)) / L1_CACHE_BYTES;
	tmp = tmp ^ ((tmp ^ GOLDEN_RATIO_PRIME) >> duet_env.itm_hash_shift);
	return tmp & duet_env.itm_hash_mask;
}

int hash_init(void)
{
	/* Allocate power-of-2 number of buckets */
	duet_env.itm_hash_shift = ilog2(totalram_pages);
	duet_env.itm_hash_size = 1 << duet_env.itm_hash_shift;
	duet_env.itm_hash_mask = duet_env.itm_hash_size - 1;

	printk(KERN_DEBUG "duet: allocated global hash table (%lu buckets)\n",
			duet_env.itm_hash_size);
	duet_env.itm_hash_table = vmalloc(sizeof(struct hlist_bl_head) *
						duet_env.itm_hash_size);
	if (!duet_env.itm_hash_table)
		return 1;

	memset(duet_env.itm_hash_table, 0, sizeof(struct hlist_bl_head) *
						duet_env.itm_hash_size);
	return 0;
}

/* Add one event into the hash table */
int hash_add(struct duet_task *task, unsigned long ino, unsigned long idx,
	__u8 evtmask, short in_scan)
{
	__u8 curmask = 0;
	short found = 0;
	unsigned long bnum, flags;
	struct hlist_bl_head *b;
	struct hlist_bl_node *n;
	struct item_hnode *itnode;

	evtmask &= task->evtmask;

	/* Get the bucket */
	bnum = hash(ino, idx);
	b = duet_env.itm_hash_table + bnum;
	local_irq_save(flags);
	hlist_bl_lock(b);

	/* Lookup the item in the bucket */
	hlist_bl_for_each_entry(itnode, n, b, node) {
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
		found ? (in_scan ? "replacing" : "updating") : "inserting",
		ino, idx);

	if (found) {
		curmask = itnode->state[task->id];

		/* Only up the refcount if we are adding a new mask */
		if (!(curmask & DUET_MASK_VALID) || in_scan) {
			if (!in_scan)
				itnode->refcount++;
			curmask = evtmask | DUET_MASK_VALID;
			goto check_dispose;
		}

		curmask |= evtmask | DUET_MASK_VALID;

		/* Negate previous events and remove if needed */
		if ((task->evtmask & DUET_PAGE_EXISTS) &&
		   ((curmask & DUET_NEGATE_EXISTS) == DUET_NEGATE_EXISTS))
			curmask &= ~DUET_NEGATE_EXISTS;

		if ((task->evtmask & DUET_PAGE_MODIFIED) &&
		   ((curmask & DUET_NEGATE_MODIFIED) == DUET_NEGATE_MODIFIED))
			curmask &= ~DUET_NEGATE_MODIFIED;

check_dispose:
		if ((curmask == DUET_MASK_VALID) && (itnode->refcount == 1)) {
			if (itnode->refcount != 1) {
				itnode->state[task->id] = 0;
			} else {
				hlist_bl_del(&itnode->node);
				kfree(itnode);
			}

			/* Are we still interested in this bucket? */
			hlist_bl_for_each_entry(itnode, n, b, node) {
#ifdef CONFIG_DUET_STATS
				duet_env.itm_stat_lkp++;
#endif /* CONFIG_DUET_STATS */
				if (itnode->state[task->id] & DUET_MASK_VALID) {
					found = 1;
					break;
				}
			}

			if (!found) {
				spin_lock(&task->bbmap_lock);
				clear_bit(bnum, task->bucket_bmap);
				spin_unlock(&task->bbmap_lock);
			}
		} else {
			itnode->state[task->id] = curmask;

			/* Update bitmap */
			spin_lock(&task->bbmap_lock);
			set_bit(bnum, task->bucket_bmap);
			spin_unlock(&task->bbmap_lock);
		}
	} else if (!found) {
		if (!evtmask)
			goto done;

		itnode = kzalloc(sizeof(struct item_hnode), GFP_NOWAIT);
		if (!itnode) {
			printk(KERN_ERR "duet: failed to allocate hash node\n");
			return 1;
		}

		(itnode->item).ino = ino;
		(itnode->item).idx = idx;
		itnode->state[task->id] = evtmask | DUET_MASK_VALID;
		itnode->refcount++;

		hlist_bl_add_head(&itnode->node, b);

		/* Update bitmap */
		spin_lock(&task->bbmap_lock);
		set_bit(bnum, task->bucket_bmap);
		spin_unlock(&task->bbmap_lock);
	}

done:
	hlist_bl_unlock(b);
	local_irq_restore(flags);
	return 0;
}

/* Fetch one item for a given task. Return found (1), empty (0), error (-1) */
int hash_fetch(struct duet_task *task, struct duet_item *itm)
{
	int found;
	unsigned long bnum, flags;
	struct hlist_bl_head *b;
	struct hlist_bl_node *n;
	struct item_hnode *itnode;

	local_irq_save(flags);
again:
	spin_lock(&task->bbmap_lock);
	bnum = find_first_bit(task->bucket_bmap, duet_env.itm_hash_size);
	if (bnum == duet_env.itm_hash_size) {
		spin_unlock(&task->bbmap_lock);
		local_irq_restore(flags);
		return 1;
	}
	clear_bit(bnum, task->bucket_bmap);
	spin_unlock(&task->bbmap_lock);
	b = duet_env.itm_hash_table + bnum;

	/* Grab first item from bucket */
	hlist_bl_lock(b);
	if (!b->first) {
		printk(KERN_ERR "duet: empty hash bucket marked in bitmap\n");
		hlist_bl_unlock(b);
		goto again;
	}

	found = 0;
	hlist_bl_for_each_entry(itnode, n, b, node) {
#ifdef CONFIG_DUET_STATS
		duet_env.itm_stat_lkp++;
#endif /* CONFIG_DUET_STATS */
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
	}

	if (!found) {
		printk(KERN_ERR "duet: uninteresting bucket marked in bitmap\n");
		hlist_bl_unlock(b);
		goto again;
	}

	/* Are we still interested in this bucket? */
	found = 0;
	hlist_bl_for_each_entry(itnode, n, b, node) {
#ifdef CONFIG_DUET_STATS
		duet_env.itm_stat_lkp++;
#endif /* CONFIG_DUET_STATS */
		if (itnode->state[task->id] & DUET_MASK_VALID) {
			found = 1;
			break;
		}
	}

	if (found) {
		spin_lock(&task->bbmap_lock);
		set_bit(bnum, task->bucket_bmap);
		spin_unlock(&task->bbmap_lock);
	}

#ifdef CONFIG_DUET_STATS
	duet_env.itm_stat_num++;
#endif /* CONFIG_DUET_STATS */
	hlist_bl_unlock(b);
	local_irq_restore(flags);
	return 0;
}

/* Warning: expensive printing function. Use with care. */
void hash_print(struct duet_task *task)
{
	unsigned long loop, count, start, end, buckets, flags;
	unsigned long long nodes, tnodes;
	struct hlist_bl_head *b;
	struct hlist_bl_node *n;
	struct item_hnode *itnode;

	count = duet_env.itm_hash_size / 100;
	tnodes = nodes = buckets = start = end = 0;
	printk(KERN_INFO "duet: Printing hash table in 100 buckets"
			" (%lu real buckets each)\n", count);
	for (loop = 0; loop < duet_env.itm_hash_size; loop++) {
		if (loop - start >= count) {
			printk(KERN_INFO "duet:   Buckets %lu - %lu: %llu nodes (task: %llu)\n",
				start, end, nodes, tnodes);
			start = end = loop;
			nodes = tnodes = 0;
		}

		/* Count bucket nodes */
		b = duet_env.itm_hash_table + loop;
		local_irq_save(flags);
		hlist_bl_lock(b);
		hlist_bl_for_each_entry(itnode, n, b, node) {
			nodes++;
			if (itnode->state[task->id] & DUET_MASK_VALID)
				tnodes++;
		}
		hlist_bl_unlock(b);
		local_irq_restore(flags);

		end = loop;
	}

	if (start != loop - 1)
		printk(KERN_INFO "duet:   Buckets %lu - %lu: %llu nodes (task: %llu)\n",
			start, end, nodes, tnodes);

#ifdef CONFIG_DUET_STATS
	printk(KERN_INFO "duet: %lu (%lu/%lu) lookups per request on average\n",
		duet_env.itm_stat_num ? (duet_env.itm_stat_lkp / duet_env.itm_stat_num) : 0,
		duet_env.itm_stat_lkp, duet_env.itm_stat_num);
#endif /* CONFIG_DUET_STATS */
}
