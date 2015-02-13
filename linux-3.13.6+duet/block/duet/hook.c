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
#include <linux/fs.h>
#include "common.h"

#define get_ratio(ret, n, d)	\
	do {			\
		ret = n * 100;	\
		do_div(ret, d);	\
	} while (0);

static struct duet_item *duet_item_init(struct duet_task *task, __u64 idx,
	__u8 evt, void *data)
{
	struct duet_item *itm = NULL;

#ifdef CONFIG_DUET_TREE_STATS
	(task->stat_itm_cur)++;
	if (task->stat_itm_cur > task->stat_itm_max) {
		task->stat_itm_max = task->stat_itm_cur;
		printk(KERN_INFO "duet: Task #%d (%s) has %llu nodes in its "
			"ItemTree\n", task->id, task->name, task->stat_itm_max);
	}
#endif /* CONFIG_DUET_TREE_STATS */
	
	itm = kzalloc(sizeof(*itm), GFP_ATOMIC);
	if (!itm)
		return NULL;

	switch (task->itmtype) {
	case DUET_ITEM_PAGE:
		RB_CLEAR_NODE(&itm->node);
		itm->ino = ((struct page *)data)->mapping->host->i_ino;
		itm->index = ((struct page *)data)->index;
		itm->evt = evt;
		break;

	case DUET_ITEM_INODE:
		RB_CLEAR_NODE(&itm->node);
		itm->ino = ((struct inode *)data)->i_ino;
		itm->inmem = evt;
		break;
	}

	return itm;
}

/* Properly disposes a node that's been removed from the item tree */
int duet_dispose_item(struct duet_item *itm)
{
	kfree(itm);
	return 0;
}
EXPORT_SYMBOL_GPL(duet_dispose_item);

/*
 * Fetches up to itreq items from the ItemTree. The number of items fetched is
 * given by itret. Items are checked against the bitmap, and discarded if they
 * have been marked; this is possible because an insertion could have happened
 * between the last fetch and the last mark.
 */
int duet_fetch(__u8 taskid, __u16 itreq, struct duet_item **items, __u16 *itret)
{
	struct rb_node *rbnode;
	struct duet_item *itm;
	struct duet_task *task = duet_find_task(taskid);

	if (!task) {
		printk(KERN_ERR "duet_fetch: invalid taskid (%d)\n", taskid);
		return 1;	
	}

	/*
	 * We'll either run out of items, or grab itreq items.
	 * We also skip the outer lock. Suck it interrupts.
	 */
	*itret = 0;

again:
	spin_lock_irq(&task->itm_lock);

	/* Grab first item from the tree */
	if (!RB_EMPTY_ROOT(&task->itmtree)) {
		rbnode = rb_first(&task->itmtree);
		itm = rb_entry(rbnode, struct duet_item, node);
		rb_erase(rbnode, &task->itmtree);
		spin_unlock_irq(&task->itm_lock);
	} else {
		spin_unlock_irq(&task->itm_lock);
		goto done;
	}

	if (task->itmtype == DUET_ITEM_INODE && duet_check(taskid, itm->ino, 1) == 1) {
		duet_dispose_item(itm);
	} else {
		items[*itret] = itm;
		(*itret)++;
	}

	if (*itret < itreq)
		goto again;

done:
	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return 0;
}
EXPORT_SYMBOL_GPL(duet_fetch);

/*
 * Inserts a node in an ItemTree of pages. Assumes the relevant locks have been
 * obtained. Returns 1 on failure.
 */
