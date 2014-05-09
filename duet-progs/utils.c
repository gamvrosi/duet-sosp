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

//#define _XOPEN_SOURCE 700
//#define __USE_XOPEN2K8
//#define __XOPEN2K8 /* due to an error in dirent.h, to get dirfd() */
//#define _GNU_SOURCE	/* O_NOATIME */
//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
//#include <sys/ioctl.h>
//#include <sys/mount.h>
//#include <sys/types.h>
#include <sys/stat.h>
//#include <uuid/uuid.h>
#include <fcntl.h>
#include <unistd.h>
//#include <mntent.h>
//#include <ctype.h>
//#include <linux/loop.h>
//#include <linux/major.h>
//#include <linux/kdev_t.h>
//#include <limits.h>
//#include <blkid/blkid.h>
//#include "kerncompat.h"
//#include "radix-tree.h"
#include "utils.h"
//#include "ioctl.h"

//#ifndef BLKDISCARD
//#define BLKDISCARD	_IO(0x12,119)
//#endif

#if 0
/*
 * checks if a path is a block device node
 * Returns negative errno on failure, otherwise
 * returns 1 for blockdev, 0 for not-blockdev
 */
int is_block_device(const char *path)
{
	struct stat statbuf;

	if (stat(path, &statbuf) < 0)
		return -errno;

	return S_ISBLK(statbuf.st_mode);
}

/*
 * Given a pathname, return a filehandle to:
 * 	the original pathname or,
 * 	if the pathname is a mounted btrfs device, to its mountpoint.
 *
 * On error, return -1, errno should be set.
 */
int open_path_or_dev_mnt(const char *path, DIR **dirstream)
{
	char mp[BTRFS_PATH_NAME_MAX + 1];
	int fdmnt;

	if (is_block_device(path)) {
		int ret;

		ret = get_btrfs_mount(path, mp, sizeof(mp));
		if (ret < 0) {
			/* not a mounted btrfs dev */
			errno = EINVAL;
			return -1;
		}
		fdmnt = open_file_or_dir(mp, dirstream);
	} else {
		fdmnt = open_file_or_dir(path, dirstream);
	}

	return fdmnt;
}

/* checks if a device is a loop device */
static int is_loop_device (const char* device) {
	struct stat statbuf;

	if(stat(device, &statbuf) < 0)
		return -errno;

	return (S_ISBLK(statbuf.st_mode) &&
		MAJOR(statbuf.st_rdev) == LOOP_MAJOR);
}


/* Takes a loop device path (e.g. /dev/loop0) and returns
 * the associated file (e.g. /images/my_btrfs.img) */
static int resolve_loop_device(const char* loop_dev, char* loop_file,
		int max_len)
{
	int ret;
	FILE *f;
	char fmt[20];
	char p[PATH_MAX];
	char real_loop_dev[PATH_MAX];

	if (!realpath(loop_dev, real_loop_dev))
		return -errno;
	snprintf(p, PATH_MAX, "/sys/block/%s/loop/backing_file", strrchr(real_loop_dev, '/'));
	if (!(f = fopen(p, "r")))
		return -errno;

	snprintf(fmt, 20, "%%%i[^\n]", max_len-1);
	ret = fscanf(f, fmt, loop_file);
	fclose(f);
	if (ret == EOF)
		return -errno;

	return 0;
}

/* Checks whether a and b are identical or device
 * files associated with the same block device
 */
static int is_same_blk_file(const char* a, const char* b)
{
	struct stat st_buf_a, st_buf_b;
	char real_a[PATH_MAX];
	char real_b[PATH_MAX];

	if(!realpath(a, real_a) ||
	   !realpath(b, real_b))
	{
		return -errno;
	}

	/* Identical path? */
	if(strcmp(real_a, real_b) == 0)
		return 1;

	if(stat(a, &st_buf_a) < 0 ||
	   stat(b, &st_buf_b) < 0)
	{
		if (errno == ENOENT)
			return 0;
		return -errno;
	}

	/* Same blockdevice? */
	if(S_ISBLK(st_buf_a.st_mode) &&
	   S_ISBLK(st_buf_b.st_mode) &&
	   st_buf_a.st_rdev == st_buf_b.st_rdev)
	{
		return 1;
	}

	/* Hardlink? */
	if (st_buf_a.st_dev == st_buf_b.st_dev &&
	    st_buf_a.st_ino == st_buf_b.st_ino)
	{
		return 1;
	}

	return 0;
}

