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
#include <linux/fs.h>
#include <linux/genhd.h>
#include "common.h"

struct duet_bio_private {
	bio_end_io_t	*real_end_io;
	void 		*real_private;
	__u8		hook_code;
	__u32		size;
};

struct duet_bh_private {
	bh_end_io_t	*real_end_io;
	void		*real_private;
	__u8		hook_code;
};

static inline void mark_bio_seen(struct bio *bio)
{
	/* Mark this bio as seen by the duet framework */
	set_bit(BIO_DUET, &bio->bi_flags);
}

/* We're finally here. Just find tasks that are interested in this event,
 * and call their handlers. */
/* TODO: Implement */
static void duet_handle_hook(__u8 hook_code, __u64 lbn, __u32 len)
{
	struct duet_task *cur;

#ifdef CONFIG_DUET_DEBUG
	printk(KERN_INFO "duet hook: code %u, lbn %llu, len %u\n",
		hook_code, lbn, len);
#endif /* CONFIG_DUET_DEBUG */

	/* Go over the task list, and look for a task whose hook */
	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
                if (cur->hook_mask & hook_code)
			cur->hook_handler(cur->id, hook_code, lbn, len, NULL);
	}
	rcu_read_unlock();
}

/* Callback function for asynchronous bio calls. We first restore the normal
 * callback (and call it), and then proceed to call duet_handle_hook. */
static void duet_bio_endio(struct bio *bio, int err)
{
	struct duet_bio_private *private;
	__u8 hook_code;
	__u64 lbn;
	__u32 len;

	/* Grab data from bio */
	private = bio->bi_private;
	lbn = bio->bi_sector << 9;
	len = private->size;
	hook_code = private->hook_code;

	/* Restore real values */
	bio->bi_end_io = private->real_end_io;
	bio->bi_private = private->real_private;
	kfree(private);

	/* Call the real callback */
	bio->bi_end_io(bio, err);

	/* Transfer control to hook handler */
	if (!err)
		duet_handle_hook(hook_code, lbn, len);
}

/* Callback function for asynchronous bh calls. We first restore the normal
 * callback (and call it), and then proceed to call duet_handle_hook. */
static void duet_bh_endio(struct buffer_head *bh, int uptodate)
{
	struct duet_bh_private *private;
	__u8 hook_code;
	__u64 lbn = 0;
	__u32 len = 0;

	/* Grab data from buffer head */
	private = bh->b_private;

	/* The b_blocknr of the buffer head is relative to the beginning
	 * of the device/partition, so let's find the start of that first */
	if (bh->b_bdev && bh->b_bdev->bd_part)
		lbn = bh->b_bdev->bd_part->start_sect << 9;

	lbn += bh->b_blocknr * bh->b_size;
	len = bh->b_size;
	hook_code = private->hook_code;

	/* Restore real values */
	bh->b_end_io = private->real_end_io;
	bh->b_private = private->real_private;
	kfree(private);

	/* Call the real callback */
	bh->b_end_io(bh, uptodate);

	/* Transfer control to hook handler */
	duet_handle_hook(hook_code, lbn, len);
}

/* Function that is paired to submit_bio calls. We need to hijack the bio
 * with a duet callback, duet_bio_endio. */
static void duet_ba_hook(__u8 hook_code, struct bio *bio)
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
	private->hook_code = hook_code;
	private->size = bio->bi_size;

	/* Fix up bio structure */
	mark_bio_seen(bio);
	bio->bi_end_io = duet_bio_endio;
	bio->bi_private = (void *)private;
}

/* Function that is paired to submit_bio_wait calls. We assume that we get
 * called after submit_bio_wait returns, so we can go ahead and call
 * duet_handle_hook with the right parameters. */
static void duet_bw_hook(__u8 hook_code, struct bio *bio)
{
	__u64 lbn;
	__u32 len;

	/* Collect data from bio */
	lbn = bio->bi_sector << 9;
	len = bio->bi_size;

	/* Transfer control to hook handler */
	duet_handle_hook(hook_code, lbn, len);
}

/* Function that is paired to submit_bh calls. We need to hijack the bh
 * with a duet callback, duet_bh_endio. */
static void duet_bh_hook(__u8 hook_code, struct buffer_head *bh)
{
	struct duet_bh_private *private = NULL;

	private = kzalloc(sizeof(*private), GFP_NOFS);
	if (!private) {
		printk(KERN_ERR "duet: bh_hook failed to allocate private\n");
		return;
	}

	/* Populate overload structure */
	private->real_end_io = bh->b_end_io;
	private->real_private = bh->b_private;
	private->hook_code = hook_code;

	/* Fix up buffer head structure */
	bh->b_end_io = duet_bh_endio;
	bh->b_private = (void *)private;
}

void duet_hook(__u8 hook_code, __u8 hook_type, void *hook_data)
{
	if (!duet_is_online())
		return;

	switch (hook_type) {
	case DUET_SETUP_HOOK_BA:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Setting up BA hook (code %u)\n",
			hook_code);
#endif /* CONFIG_DUET_DEBUG */
		duet_ba_hook(hook_code, (struct bio *)hook_data);
		break;
	case DUET_SETUP_HOOK_BW_START:
		mark_bio_seen((struct bio *)hook_data);
		break;
	case DUET_SETUP_HOOK_BW_END:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Setting up BW hook (code %u)\n",
			hook_code);
#endif /* CONFIG_DUET_DEBUG */
		duet_bw_hook(hook_code, (struct bio *)hook_data);
		break;
	case DUET_SETUP_HOOK_BH:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Setting up BH hook (code %u)\n",
			hook_code);
#endif /* CONFIG_DUET_DEBUG */
		duet_bh_hook(hook_code, (struct buffer_head *)hook_data);
		break;
	default:
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: Unknown hook type %u (code %u)\n",
			hook_type, hook_code);
#endif /* CONFIG_DUET_DEBUG */
		break;
	}
}
EXPORT_SYMBOL_GPL(duet_hook);
