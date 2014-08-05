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

/*
 * Notes:
 * - Support different sector/leaf/node sizes
 * - Check efficiency of sync'ing and re-reading root tree, fs root, path
 * - Why not defrag/fragment files as needed to reach target, instead of
 *   applying defragmentation and fragmentation on each file?
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <uuid/uuid.h>
#include <math.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "version.h"
#include "utils.h"
#include "ioctl.h"

/* Some global variables */
struct argflags
{
	char progname[256];

	short int verbose;
	short int stats;
	short int fragment;
	double f;
} args;

#define BUF_BLOCKS 1024
struct fs_stats
{
	unsigned long long tblocks;
	unsigned long long textents;
	unsigned long long tfiles;

	u32 blksize;
	char mntpath[PATH_MAX];
	char devname[256];
	struct btrfs_fs_info *info;

	/* used during fragmentation */
	char *fragbuf; /* BUF_BLOCKS wide */
} stats;

struct filepath
{
	char comp[BTRFS_NAME_LEN];
	struct filepath *next;
};

static int print_usage(void)
{
	fprintf(stderr, "usage: %s [-sv] [-f F] device\n", args.progname);
	fprintf(stderr, "\t-s   : prints per-file fragmentation statistics for device\n");
	fprintf(stderr, "\t-f F : fragments the filesystem to at least F*100%% (0.0 <= F <= 1.0)\n");
	fprintf(stderr, "\t-v   : be verbose! I want to know everything!\n");
	fprintf(stderr, "\t       filesystem MUST be mounted\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

static int sync_btrfs(char *path)
{
	int fd, res, e;
	DIR *dirstream = NULL;

	if (path == NULL) path = stats.mntpath;

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "Error: couldn't open '%s'\n", path);
		return 1;
	}

	if (args.verbose >= 2)
		printf("- Syncing '%s'\n", path);
	res = ioctl(fd, BTRFS_IOC_SYNC);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if (res < 0) {
		fprintf(stderr, "Error: unable to sync '%s' - %s\n",
			path, strerror(e));
		return 1;
	}

	return 0;
}

/* Uses btrfs's defrag file ioctl to defrag a file */
static int defrag_file(char *path, short int flush)
{
	int fd, ret = 0, e = 0;
	int fancy_ioctl = flush;
	struct btrfs_ioctl_defrag_range_args range;
	DIR *dirstream = NULL;

	/* Initialization of defrag ioctl parameters */
	memset(&range, 0, sizeof(range));
	range.len = (u64)-1;
	range.extent_thresh = (u32)-1;
	if (flush)
		range.flags |= BTRFS_DEFRAG_RANGE_START_IO;

	/* Open file for defrag ioctl */
	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s for defrag\n", path);
		perror("open:");
		return 1;
	}

	if (!fancy_ioctl) {
		ret = ioctl(fd, BTRFS_IOC_DEFRAG, NULL);
		e=errno;
	} else {
		ret = ioctl(fd, BTRFS_IOC_DEFRAG_RANGE, &range);
		e=errno;
		if (ret && e == ENOTTY) {
			fprintf(stderr, "ERROR: defrag range ioctl not supported "
				"in this kernel, please amend code to avoid flushing.\n");
			close(fd);
			return 1;
		}
	}

	if (ret) {
		fprintf(stderr, "ERROR: defrag failed on %s - %s\n", path, strerror(e));
		ret = 1;
	}
	close_file_or_dir(fd, dirstream);

	return ret;
}

/*
 * Perhaps the most complicated function of them all, it fragments a file
 * doing synchronous writes to its last frag_blocks blocks in reverse
 * order (btrfs will create new extents for them)
 */
