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

#ifndef __BTRFS_MAPPING_
#define __BTRFS_MAPPING_

#include "ctree.h"

struct btrfs_dev_extent_ctx {
	u64 pstart;	/* Physical starting address in device */
	u64 vstart;	/* Virtual starting address */
	u64 len;	/* Length of dev extent in bytes */
};

/*
 * Used by both logical-to-physical and physical-to-logical mappers to
 * iterate through discovered contiguous ranges:
 * - logical-to-physical: called with start, length, and bdev ptr
 * - physical-to-logical: called with start, length, and file_extent ptr
 */
typedef int (*iterate_ranges_t)(u64, u64, void *);

int btrfs_physical_to_items(struct btrfs_fs_info *fs_info,
			struct block_device *bdev, u64 p_start, u64 len,
			struct btrfs_root *root, iterate_ranges_t iterate);
int btrfs_ino_to_physical(struct btrfs_fs_info *fs_info, u64 ino, u64 iofft,
			u64 ilen, iterate_ranges_t iterate);
#endif /* __BTRFS_MAPPING_ */
