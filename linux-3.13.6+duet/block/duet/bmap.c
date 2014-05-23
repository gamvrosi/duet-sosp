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
#include "common.h"

__u32 duet_bmap_count(__u8 *bmap, __u32 byte_len)
{
	__u32 i, bits_on = 0;
	__u8 par_array[16] =
		{ 0 /* 0000 */, 1 /* 0001 */, 1 /* 0010 */, 2 /* 0011 */,
		  1 /* 0100 */, 2 /* 0101 */, 2 /* 0110 */, 3 /* 0111 */,
		  1 /* 1000 */, 2 /* 1001 */, 2 /* 1010 */, 3 /* 1011 */,
		  2 /* 1100 */, 3 /* 1101 */, 3 /* 1110 */, 4 /* 1111 */};

	/* Count bits set in the bitmap */
	for (i=0; i<byte_len; i++)
		bits_on += (par_array[ bmap[i] & 0xf ] +
			par_array[(bmap[i] >> 4) & 0xf ]);

	return bits_on;
}

void duet_bmap_print(__u8 *bmap, __u32 byte_len)
{
	__u32 i, m;
	char buf[128];

	/* Print a bitmap as 32 groups of 4 hex (512 bits) per line */
	for (i=0; i<byte_len; i++) {
		m = i % 32;
		if (m == 0) {
			if (i > 0)
				printk(KERN_DEBUG "%s\n", buf);
			sprintf(buf, "bgtask: [%5u] %02x", i, bmap[i]);
		} else {
			sprintf(buf, "%s:%02x", buf, bmap[i]);
		}
	}
	printk(KERN_DEBUG "%s\n", buf);
}

/* sets/clears bits [start, start + num) for bmap */
static void duet_bmap_set_bits(__u8 *bmap, __u32 start, __u32 num, __u8 set)
{
	__u8 f_bits, l_bits, n_bytes, f_mask, l_mask;

	/*
	 * We are asked to mark an arbitrary number of bits, and it may look like
	 * the following diagram, and we don't want to loop here; we'd rather
	 * memset the nbytes:
	 *             01234567     01234567           01234567
	 *           +----------+ +----------+       +----------+
	 *           | *****sss | | ssssssss |  ...  | ssss**** |
	 *           +----------+ +----------+       +----------+
	 *              f_bits    1st of n_bytes        l_bits     (s = set)
	 */

	f_bits = start % 8;
	l_bits = (start + num - 1) % 8;
	f_mask = (1 << (8 - f_bits)) - 1;
	l_mask = ~((1 << (8 - l_bits - 1)) - 1);

#ifdef CONFIG_DUET_DEBUG
	printk(KERN_DEBUG
		"duet_bmap_set_bits: start=%u, num=%u, fbits=%02x, "
		"l_bits=%02x, f_mask=%02x, l_mask=%02x\n", start, num,
		f_bits, l_bits, f_mask, l_mask);
#endif /* CONFIG_DUET_DEBUG */

	if (8 - f_bits >= num) {
		/* We are marking stuff only in one byte block */
		if (set)
			bmap[start / 8] |=  (f_mask & l_mask);
		else
			bmap[start / 8] &= ~(f_mask & l_mask);
	} else {
		/* There are at least 2 byte blocks that need to be marked */
		n_bytes = (num - (l_bits + 1) - (8 - f_bits)) / 8;

		/* Set the first and last byte blocks */
		if (set) {
			bmap[start / 8] |=  f_mask;
			bmap[(start + num - 1) / 8] |=  l_mask;
		} else {
			bmap[start / 8] &= ~f_mask;
			bmap[(start + num - 1) / 8] &= ~l_mask;
		}

		/* Set the intermediate byte blocks */
		if (n_bytes)
			memset(&bmap[(start/8)+1], set ? 0xff : 0, n_bytes);
	}
}

/*
 * Generic function to (un)mark a bitmap. We need information on the bitmap,
 * and information on the byte range it represents, and the byte range that
 * needs to be (un)marked.
 * A return value of 0 implies success, while -1 implies failure.
 */
