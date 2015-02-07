/*
 * Copyright (C) 2014 George Amvrosiadis.  All rights reserved.
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
#include <linux/buffer_head.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include "common.h"

#define get_ratio(ret, n, d)	\
	do {			\
		ret = n * 100;	\
		do_div(ret, d);	\
	} while (0);

struct duet_bio_private {
	bio_end_io_t	*real_end_io;
	void 		*real_private;
	__u8		event_code;
	__u32		size;
};

static inline void mark_bio_seen(struct bio *bio)
{
	/* Mark this bio as seen by the duet framework */
	set_bit(BIO_DUET, &bio->bi_flags);
}

static struct item_rbnode *tnode_init(struct duet_task *task, __u64 itmidx,
	__u8 evt, void *data)
{
	struct item_rbnode *tnode = NULL;

#ifdef CONFIG_DUET_TREE_STATS
	(task->stat_itm_cur)++;
	if (task->stat_itm_cur > task->stat_itm_max) {
		task->stat_itm_max = task->stat_itm_cur;
		printk(KERN_INFO "duet: Task #%d (%s) has %llu nodes in its ItemTree.\n",
			task->id, task->name, task->stat_itm_max);
	}
#endif /* CONFIG_DUET_TREE_STATS */
	
	tnode = kzalloc(sizeof(*tnode), GFP_ATOMIC);
	if (!tnode)
		return NULL;

	RB_CLEAR_NODE(&tnode->node);
	tnode->idx = itmidx;

	if (task->itmtype == DUET_ITEM_BLOCK) {
		tnode->bio = (struct bio *)data;
		tnode->evt = evt;
	} else if (task->itmtype == DUET_ITEM_PAGE) {
		tnode->page = (struct page *)data;
		tnode->evt = evt;
	} else if (task->itmtype == DUET_ITEM_INODE) {
		tnode->inode = (struct inode *)data;
		tnode->inmem = evt;
	}

	return tnode;
}

/*
 * Inserts a node in the item tree. Assumes the relevant locks have been
 * obtained. Returns 1 on failure.
 */
static int tnode_insert(struct duet_task *task, struct item_rbnode *itn)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;
	struct item_rbnode *cur;

	link = &task->itmtree.rb_node;

	while (*link) {
		parent = *link;
		cur = rb_entry(parent, struct item_rbnode, node);

		/* We order based on index */
		if (cur->idx > itn->idx) {
			link = &(*link)->rb_left;
		} else if (cur->idx < itn->idx) {
			link = &(*link)->rb_right;
		} else {
			found = 1;
			break;
		}
	}

	duet_dbg(KERN_DEBUG "duet-defrag: %s node (#%llu, e%u, a%p)\n",
		found ? "will not insert" : "will insert", itn->idx,
		itn->evt, itn->bio);

	if (found)
		goto out;

	/* Insert node in RBtree */
	rb_link_node(&itn->node, parent, link);
	rb_insert_color(&itn->node, &task->itmtree);

out:
	return found;
}

/* This handles CACHE_INSERT, CACHE_MODIFY and CACHE_REMOVE */
static void duet_handle_inode(struct duet_task *task, __u8 evtcode, __u64 idx,
	struct page *page)
{
	int ret=0, found=0;
	struct rb_node *node = NULL;
	struct item_rbnode *itm = NULL;
	struct inode *inode = page->mapping->host;
	u8 cur_inmem_ratio, new_inmem_ratio;
	u64 inmem_pages, total_pages;

	if (!(evtcode & (DUET_EVT_CACHE_INSERT | DUET_EVT_CACHE_REMOVE |
					DUET_EVT_CACHE_MODIFY))) {
		printk(KERN_ERR "duet: evtcode %d in duet_handle_inode\n",
			evtcode);
		return;
	}

	/* Verify that the event refers to the fs we're interested in */
	if (task->sb != inode->i_sb) {
		duet_dbg(KERN_INFO "duet: event not on fs of interest\n");
		return;
	}

	/* Calculate the current inmem ratio, and the updated one */
	total_pages = ((i_size_read(inode) - 1) >> PAGE_CACHE_SHIFT) + 1;
	inmem_pages = page->mapping->nrpages;
	if (evtcode == DUET_EVT_CACHE_INSERT)
		inmem_pages--;

	get_ratio(cur_inmem_ratio, inmem_pages, total_pages);
	get_ratio(new_inmem_ratio,
		inmem_pages + (evtcode == DUET_EVT_CACHE_INSERT ? 1 : -1),
		total_pages);

	/* First, look up the inode number in the ItemTree */
	spin_lock(&task->itmtree_lock);
	node = task->itmtree.rb_node;