static int duet_item_insert_page(struct duet_task *task, struct duet_item *itm)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;
	struct duet_item *cur;

	link = &task->itmtree.rb_node;

	while (*link) {
		parent = *link;
		cur = rb_entry(parent, struct duet_item, node);

		/* We order based on (inode, page index) */
		if (cur->ino > itm->ino) {
			link = &(*link)->rb_left;
		} else if (cur->ino < itm->ino) {
			link = &(*link)->rb_right;
		} else {
			/* Found inode, look for index */
			if (cur->index > itm->index) {
				link = &(*link)->rb_left;
			} else if (cur->index < itm->index) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	duet_dbg(KERN_DEBUG "duet: %s page node (ino%lu, idx%lu, e%u)\n",
		found ? "will not insert" : "will insert",
		itm->ino, itm->index, itm->evt);

	if (found)
		goto out;

	/* Insert node in tree */
	rb_link_node(&itm->node, parent, link);
	rb_insert_color(&itm->node, &task->itmtree);

out:
	return found;
}

/*
 * This handles ADD, MOD, and REM events for a page tree. Indexing is based on
 * the inode number, and the index of the page within said inode.
 */
static void duet_handle_page(struct duet_task *task, __u8 evtcode, __u64 idx,
	struct page *page)
{
	int found = 0;
	struct rb_node *node = NULL;
	struct duet_item *itm = NULL;
	struct inode *inode = page->mapping->host;

	if (!(evtcode & (DUET_EVT_ADD | DUET_EVT_MOD | DUET_EVT_REM))) {
		printk(KERN_ERR "duet: evtcode %d in duet_handle_page\n",
			evtcode);
		return;
	}

	/* Verify that the event refers to the fs we're interested in */
	if (task->sb && task->sb != inode->i_sb) {
		duet_dbg(KERN_INFO "duet: event not on fs of interest\n");
		return;
	}

	/* First, look up the node in the ItemTree */
	//spin_lock_irq(&task->itm_outer_lock);
	spin_lock_irq(&task->itm_lock);
	node = task->itmtree.rb_node;

	while (node) {
		itm = rb_entry(node, struct duet_item, node);

		/* We order based on (inode, page index) */
		if (itm->ino > inode->i_ino) {
			node = node->rb_left;
		} else if (itm->ino < inode->i_ino) {
			node = node->rb_right;
		} else {
			/* Found inode, look for index */
			if (itm->index > page->index) {
				node = node->rb_left;
			} else if (itm->index < page->index) {
				node = node->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	duet_dbg(KERN_DEBUG "duet-page: %s node (#%lu, i%lu, e%u)\n",
		found ? "found" : "didn't find", found ? itm->ino : inode->i_ino,
		found ? itm->index : page->index, found ? itm->evt : evtcode);

	/* If we found it, we might have to remove it. Otherwise, insert. */
	if (!found) {
		itm = duet_item_init(task, inode->i_ino, evtcode, (void *)page);
		if (!itm) {
			printk(KERN_ERR "duet: itnode alloc failed\n");
			goto out;
		}

		if (duet_item_insert_page(task, itm)) {
			printk(KERN_ERR "duet: insert failed\n");
			duet_dispose_item(itm);
		}
	} else if (found) {
		itm->evt |= evtcode;

		/* If this a REM event and this page was listed first by an ADD,
		 * remove the node. The net result is zero. */
		if ((itm->evt & (DUET_EVT_ADD | DUET_EVT_REM)) ==
				(DUET_EVT_ADD | DUET_EVT_REM)) {
			rb_erase(node, &task->itmtree);
			duet_dispose_item(itm);
		}
	}

out:
	spin_unlock_irq(&task->itm_lock);
	//spin_unlock_irq(&task->itm_outer_lock);
}

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

void duet_hook(__u8 evtcode, void *data)
{
	__u64 idx;
	struct page *page = (struct page *)data;
	struct duet_task *cur;

	if (!duet_online())
		return;

	BUG_ON(!page);
	BUG_ON(!page->mapping);

	/* We're in RCU context so whatever happens, stay awake! */
	idx = (__u64)page->index << PAGE_SHIFT;

	duet_dbg(KERN_INFO "duet hook: evt %u, idx %llu, data %p\n",
		evtcode, idx, data);

	/* Look for tasks interested in this event type and invoke callbacks */
	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		if (cur->evtmask & evtcode) {
			/* Provided the task has the appropriate tree in place,
			 * call the proper handler for this type of event */
			switch (cur->itmtype) {
			case DUET_ITEM_INODE:
				duet_handle_inode(cur, evtcode, idx, page);
				break;
			case DUET_ITEM_PAGE:
				duet_handle_page(cur, evtcode, idx, page);
				break;
			}
		}	
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(duet_hook);