int frag_file(char *path, u64 frag_blocks)
{
	int fd, ret = 0;
	u64 read_blocks;
	ssize_t processed, wrsize;
	off_t fd_offt, buf_rem, buf_offt;

	if (!frag_blocks)
		return ret;

	/* Open file, then read and overwrite blocks starting from block #last-1,
	   until fragmented enough */
	fd = open(path, O_RDWR | O_SYNC);
	fd_offt = lseek(fd, (off_t)0, SEEK_END);

again:
	/*
	 * We try to buffer up to BUF_BLOCKS, unless that's too many. We also
	 * make sure to buffer only as many bytes of the last block as we need
	 * to avoid altering the file's size when we write out the data
	 */
	read_blocks = (BUF_BLOCKS > frag_blocks ? frag_blocks : BUF_BLOCKS);
	buf_rem = read_blocks * stats.blksize;
	if (fd_offt % stats.blksize)
		buf_rem -= (stats.blksize - (fd_offt % stats.blksize));

	if (fd_offt < buf_rem) {
		buf_rem = fd_offt;
		fd_offt = 0;
	} else {
		fd_offt -= buf_rem;
	}

	/* Update remaining blocks. This is as good a time as any. */
	frag_blocks -= read_blocks;

	fprintf(stderr, "fd_offt: %zd, buf_rem: %zd, frag_blocks: %llu, "
		"read_blocks: %llu\n", fd_offt, buf_rem, frag_blocks,
		read_blocks);

	/* Lather: populate the buffer */
	ret = lseek(fd, fd_offt, SEEK_SET);
	if (ret == (off_t)-1) {
		fprintf(stderr, "Error: seek failed (%zd)\n", fd_offt);
		ret = -1;
		goto done;
	}

	buf_offt = 0;
	while (buf_rem) {
		processed = read(fd, stats.fragbuf + buf_offt, buf_rem);
		if (processed == -1) {
			fprintf(stderr, "Error: read failed "
				"(offt: %zd, sz: %zd)\n", fd_offt, buf_rem);
			ret = -1;
			goto done;
		}

		buf_rem -= processed;
		buf_offt += processed;
	}

	/*
	 * Rinse: write until we run out of blocks or we're done.
	 * We'll start writing from the last buffered block.
	 */
	buf_rem = buf_offt;
	if (buf_rem < stats.blksize)
		buf_offt = 0;
	else
		buf_offt = ceil((buf_rem - stats.blksize) / stats.blksize) *
								stats.blksize;
	while (buf_rem) {
		ret = lseek(fd, fd_offt + buf_offt, SEEK_SET);
		if (ret == (off_t)-1) {
			fprintf(stderr, "Error: seek failed (%zd)\n",
							fd_offt + buf_offt);
			ret = -1;
			goto done;
		}

		wrsize = (stats.blksize > (buf_rem - buf_offt) ?
					(buf_rem - buf_offt) : stats.blksize);
		processed = write(fd, stats.fragbuf + buf_offt, wrsize);
		if (processed != wrsize || processed == -1) {
			fprintf(stderr, "Error: write failed "
				"(offt: %zd, sz: %zd)\n",
				fd_offt + buf_offt, wrsize);
			ret = -1;
			goto done;
		}

		buf_rem = buf_offt;
		if (buf_offt)
			buf_offt -= stats.blksize;
	}

	/* Repeat to fragment further */
	if (frag_blocks)
		goto again;

	/* We made it here? Then we succeeded */
	ret = 0;
done:
	close(fd);
	return ret;
}

static void free_fpath(struct filepath **fpath)
{
	struct filepath *tmp;

	while (*fpath != NULL) {
		tmp = *fpath;
		*fpath = (*fpath)->next;
		free(tmp);
	}
}

/* Function handling inode ref items. ext refs are not supported, and we only take
 * into account the first hard link to the file (which should be ok for defrag
 * purposes).
 */