	while (node) {
		itm = rb_entry(node, struct item_rbnode, node);

		/* We order based on (inmem_ratio, inode) */
		if (itm->inmem > cur_inmem_ratio) {
			node = node->rb_left;
		} else if (itm->inmem < cur_inmem_ratio) {
			node = node->rb_right;
		} else {
			/* Found inmem_ratio, look for btrfs_ino */
			if (itm->idx > inode->i_ino) {
				node = node->rb_left;
			} else if (itm->idx < inode->i_ino) {
				node = node->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	defrag_dbg(KERN_DEBUG "d: %s node (#%llu, r%u, a%p)\n",
		found ? "found" : "didn't find",
		found ? itm->idx : inode->i_ino,
		found ? itm->inmem : cur_inmem_ratio,
		found ? itm->inode : inode);

	/* If we found it, update it. If not, insert. */
	if (!found && evtcode == DUET_EVT_CACHE_INSERT) {
		itn = kzalloc(sizeof(*itn), GFP_NOFS);
		if (!itn) {
			printk(KERN_ERR "duet-defrag: itnode alloc failed\n");
			ret = 1;
			goto out;
		}
		RB_CLEAR_NODE(&(itn->node));

		/* Update the itnode */
		itn->btrfs_ino = dwork->btrfs_ino;
		itn->inmem_pages = dwork->inmem_pages + 1;
		itn->total_pages = dwork->total_pages;
		itn->inmem_ratio = new_inmem_ratio;

		ret = insert_itnode(dwork, itn);
		if (ret) {
			printk(KERN_ERR "duet-defrag: insert failed\n");
			kfree(itn);
		}
	} else if (found) {
		/* Update the itnode */
		itn->btrfs_ino = dwork->btrfs_ino;
		itn->inmem_pages = dwork->inmem_pages +
			(dwork->event_code == DUET_EVENT_CACHE_INSERT ? 1 : -1);
		itn->total_pages = dwork->total_pages;
		itn->inmem_ratio = new_inmem_ratio;

		/* Did the number of pages reach zero? Then remove */
		if (itn->inmem_pages == 0) {
			rb_erase(node, &dwork->dctx->itree);
			kfree(itn);
			goto out;
		}

		/* If the ratio has changed, erase and reinsert */
		if (new_inmem_ratio != cur_inmem_ratio) {
			rb_erase(node, &dwork->dctx->itree);
			ret = insert_itnode(dwork, itn);
			if (ret) {
				printk(KERN_ERR "duet-defrag: insert failed\n");
				kfree(itn);
			}
		}
	}

}

/* This handles FS_READ and FS_WRITE events */
static void duet_handle_blk(struct duet_task *task, __u8 evtcode, __u64 lbn,
	__u32 len, struct bio *bio)
{
	__u64 curlbn;
	int ret=0, found=0;
	struct rb_node *node = NULL;
	struct item_rbnode *itm = NULL;

	if (!(evtcode & (DUET_EVT_FS_READ | DUET_EVT_FS_WRITE))) {
		printk(KERN_ERR "duet: evtcode %d in duet_handle_block\n",
			evtcode);
		return;
	}

	if (task->bitrange > len) {
		duet_dbg(KERN_INFO "duet: duet_handle_block too small (%u/%u)",
			len, task->bitrange);
		return;
	}

	/* Verify that the event refers to the device we're interested in */
	if (task->bdev != bio->bi_bdev->bd_contains) {
		duet_dbg(KERN_INFO "duet: event not in device of interest\n");
		return;
	}

	curlbn = lbn / task->bitrange;
	if (curlbn * task->bitrange < lbn)
		curlbn++;

again:
	/* First, look up the block number in the ItemTree */
	spin_lock(&task->itmtree_lock);
	node = task->itmtree.rb_node;

	while (node) {
		itm = rb_entry(node, struct item_rbnode, node);

		/* We order based on item number alone */
		if (itm->idx > curlbn) {
			node = node->rb_left;
		} else if (itm->idx < curlbn) {
			node = node->rb_right;
		} else {
			found = 1;
			break;
		}
	}

	duet_dbg(KERN_DEBUG "duet-defrag: %s node (#%llu, e%u, d%p)\n",
		found ? "found" : "didn't find", found ? itm->idx : curlbn,
		found ? itm->evt : 0, found ? itm->bio : NULL);

	/* If we didn't find it insert and return */
	if (!found) {
		itm = tnode_init(task, curlbn, evtcode, (void *)bio);
		if (!itm) {
			printk(KERN_ERR "duet: itmnode alloc failed\n");
			goto out;
		}

		ret = tnode_insert(task, itm);
		if (ret) {
			printk(KERN_ERR "duet: item insert failed\n");
			kfree(itm);
		}
	} else if (found) {
		/* Update the itmnode */
		itm->evt = evtcode;
		itm->bio = bio;
	}

	curlbn++;
	if (curlbn * task->bitrange < (lbn + len - 1))
		goto again;

out:
	spin_unlock(&task->itmtree_lock);
}

/* We're finally here. Just find tasks that are interested in this event,
 * and call the appropriate handler for their ItemTree. */
static void duet_handle_event(__u8 evtcode, __u64 idx, __u32 len, void *data)
{
	struct duet_task *cur;

	duet_dbg(KERN_INFO "duet event: evt %u, idx %llu, len %u, data %p\n",
		evtcode, idx, len, data);

	/* Look for tasks interested in this event type and invoke callbacks */
	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		if (cur->evtmask & evtcode) {
			/* Provided the task has the appropriate tree in place,
			 * call the proper handler for this type of event */
			switch (cur->itmtype) {
			case DUET_ITEM_INODE:
				duet_handle_inode(cur, evtcode, idx,
							(struct page *)data);
				break;
			case DUET_ITEM_PAGE:
				//duet_handle_page(cur, evtcode, idx,
				//			(struct page *)data);
				break;
			case DUET_ITEM_BLOCK:
				duet_handle_blk(cur, evtcode, idx, len,
							(struct bio *)data);
				break;
			}
		}	
	}
	rcu_read_unlock();
}

/* Callback function for asynchronous bio calls. We first restore the normal
 * callback (and call it), and then proceed to call duet_handle_event. */
static void duet_bio_endio(struct bio *bio, int err)
{
	struct duet_bio_private *private;
	__u8 event_code;
	__u64 lbn;
	__u32 len;

	/* Grab data from bio -- keep in mind that bi_sector/size advance! */
	private = bio->bi_private;
	len = private->size - bio->bi_size; /* transf = orig - remaining */
	lbn = (bio->bi_sector << 9) - len; /* orig_offt = new_offt - transf */
	event_code = private->event_code;

	/* Restore real values */
	bio->bi_end_io = private->real_end_io;
	bio->bi_private = private->real_private;
	kfree(private);

	/* Transfer control to the duet event handler */
	duet_handle_event(event_code, lbn, len, (void *)bio);

	/* Call the real callback */
	bio->bi_end_io(bio, err);
}

/* Function that hijacks the bio with a duet callback, duet_bio_endio. */
static void duet_async_bio_hook(__u8 event_code, struct bio *bio)
{
	struct duet_bio_private *private = NULL;

	private = kzalloc(sizeof(*private), GFP_NOFS);
	if (!private) {
		printk(KERN_ERR "duet: bio_hook failed to allocate private\n");
		return;
	}

	/* Populate overload structure */
	private->real_end_io = bio->bi_end_io;
	private->real_private = bio->bi_private;
	private->event_code = event_code;
	private->size = bio->bi_size;

	/* Fix up bio structure */
	mark_bio_seen(bio);
	bio->bi_end_io = duet_bio_endio;
	bio->bi_private = (void *)private;
}

void duet_hook(__u8 event_code, __u8 hook_type, void *hook_data)
{
	__u64 offt, len;
	struct bio *bio;
	struct request *rq;
	struct page *page;

	if (!duet_online())
		return;

	switch (hook_type) {
	case DUET_HOOK_BA:
		/* Asynchronous bio hook */
		duet_dbg(KERN_INFO "duet: got async bio hook\n");
		duet_async_bio_hook(event_code, (struct bio *)hook_data);
		break;

	case DUET_HOOK_BW_START:
		/* Synchronous bio hook on dispatch */
		mark_bio_seen((struct bio *)hook_data);
		break;

	case DUET_HOOK_BW_END:
		/* Synchronous bio hook on return */
		duet_dbg(KERN_INFO "duet: got sync bio hook\n");

		bio = ((struct duet_bw_hook_data *)hook_data)->bio;
		offt = ((struct duet_bw_hook_data *)hook_data)->offset;
		len = (bio->bi_sector << 9) - offt;

		duet_handle_event(event_code, offt, len, (void *)bio);
		break;

	case DUET_HOOK_PAGE:
		/* Page cache hook */
		duet_dbg(KERN_INFO "duet: got page hook\n");

		page = (struct page *)hook_data;

		/* We're in RCU context so whatever happens, stay awake! */
		BUG_ON(!page);
		BUG_ON(!page->mapping);

		duet_handle_event(event_code, (__u64)page->index << PAGE_SHIFT,
					1, (void *)page);
		break;

	case DUET_HOOK_SCHED_INIT:
		/* IO scheduler hook on dispatch */
		bio = (struct bio *)hook_data;

		duet_handle_event(event_code, bio->bi_size, bio->bi_sector<<9,
					(void *)bio);
		break;

	case DUET_HOOK_SCHED_DONE:
		/* IO scheduler hook on return */
		rq = (struct request *)hook_data;

		/*
		 * TODO: When we finally get to use these hooks, figure out which
		 * fields are actually needed by the handlers that speak to the block
		 * layer. Until then, we'll bail if the a bio doesn't exist for the
		 * request.
		 */
		if (!rq->bio)
			return;

		duet_handle_event(event_code, rq->bio->bi_size,
			rq->bio->bi_sector << 9, (void *)rq->bio);
		break;

	default:
		printk(KERN_ERR "duet: unknown hook type %u (code %u)\n",
			hook_type, event_code);
		break;
	}
}
EXPORT_SYMBOL_GPL(duet_hook);