/* Checks if a file exists and is a block or regular file*/
static int is_existing_blk_or_reg_file(const char* filename)
{
	struct stat st_buf;

	if(stat(filename, &st_buf) < 0) {
		if(errno == ENOENT)
			return 0;
		else
			return -errno;
	}

	return (S_ISBLK(st_buf.st_mode) || S_ISREG(st_buf.st_mode));
}

static char *size_strs[] = { "", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
int pretty_size_snprintf(u64 size, char *str, size_t str_bytes)
{
	int num_divs = 0;
	float fraction;

	if (str_bytes == 0)
		return 0;

	if( size < 1024 ){
		fraction = size;
		num_divs = 0;
	} else {
		u64 last_size = size;
		num_divs = 0;
		while(size >= 1024){
			last_size = size;
			size /= 1024;
			num_divs ++;
		}

		if (num_divs >= ARRAY_SIZE(size_strs)) {
			str[0] = '\0';
			return -1;
		}
		fraction = (float)last_size / 1024;
	}
	return snprintf(str, str_bytes, "%.2f%s", fraction,
			size_strs[num_divs]);
}

/*
 * __strncpy__null - strncpy with null termination
 * @dest:	the target array
 * @src:	the source string
 * @n:		maximum bytes to copy (size of *dest)
 *
 * Like strncpy, but ensures destination is null-terminated.
 *
 * Copies the string pointed to by src, including the terminating null
 * byte ('\0'), to the buffer pointed to by dest, up to a maximum
 * of n bytes.  Then ensure that dest is null-terminated.
 */
char *__strncpy__null(char *dest, const char *src, size_t n)
{
	strncpy(dest, src, n);
	if (n > 0)
		dest[n - 1] = '\0';
	return dest;
}

u64 parse_size(char *s)
{
	int i;
	char c;
	u64 mult = 1;

	for (i = 0; s && s[i] && isdigit(s[i]); i++) ;
	if (!i) {
		fprintf(stderr, "ERROR: size value is empty\n");
		exit(50);
	}

	if (s[i]) {
		c = tolower(s[i]);
		switch (c) {
		case 'e':
			mult *= 1024;
			/* fallthrough */
		case 'p':
			mult *= 1024;
			/* fallthrough */
		case 't':
			mult *= 1024;
			/* fallthrough */
		case 'g':
			mult *= 1024;
			/* fallthrough */
		case 'm':
			mult *= 1024;
			/* fallthrough */
		case 'k':
			mult *= 1024;
			/* fallthrough */
		case 'b':
			break;
		default:
			fprintf(stderr, "ERROR: Unknown size descriptor "
				"'%c'\n", c);
			exit(1);
		}
	}
	if (s[i] && s[i+1]) {
		fprintf(stderr, "ERROR: Illegal suffix contains "
			"character '%c' in wrong position\n",
			s[i+1]);
		exit(51);
	}
	return strtoull(s, NULL, 10) * mult;
}
#endif

int open_dev(const char *fname)
{
	int ret;
	struct stat st;
	int fd = -1;

	ret = stat(fname, &st);
	if (ret < 0)
		return -1;

	if (S_ISCHR(st.st_mode))
		fd = open(fname, O_RDWR);

	return fd;
}

void close_dev(int fd)
{
		close(fd);
}

#if 0
#define isoctal(c)	(((c) & ~7) == '0')

static inline void translate(char *f, char *t)
{
	while (*f != '\0') {
		if (*f == '\\' &&
		    isoctal(f[1]) && isoctal(f[2]) && isoctal(f[3])) {
			*t++ = 64*(f[1] & 7) + 8*(f[2] & 7) + (f[3] & 7);
			f += 4;
		} else
			*t++ = *f++;
	}
	*t = '\0';
	return;
}

/*
 * This reads a line from the stdin and only returns non-zero if the
 * first whitespace delimited token is a case insensitive match with yes
 * or y.
 */
int ask_user(char *question)
{
	char buf[30] = {0,};
	char *saveptr = NULL;
	char *answer;

	printf("%s [y/N]: ", question);

	return fgets(buf, sizeof(buf) - 1, stdin) &&
	       (answer = strtok_r(buf, " \t\n\r", &saveptr)) &&
	       (!strcasecmp(answer, "yes") || !strcasecmp(answer, "y"));
}
#endif