static int find_full_path(struct btrfs_root *root, struct btrfs_path *path, char **res)
{
	int idx, ret = 0;
	u32 len, name_len;
	u64 cur_offset;
	struct btrfs_key search_key;
	struct btrfs_disk_key dkey;
	struct btrfs_path rpath;
	struct btrfs_inode_ref *ref;
	struct filepath *cur, *fpath = NULL;

	btrfs_item_key(path->nodes[0], &dkey, path->slots[0]);
	cur_offset = btrfs_disk_key_objectid(&dkey);

	while (cur_offset > 256ULL /* objectID of root inode_ref */) {
		/* Find next BTRFS_INODE_REF_KEY */
		search_key.objectid = cur_offset;
		btrfs_set_key_type(&search_key, BTRFS_INODE_REF_KEY);
		search_key.offset = (u64)-1;
		btrfs_init_path(&rpath);

		/* Search key */
		ret = btrfs_search_slot(NULL, root, &search_key, &rpath, 0, 0);
		BUG_ON(ret <= 0);
		ret = btrfs_previous_item(root, &rpath, 0, search_key.type);
		btrfs_item_key(rpath.nodes[0], &dkey, rpath.slots[0]);

		if (args.verbose >= 3) {
			btrfs_print_leaf(root, rpath.nodes[0]);
			btrfs_print_key(&dkey);
			printf(" (offset = %llu)\n", cur_offset);
		}

		/* Check that what we found is actually an inode ref item */
		if (btrfs_disk_key_type(&dkey) != BTRFS_INODE_REF_KEY) {
			fprintf(stderr, "Error: failed to track ref item\n");
			ret = 1;
			goto out;
		}

		/* Grab name and add that to the name-component list */
		cur = (struct filepath *)malloc(sizeof(struct filepath));
		BUG_ON(!cur);
		cur->next = fpath;
		fpath = cur;

		ref = btrfs_item_ptr(rpath.nodes[0], rpath.slots[0], struct btrfs_inode_ref);
		name_len = btrfs_inode_ref_name_len(rpath.nodes[0], ref);
		len = (name_len <= sizeof(cur->comp)) ? name_len : sizeof(cur->comp);
		read_extent_buffer(rpath.nodes[0], cur->comp, (unsigned long)(ref + 1), len);
		cur->comp[len] = '\0';

		cur_offset = btrfs_disk_key_offset(&dkey);
	}

	/* Concatenate the list/path components in a string reflecting the full path
	 * to the file
	 */
	ret = idx = 0;
	*res = (char *)malloc(PATH_MAX * sizeof(char));
	if (*res == NULL) {
		fprintf(stderr, "Error: couldn't allocate space for filename\n");
		ret = 1;
		goto out;
	}

	/* First, append the mntpath */
	strcpy(*res, stats.mntpath);
	idx = strlen(*res);

	/* Append to res until PATH_MAX is reached */
	cur = fpath;
	while (cur != NULL && idx + strlen(cur->comp) < PATH_MAX) {
		(*res)[idx++] = '/';
		(*res)[idx++] = '\0';
		strcat(*res, cur->comp);
		idx += strlen(cur->comp) - 1;
		cur = cur->next;
	}

out:
	free_fpath(&fpath);
	return ret;
}