int duet_bmap_set(__u8 *bmap, __u32 bmap_bytelen, __u64 first_byte,
	__u32 blksize, __u64 req_byte, __u32 req_bytelen, __u8 set)
{
	__u32 start, num;

	start = (req_byte - first_byte) / blksize;
	num = req_bytelen / blksize + (req_bytelen % blksize ? 1 : 0);

	if (start + num >= (first_byte + (bmap_bytelen * 8 * blksize)))
		return -1;

	duet_bmap_set_bits(bmap, start, num, set);
	return 0;
}

/* checks bits [start, start + num) for bmap, to ensure they match 'set' */
static int duet_bmap_chk_bits(__u8 *bmap, __u32 start, __u32 num, __u8 set)
{
	__u8 f_bits, l_bits, n_bytes, f_mask, l_mask;
	int *buf, ret = 1;

	/*
	 * We are asked to check an arbitrary number of bits, and it may look like
	 * the following diagram, and we don't want to loop here; we'd rather
	 * memcmp bytes if we can:
	 *             01234567     01234567           01234567 
	 *           +----------+ +----------+       +----------+
	 *           | *****sss | | ssssssss |  ...  | ssss**** |
	 *           +----------+ +----------+       +----------+
	 *              f_bits    1st of n_bytes        l_bits     (s = set)
	 */

	f_bits = start % 8;
	l_bits = (start + num - 1) % 8;
	f_mask = (1 << (8 - f_bits)) - 1;
	l_mask = ~((1 << (8 - l_bits - 1)) - 1);

#ifdef CONFIG_DUET_DEBUG
	printk(KERN_DEBUG
		"duet_bmap_chk_bits: start=%u, num=%lu, fbits=%02x, "
		"l_bits=%02x, f_mask=%02x, l_mask=%02x\n", start,
		(long unsigned int) num, f_bits, l_bits, f_mask, l_mask);
#endif /* CONFIG_DUET_DEBUG */

	if (8 - f_bits >= num) {
		/* We are checking stuff only in one byte block */
		if (set)
			ret &= (( bmap[start / 8] & (f_mask & l_mask)) == (f_mask & l_mask));
		else
			ret &= ((~bmap[start / 8] & (f_mask & l_mask)) == (f_mask & l_mask));
	} else {
		/* There are at least 2 byte blocks that need to be checked */
		n_bytes = (num - (l_bits + 1) - (8 - f_bits)) / 8;

		/* Check the first and last byte block */
		if (set) {
			ret &= (( bmap[start / 8] & f_mask) == f_mask);
			ret &= (( bmap[(start + num - 1) / 8] & l_mask) == l_mask);
		} else {
			ret &= ((~bmap[start / 8] & f_mask) == f_mask);
			ret &= ((~bmap[(start + num - 1) / 8] & l_mask) == l_mask);
		}

		if (!ret) return ret;

		/* Check the intermediate byte blocks */
		if (n_bytes) {
			buf = kzalloc(n_bytes, GFP_NOFS);
			memset(buf, set ? 0xff : 0, n_bytes);
			ret &= (1 - memcmp(&bmap[(start/8)+1], buf, n_bytes));
			kfree(buf);
		}
	}

	return ret;
}

/* Returns 1, if all bytes in [req_bytes, req_bytes+req_bytelen) are set/reset,
 * 0 otherwise. If an error occurs, then -1 is returned. */
int duet_bmap_chk(__u8 *bmap, __u32 bmap_bytelen, __u64 first_byte,
	__u32 blksize, __u64 req_byte, __u32 req_bytelen, __u8 set)
{
	__u32 start, num;

	start = (req_byte - first_byte) / blksize;
	num = req_bytelen / blksize + (req_bytelen % blksize ? 1 : 0);

	if (start + num >= (first_byte + (bmap_bytelen * 8 * blksize)))
		return -1;

	return duet_bmap_chk_bits(bmap, start, num, set);
}
