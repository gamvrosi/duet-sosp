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

struct bmap_rbnode *bnode_init(struct duet_task *task, __u64 idx)
{
	struct bmap_rbnode *bnode = NULL;

#ifdef CONFIG_DUET_TREE_STATS
	(task->stat_bit_cur)++;
	if (task->stat_bit_cur > task->stat_bit_max) {
		task->stat_bit_max = task->stat_bit_cur;
		printk(KERN_INFO "duet: Task#%d (%s): %llu nodes in BitTree.\n"
			"      That's %llu bytes.\n", task->id, task->name,
			task->stat_bit_max,task->stat_bit_max * task->bmapsize);
	}
#endif /* CONFIG_DUET_TREE_STATS */
	
	bnode = kzalloc(sizeof(*bnode), GFP_ATOMIC);
	if (!bnode)
		return NULL;

	bnode->bmap = kzalloc(task->bmapsize, GFP_ATOMIC);
	if (!bnode->bmap) {
		kfree(bnode);
		return NULL;
	}

	RB_CLEAR_NODE(&bnode->node);
	bnode->idx = idx;
	return bnode;
}

void bnode_dispose(struct bmap_rbnode *bnode, struct rb_node *rbnode,
	struct rb_root *root)
{
	rb_erase(rbnode, root);
	kfree(bnode->bmap);
	kfree(bnode);
}

struct item_rbnode *tnode_init(struct duet_task *task, unsigned long ino,
	unsigned long idx, __u8 evt)
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
	tnode->item->evt = evt;
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