static int find_inode_frag(struct btrfs_root *root, struct btrfs_path *path,
	double *fragidx, u64 *size, u64 *blocks, u64 *extents)
{
	int s, ret;
	int extent_type;
	u64 remsize, lastbyte, firstbyte;
	struct btrfs_key search_key;
	struct btrfs_disk_key dkey;
	struct btrfs_inode_item *ii;
	struct btrfs_file_extent_item *fi;
	struct extent_buffer *l;
	struct btrfs_path epath;

	/* To estimate file fragmentation, We need: the number of extents that
	 * the file is broken into, and its size. We get that by looking into
	 * inode and extent data items.
	 */

	/* First, we assume that path is pointing to an inode item. process_tree
	 * should take care of that. We need to grab the file size from it.
	 */
	l = path->nodes[0];
	s = path->slots[0];
	ii = btrfs_item_ptr(l, s, struct btrfs_inode_item);
	/* Check this is not a directory */
	*extents = 1;
	if (btrfs_inode_mode(l, ii) / 01000 != 040) {
		*size = btrfs_inode_size(l, ii);
		if (*size > 0)
			*blocks = ceil((double) *size / stats.blksize);
		else
			*blocks = 1;
	} else
		return 1;

	/* If file is empty, there are not extents for it */
	if (*size == 0) {
		*fragidx = 0.0;
		goto done;
	}

	/* Now we need to find all the extents for the file. Keep searching until
	   we have found enough to cover the known file size */
	remsize = *blocks * stats.blksize;

	/* Find the first extent data item */
	btrfs_item_key(path->nodes[0], &dkey, path->slots[0]);
	search_key.objectid = btrfs_disk_key_objectid(&dkey);
	btrfs_set_key_type(&search_key, BTRFS_EXTENT_DATA_KEY);
	search_key.offset = 0;
	btrfs_init_path(&epath);
	ret = btrfs_search_slot(NULL, root, &search_key, &epath, 0, 0);
	BUG_ON(ret != 0);

	l = epath.nodes[0];
	s = epath.slots[0];
	fi = btrfs_item_ptr(l, s, struct btrfs_file_extent_item);
	extent_type = btrfs_file_extent_type(l, fi);

	/* If the data is inlined, then stop here */
	if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
		*fragidx = 0.0;
		goto done;
	}

	lastbyte = btrfs_file_extent_disk_bytenr(l, fi) +
		btrfs_file_extent_offset(l, fi) +
		btrfs_file_extent_num_bytes(l, fi) - 1;
	remsize -= btrfs_file_extent_num_bytes(l, fi);

	/* Get the rest of the extent data items */
	while (remsize > 0) {
		/* Just grab the next item in the tree */
		if (btrfs_next_item(root, &epath, BTRFS_EXTENT_DATA_KEY))
			break;

		l = epath.nodes[0];
		s = epath.slots[0];
		fi = btrfs_item_ptr(l, s, struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(l,fi);
		if (extent_type != BTRFS_FILE_EXTENT_REG &&
		    extent_type != BTRFS_FILE_EXTENT_PREALLOC) {
			fprintf(stderr, "Warning: found fewer extents than expected (%llu "
				"bytes left)\n", (unsigned long long)remsize);
			break;
		}

		/* Find the first valid byte for this extent */
		firstbyte = btrfs_file_extent_disk_bytenr(l, fi) +
			btrfs_file_extent_offset(l, fi);

		/* Two blocks are close, if they're less than 4*block_size
		   bytes apart */
		if (lastbyte > firstbyte || firstbyte > lastbyte + 4*stats.blksize)
			(*extents)++;

		if (remsize >= btrfs_file_extent_num_bytes(l, fi))
			remsize -= btrfs_file_extent_num_bytes(l, fi);
		else {
			fprintf(stderr, "Warning: we exceeded file size while looking "
				"for extents!\n");
			remsize = 0;
		}

		/* Update last valid byte */
		lastbyte = btrfs_file_extent_disk_bytenr(l, fi) +
			btrfs_file_extent_offset(l, fi) +
			btrfs_file_extent_num_bytes(l, fi) - 1;
	}

	if (*blocks == 1) *fragidx = 0.0;
	else *fragidx = (double) (*extents - 1) / (*blocks - 1);

done:
	return 0;
}

