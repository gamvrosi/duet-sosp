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

/* We're finally here. Just find tasks that are interested in this event,
 * and call their handlers. */
static void duet_handle_event(__u8 event_code, void *owner, __u64 offt,
					__u32 len, void *data, int data_type)
{
	struct duet_task *cur;

#ifdef CONFIG_DUET_DEBUG
	printk(KERN_INFO "duet event: event %u, %s %p, offt %llu, len %u\n",
		event_code, (data_type == DUET_DATA_PAGE) ? "inode" : "bdev",
		owner, offt, len);
#endif /* CONFIG_DUET_DEBUG */

	/* Look for tasks interested in this event type and invoke callbacks */
	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		if (cur->event_mask & event_code)
			cur->event_handler(cur->id, event_code, owner, offt,
					len, data, data_type, cur->privdata);
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
	struct block_device *bdev;

	/* Grab data from bio -- keep in mind that bi_sector/size advance! */
	private = bio->bi_private;
	len = private->size - bio->bi_size; /* transf = orig - remaining */
	lbn = (bio->bi_sector << 9) - len; /* orig_offt = new_offt - transf */
	bdev = bio->bi_bdev;
	event_code = private->event_code;

	/* Restore real values */
	bio->bi_end_io = private->real_end_io;
	bio->bi_private = private->real_private;
	kfree(private);

	/* Transfer control to the duet event handler */
	duet_handle_event(event_code, (void *)bdev, lbn, len, (void *)bio,
								DUET_DATA_BIO);

	/* Call the real callback */
	bio->bi_end_io(bio, err);
}

/* Function that is paired to submit_bio calls. We need to hijack the bio
 * with a duet callback, duet_bio_endio. */
static void duet_ba_hook(__u8 event_code, struct bio *bio)
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

/* Function that is paired to submit_bio_wait calls. We assume that we get
 * called after submit_bio_wait returns, so we can go ahead and call
 * duet_handle_event with the right parameters. */
static void duet_bw_hook(__u8 event_code, struct duet_bw_hook_data *hook_data)
{
	struct bio *bio = hook_data->bio;
	__u64 lbn;
	__u32 len;
	struct block_device *bdev;

	/* Collect data from bio */
	lbn = hook_data->offset;
	len = (bio->bi_sector << 9) - lbn;
	bdev = bio->bi_bdev;

	/* Transfer control to the duet event handler */
	duet_handle_event(event_code, (void *)bdev, lbn, len, (void *)bio,
								DUET_DATA_BIO);
}

/* We're in RCU context so whatever happens, stay awake! */
void duet_page_hook(__u8 event_code, struct page *page)
{
	BUG_ON(!page);
	BUG_ON(!page->mapping);

	duet_handle_event(event_code, (void *)page->mapping->host,
			(__u64)page->index << PAGE_SHIFT, PAGE_SIZE,
			(void *)page, DUET_DATA_PAGE);
}

void duet_blkreq_init_hook(__u8 event_code, struct bio *bio)
{
	duet_handle_event(event_code, (void *)bio->bi_bdev, bio->bi_size,
		bio->bi_sector << 9, (void *)bio, DUET_DATA_BIO);
}

void duet_blkreq_done_hook(__u8 event_code, struct request *rq)
{
	/*
	 * TODO: When we finally get to use these hooks, figure out which
	 * fields are actually needed by the handlers that speak to the block
	 * layer. Until then, we'll bail if the a bio doesn't exist for the
	 * request.
	 */
	if (!rq->bio)
		return;

	duet_handle_event(event_code, (void *)rq->bio->bi_bdev,
		rq->bio->bi_size, rq->bio->bi_sector << 9, (void *)rq->bio,
		DUET_DATA_BLKREQ);
}

void duet_hook(__u8 event_code, __u8 hook_type, void *hook_data)
{
	if (!duet_is_online())
		return;

	switch (hook_type) {
	case DUET_SETUP_HOOK_BA:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Setting up BA hook (code %u)\n",
			event_code);
#endif /* CONFIG_DUET_DEBUG */
		duet_ba_hook(event_code, (struct bio *)hook_data);
		break;
	case DUET_SETUP_HOOK_BW_START:
		mark_bio_seen((struct bio *)hook_data);
		break;
	case DUET_SETUP_HOOK_BW_END:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Setting up BW hook (code %u)\n",
			event_code);
#endif /* CONFIG_DUET_DEBUG */
		duet_bw_hook(event_code, (struct duet_bw_hook_data *)hook_data);
		break;
	case DUET_SETUP_HOOK_PAGE:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Setting up PAGE hook (code %u)\n",
			event_code);
#endif /* CONFIG_DUET_DEBUG */
		duet_page_hook(event_code, (struct page *)hook_data);
		break;
	case DUET_SETUP_HOOK_BLKREQ_INIT:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Setting up BLKREQ_INIT hook (code %u)\n",
			event_code);
#endif /* CONFIG_DUET_DEBUG */
		duet_blkreq_init_hook(event_code, (struct bio *)hook_data);
		break;
	case DUET_SETUP_HOOK_BLKREQ_DONE:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Setting up BLKREQ_DONE hook (code %u)\n",
			event_code);
#endif /* CONFIG_DUET_DEBUG */
		duet_blkreq_done_hook(event_code, (struct request *)hook_data);
		break;
	default:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Unknown hook type %u (code %u)\n",
			hook_type, event_code);
#endif /* CONFIG_DUET_DEBUG */
		break;
	}
}
EXPORT_SYMBOL_GPL(duet_hook);