static void process_inode(struct btrfs_path *path)
{
	int ret;
	double fragidx, f_a;
	u64 size, blocks, extents, e_a;
	char *fullpath = NULL;
	struct btrfs_key search_key;
	struct btrfs_root *root = stats.info->fs_root;
	struct btrfs_disk_key key;

	if (args.verbose >= 2) {
		btrfs_item_key(path->nodes[0], &key, path->slots[0]);
		printf("Found: ");
		btrfs_print_key(&key);
		printf("\n");
	}

	/* First, find how fragmented this inode is */
	if (find_inode_frag(root, path, &fragidx, &size, &blocks, &extents)) {
		/* This inode did not correspond to a file */
		return;
	}

	if (args.fragment) {
		/* Estimate attainable fragmentation goal for this file */
		/* First, find out the full path of the file */
		if (find_full_path(root, path, &fullpath)) {
			fprintf(stderr, "Error: couldn't get inode path\n");
			return;
		}

		/*
		 * Calculate how many blocks we need relocate (e_a), for file
		 * to be f_a% fragmented, where f_a is the min attainable frag
		 * goal for this file, s.t. f_a >= f, where f is the requested
		 * frag goal, f = F / 100.0. We have:
		 *      f_a = ceil(f / (1 / (b-1))) * (1 / (b-1))
		 */
		if (blocks > 1) {
			e_a = ceil(args.f * (blocks - 1.0));
			f_a = (double) e_a / (blocks - 1.0);

			/* If fragidx != f_a, then defrag first, then fragment as needed */
			if (fragidx != f_a) {
				if (args.verbose >= 2)
					printf("- Before: %9llu bytes (%5lu blocks), "
						"\t%5.2f%% fragmented (%2lu extents)\n",
						(unsigned long long)size, (unsigned long)blocks,
						(double)fragidx * 100.0, (unsigned long)extents);

				if (fragidx) defrag_file(fullpath, 1);

				if (args.verbose >= 2)
					printf("- Fragmenting: Need to write %lu blocks "
						"to fragment adequately (%5.2f%%).\n",
						(unsigned long)e_a, (double)f_a * 100.0);

				if (f_a && frag_file(fullpath, e_a))
					printf("Error: failed to fragment file '%s'\n", fullpath);

				/* Sync changes to disk */
				sync_btrfs(fullpath);

				/* We've been switching things around; better update the root/path
				 * (latch onto inode number to find the way) */
				btrfs_item_key(path->nodes[0], &key, path->slots[0]);
				search_key.objectid = btrfs_disk_key_objectid(&key);
				btrfs_set_key_type(&search_key, BTRFS_INODE_ITEM_KEY);
				search_key.offset = 0;
				btrfs_release_path(path);

				btrfs_free_fs_info(stats.info);
				stats.info = open_ctree_fs_info(stats.devname, 0, 0, 1);
				root = stats.info->fs_root;

				ret = btrfs_search_slot(NULL, root, &search_key, path, 0, 0);
				BUG_ON(ret != 0);

				/* Now that our path is not stale, estimate the new fragmentation
				 * index for the file */
				if (find_inode_frag(root, path, &fragidx, &size, &blocks, &extents))
					fprintf(stderr, "There was some issue updating file fragmentation info.\n");
			}
		}
	}

	/* Update global stats */
	stats.tblocks += blocks;
	stats.tfiles++;
	stats.textents += extents;

	if (args.stats) {
		printf("File: %10llu bytes (%5lu blocks), \t%6.2f%% fragmented (%2lu extents)",
			(unsigned long long)size, (unsigned long)blocks,
			(double)fragidx * 100.0, (unsigned long)extents);

		if (args.verbose >= 1) {
			char *dbgpath;
			find_full_path(root, path, &dbgpath);
			printf(", path: %s", dbgpath);
			free(dbgpath);
		}
		printf("\n");
		if (args.verbose >= 2) {
			if (!extent_buffer_uptodate(path->nodes[0]))
				printf("- Warning: the provided info came from an out-of-date extent!\n");
			if (stats.info->fs_root != root)
				printf("- Warning: the fs tree root is out-of-date!\n");
		}
	}

	if (fullpath)
		free(fullpath);
}

static void process_tree(void)
{
	int ret;
	struct btrfs_path path;
	struct btrfs_key search_key;
	struct btrfs_disk_key disk_key;

	btrfs_init_path(&path);

	/* Find the first inode in the fs tree */
	search_key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	btrfs_set_key_type(&search_key, BTRFS_INODE_ITEM_KEY);
	search_key.offset = 0;

	ret = btrfs_search_slot(NULL, stats.info->fs_root, &search_key, &path,
									0, 0);
	BUG_ON(ret);

	while (1) {
		/* Check that what we found is actually an inode item */
		btrfs_item_key(path.nodes[0], &disk_key, path.slots[0]);
		if (btrfs_disk_key_type(&disk_key) != BTRFS_INODE_ITEM_KEY)
			goto next;

		/* Process this inode */
		process_inode(&path);
		if (args.verbose)
			printf("  Processed inode #%llu\n",
				btrfs_disk_key_objectid(&disk_key));

next:
		/* Find next item */
		ret = btrfs_next_item(stats.info->fs_root, &path, BTRFS_INODE_ITEM_KEY);
		if (ret) {
			if (ret < 0)
				fprintf(stderr, "Error getting next inode\n");
			break;
		}
	}

	if (args.fragment)
		sync_btrfs(NULL);

	btrfs_release_path(&path);
}

int main(int ac, char **av)
{
	int ret = 0;
	FILE *fp;
	char uuidbuf[37], cmd[128];

	/* Initialize globals and locals */
	memset(&args, 0, sizeof(struct argflags));
	memset(&stats, 0, sizeof(stats));

	/* Get program name */
	strncpy(args.progname, av[0], 256);

	/* Parse command line options */
	while (1) {
		int c = getopt(ac, av, "sf:v");

		if (c < 0) break;
		switch (c) {
		case 's': /* print stats */
			args.stats = 1;
			break;
		case 'f': /* define fragmentation target */
			args.fragment = 1;
			args.f = atof(optarg);
			if (args.f < 0.0 || args.f > 1.0) {
				fprintf(stderr, "Error: bad frag target.\n");
				print_usage();
				return 1;
			}
			break;
		case 'v': /* be verbose */
			args.verbose++;
			break;
		default:
			print_usage();
		}
	}

	ac = ac - optind;
	if (ac != 1)
		print_usage();

	/* Get fs info */
	strncpy(stats.devname, av[optind], 256);
	stats.info = open_ctree_fs_info(stats.devname, 0, 0, 1);
	if (!stats.info) {
		fprintf(stderr, "unable to open %s\n", stats.devname);
		exit(1);
	}

	/* If fs is mounted, find mount point */
	sprintf(cmd, "cat /proc/mounts | grep %s | cut -f2 -d' '",
		stats.devname);
	fp = popen(cmd, "r");
	if (fp) {
		if (fscanf(fp, "%s", stats.mntpath) != 1)
			stats.mntpath[0] = '\0';
		pclose(fp);
	}

	if (stats.mntpath[0] == '\0') {
		fprintf(stderr, "Error: Couldn't find fs mount point. Have you\n"
			"mounted the filesystem?\n");
		print_usage();
		ret = 1;
		goto done;
	}

	/* Print fs info header */
	printf("Fragmentation tool for %s\n\n", BTRFS_BUILD_VERSION);
	printf("Device: %s, ", av[optind]);
	if (stats.mntpath[0] == '\0')
		printf("unmounted.\n");
	else
		printf("mounted on: %s\n", stats.mntpath);

	uuidbuf[36] = '\0';
	uuid_unparse(stats.info->super_copy->fsid, uuidbuf);
	printf("Filesystem UUID: %s\n", uuidbuf);
	printf("Capacity: %llu bytes total, %llu bytes (%3.2f%%) used\n",
		(unsigned long long)btrfs_super_total_bytes(stats.info->super_copy),
		(unsigned long long)btrfs_super_bytes_used(stats.info->super_copy),
		100.0 * (double)btrfs_super_bytes_used(stats.info->super_copy) /
		(double)btrfs_super_total_bytes(stats.info->super_copy));

	stats.blksize = (unsigned long)btrfs_super_leafsize(stats.info->super_copy);
	printf("Sector: %lub, Node: %lub, Leaf: %lub, Stripe: %lub\n\n",
		(unsigned long)btrfs_super_sectorsize(stats.info->super_copy),
		(unsigned long)btrfs_super_nodesize(stats.info->super_copy),
		(unsigned long)btrfs_super_leafsize(stats.info->super_copy),
		(unsigned long)btrfs_super_stripesize(stats.info->super_copy));

	/* Allocate buffer used during fragmentation */
	if (args.fragment) {
		stats.fragbuf = (char *)malloc(BUF_BLOCKS * stats.blksize);
		if (!stats.fragbuf) {
			fprintf(stderr, "Error allocating frag buffer\n");
			ret = 1;
			goto done;
		}
	}

	/* Go ahead and process the fs tree */
	printf("Traversing filesystem tree...\n");
	process_tree();
	printf("Filesystem %5.2f%% fragmented\n",
		100.0 * (1.0 - (double) (stats.tblocks - stats.textents) /
		(stats.tblocks - stats.tfiles)));

	if (args.fragment)
		free(stats.fragbuf);
done:
	btrfs_free_fs_info(stats.info);
	return ret;
}

