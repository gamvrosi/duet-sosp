/*
 * Copyright (C) 2012 Alexander Block.  All rights reserved.
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

#define _GNU_SOURCE

#include "kerncompat.h"

#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <libgen.h>
#include <mntent.h>
#include <assert.h>
#include <signal.h>

#include <uuid/uuid.h>

#include "ctree.h"
#include "ioctl.h"
#include "commands.h"
#include "list.h"

#include "send.h"
#include "send-utils.h"

#ifdef DUET_BACKUP
#include <sys/syscall.h>

/* TBD: replace with #include "linux/ioprio.h" in some years */
#if !defined (IOPRIO_H)
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_PRIO_VALUE(class, data) \
		(((class) << IOPRIO_CLASS_SHIFT) | (data))
#define IOPRIO_CLASS_IDLE 3
#endif
#endif /* DUET_BACKUP */

static const char * const send_cmd_group_usage[] = {
	"btrfs send <command> [options] <subvol>",
	NULL
};

#define MAX_SUBV_LEN 1024
static struct btrfs_send cancel_send;
static char cancel_subvol[MAX_SUBV_LEN];
static int cancel_in_progress = 0;
static int g_verbose = 0;

struct btrfs_send {
	int send_fd;
	int dump_fd;
	int mnt_fd;

	u64 *clone_sources;
	u64 clone_sources_count;

	char *root_path;
	struct subvol_uuid_search sus;

#ifdef DUET_BACKUP
	/* Send priority state */
	int ioprio_class;
	int ioprio_classdata;
#endif /* DUET_BACKUP */
};

int find_mount_root(const char *path, char **mount_root)
{
	FILE *mnttab;
	int fd;
	struct mntent *ent;
	int len;
	int ret;
	int longest_matchlen = 0;
	char *longest_match = NULL;

	fd = open(path, O_RDONLY | O_NOATIME);
	if (fd < 0)
		return -errno;
	close(fd);

	mnttab = fopen("/proc/mounts", "r");
	if (!mnttab)
		return -errno;

	while ((ent = getmntent(mnttab))) {
		len = strlen(ent->mnt_dir);
		if (strncmp(ent->mnt_dir, path, len) == 0) {
			/* match found */
			if (longest_matchlen < len) {
				free(longest_match);
				longest_matchlen = len;
				longest_match = strdup(ent->mnt_dir);
			}
		}
	}
	fclose(mnttab);

	if (!longest_match) {
		fprintf(stderr,
			"ERROR: Failed to find mount root for path %s.\n",
			path);
		return -ENOENT;
	}

	ret = 0;
	*mount_root = realpath(longest_match, NULL);
	if (!*mount_root)
		ret = -errno;

	free(longest_match);
	return ret;
}

static int get_root_id(struct btrfs_send *s, const char *path, u64 *root_id)
{
	struct subvol_info *si;

	si = subvol_uuid_search(&s->sus, 0, NULL, 0, path,
			subvol_search_by_path);
	if (!si)
		return -ENOENT;
	*root_id = si->root_id;
	free(si->path);
	free(si);
	return 0;
}

static struct subvol_info *get_parent(struct btrfs_send *s, u64 root_id)
{
	struct subvol_info *si_tmp;
	struct subvol_info *si;

	si_tmp = subvol_uuid_search(&s->sus, root_id, NULL, 0, NULL,
			subvol_search_by_root_id);
	if (!si_tmp)
		return NULL;

	si = subvol_uuid_search(&s->sus, 0, si_tmp->parent_uuid, 0, NULL,
			subvol_search_by_uuid);
	free(si_tmp->path);
	free(si_tmp);
	return si;
}

static int find_good_parent(struct btrfs_send *s, u64 root_id, u64 *found)
{
	int ret;
	struct subvol_info *parent = NULL;
	struct subvol_info *parent2 = NULL;
	struct subvol_info *best_parent = NULL;
	__s64 tmp;
	u64 best_diff = (u64)-1;
	int i;

	parent = get_parent(s, root_id);
	if (!parent) {
		ret = -ENOENT;
		goto out;
	}

	for (i = 0; i < s->clone_sources_count; i++) {
		if (s->clone_sources[i] == parent->root_id) {
			best_parent = parent;
			parent = NULL;
			goto out_found;
		}
	}

	for (i = 0; i < s->clone_sources_count; i++) {
		parent2 = get_parent(s, s->clone_sources[i]);
		if (!parent2)
			continue;
		if (parent2->root_id != parent->root_id) {
			free(parent2->path);
			free(parent2);
			parent2 = NULL;
			continue;
		}

		free(parent2->path);
		free(parent2);
		parent2 = subvol_uuid_search(&s->sus, s->clone_sources[i], NULL,
				0, NULL, subvol_search_by_root_id);

		assert(parent2);
		tmp = parent2->ctransid - parent->ctransid;
		if (tmp < 0)
			tmp *= -1;
		if (tmp < best_diff) {
			if (best_parent) {
				free(best_parent->path);
				free(best_parent);
			}
			best_parent = parent2;
			parent2 = NULL;
			best_diff = tmp;
		} else {
			free(parent2->path);
			free(parent2);
			parent2 = NULL;
		}
	}

	if (!best_parent) {
		ret = -ENOENT;
		goto out;
	}

out_found:
	*found = best_parent->root_id;
	ret = 0;

out:
	if (parent) {
		free(parent->path);
		free(parent);
	}
	if (best_parent) {
		free(best_parent->path);
		free(best_parent);
	}
	return ret;
}

static void add_clone_source(struct btrfs_send *s, u64 root_id)
{
	s->clone_sources = realloc(s->clone_sources,
		sizeof(*s->clone_sources) * (s->clone_sources_count + 1));
	s->clone_sources[s->clone_sources_count++] = root_id;
}

static int write_buf(int fd, const void *buf, int size)
{
	int ret;
	int pos = 0;

	while (pos < size) {
		ret = write(fd, (char*)buf + pos, size - pos);
		if (ret < 0) {
			ret = -errno;
			fprintf(stderr, "ERROR: failed to dump stream. %s",
					strerror(-ret));
			goto out;
		}
		if (!ret) {
			ret = -EIO;
			fprintf(stderr, "ERROR: failed to dump stream. %s",
					strerror(-ret));
			goto out;
		}
		pos += ret;
	}
	ret = 0;

out:
	return ret;
}

static void *dump_thread(void *arg_)
{
	int ret;
	struct btrfs_send *s = (struct btrfs_send*)arg_;
	char buf[4096];
	int readed;

	while (1) {
		readed = read(s->send_fd, buf, sizeof(buf));
		if (readed < 0) {
			ret = -errno;
			fprintf(stderr, "ERROR: failed to read stream from "
					"kernel. %s\n", strerror(-ret));
			goto out;
		}
		if (!readed) {
			ret = 0;
			goto out;
		}
		ret = write_buf(s->dump_fd, buf, readed);
		if (ret < 0)
			goto out;
	}

out:
	if (ret < 0) {
		exit(-ret);
	}

	return ERR_PTR(ret);
}

static int do_send(struct btrfs_send *send, u64 root_id, u64 parent_root_id,
		   int is_first_subvol, int is_last_subvol)
{
	int ret;
	pthread_t t_read;
	pthread_attr_t t_attr;
	struct btrfs_ioctl_send_args io_send;
	struct subvol_info *si;
	void *t_err = NULL;
	int subvol_fd = -1;
	int pipefd[2] = {-1, -1};

	si = subvol_uuid_search(&send->sus, root_id, NULL, 0, NULL,
			subvol_search_by_root_id);
	if (!si) {
		ret = -ENOENT;
		fprintf(stderr, "ERROR: could not find subvol info for %llu",
				root_id);
		goto out;
	}

	subvol_fd = openat(send->mnt_fd, si->path, O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: open %s failed. %s\n", si->path,
				strerror(-ret));
		goto out;
	}

	ret = pthread_attr_init(&t_attr);

	ret = pipe(pipefd);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: pipe failed. %s\n", strerror(-ret));
		goto out;
	}

#ifdef DUET_BACKUP
	/* before moving on, set the ioprio */
	ret = syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0,
		IOPRIO_PRIO_VALUE(send->ioprio_class, send->ioprio_classdata));
	if (ret)
		fprintf(stderr,
			"WARNING: setting ioprio failed: %s (ignored).\n",
			strerror(errno));
#endif /* DUET_BACKUP */

	memset(&io_send, 0, sizeof(io_send));
	io_send.send_fd = pipefd[1];
	send->send_fd = pipefd[0];

	if (!ret)
		ret = pthread_create(&t_read, &t_attr, dump_thread,
					send);
	if (ret) {
		ret = -ret;
		fprintf(stderr, "ERROR: thread setup failed: %s\n",
			strerror(-ret));
		goto out;
	}

	io_send.clone_sources = (__u64*)send->clone_sources;
	io_send.clone_sources_count = send->clone_sources_count;
	io_send.parent_root = parent_root_id;
	if (!is_first_subvol)
		io_send.flags |= BTRFS_SEND_FLAG_OMIT_STREAM_HEADER;
	if (!is_last_subvol)
		io_send.flags |= BTRFS_SEND_FLAG_OMIT_END_CMD;
	ret = ioctl(subvol_fd, BTRFS_IOC_SEND, &io_send);
	if (cancel_in_progress) {
		fprintf(stderr, "send ioctl terminated\n");
		goto out;
	}
	if (ret) {
		ret = -errno;
		fprintf(stderr, "ERROR: send ioctl failed with %d: %s\n", ret,
			strerror(-ret));
		if (ret == -EINVAL && (!is_first_subvol || !is_last_subvol))
			fprintf(stderr,
				"Try upgrading your kernel or don't use -e.\n");
		goto out;
	}
	if (g_verbose > 0)
		fprintf(stderr, "BTRFS_IOC_SEND returned %d\n", ret);

	if (g_verbose > 0)
		fprintf(stderr, "joining genl thread\n");

	close(pipefd[1]);
	pipefd[1] = -1;

	ret = pthread_join(t_read, &t_err);
	if (ret) {
		ret = -ret;
		fprintf(stderr, "ERROR: pthread_join failed: %s\n",
			strerror(-ret));
		goto out;
	}
	if (t_err) {
		ret = (long int)t_err;
		fprintf(stderr, "ERROR: failed to process send stream, ret=%ld "
			"(%s)\n", (long int)t_err, strerror(-ret));
		goto out;
	}

	pthread_attr_destroy(&t_attr);

	ret = 0;

out:
	if (subvol_fd != -1)
		close(subvol_fd);
	if (pipefd[0] != -1)
		close(pipefd[0]);
	if (pipefd[1] != -1)
		close(pipefd[1]);
	if (si) {
		free(si->path);
		free(si);
	}
	return ret;
}

char *get_subvol_name(char *mnt, char *full_path)
{
	int len = strlen(mnt);
	if (!len)
		return full_path;
	if (mnt[len - 1] != '/')
		len += 1;

	return full_path + len;
}

static int init_root_path(struct btrfs_send *s, const char *subvol)
{
	int ret = 0;

	if (s->root_path)
		goto out;

	ret = find_mount_root(subvol, &s->root_path);
	if (ret < 0) {
		ret = -EINVAL;
		fprintf(stderr, "ERROR: failed to determine mount point "
				"for %s\n", subvol);
		goto out;
	}

	s->mnt_fd = open(s->root_path, O_RDONLY | O_NOATIME);
	if (s->mnt_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: can't open '%s': %s\n", s->root_path,
			strerror(-ret));
		goto out;
	}

	ret = subvol_uuid_search_init(s->mnt_fd, &s->sus);
	if (ret < 0) {
		fprintf(stderr, "ERROR: failed to initialize subvol search. "
				"%s\n", strerror(-ret));
		goto out;
	}

out:
	return ret;

}

static int is_subvol_ro(struct btrfs_send *s, char *subvol)
{
	int ret;
	u64 flags;
	int fd = -1;

	fd = openat(s->mnt_fd, subvol, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to open %s. %s\n",
				subvol, strerror(-ret));
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: failed to get flags for subvolume. "
				"%s\n", strerror(-ret));
		goto out;
	}

	if (flags & BTRFS_SUBVOL_RDONLY)
		ret = 1;
	else
		ret = 0;

out:
	if (fd != -1)
		close(fd);

	return ret;
}

static int do_cancel(struct btrfs_send *send, char *subvol)
{
	int ret = 0;
	u64 root_id;
	int subvol_fd = -1;
	struct subvol_info *si;

	ret = get_root_id(send, get_subvol_name(send->root_path, subvol),
			&root_id);
	if (ret < 0) {
		fprintf(stderr, "ERROR: could not resolve "
				"root_id for %s\n", subvol);
		return ret;
	}

	si = subvol_uuid_search(&send->sus, root_id, NULL, 0, NULL,
				subvol_search_by_root_id);
	if (!si) {
		ret = -ENOENT;
		fprintf(stderr, "ERROR: could not find subvol info for %llu",
				root_id);
		return ret;
	}

	subvol_fd = openat(send->mnt_fd, si->path, O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: open %s failed. %s\n", si->path,
				strerror(-ret));
		return ret;
	}

	ret = ioctl(subvol_fd, BTRFS_IOC_SEND_CANCEL, NULL);
	if (ret < 0) {
		fprintf(stderr, "ERROR: send cancel failed on %s: %s\n", subvol,
			errno == ENOTCONN ? "not running" : strerror(errno));
		return ret;
	}

	return ret;
}

static void send_sigint_terminate(int signal)
{
	int ret;

	fprintf(stderr, "Received SIGINT. Terminating...\n");
	cancel_in_progress = 1;
	ret = do_cancel(&cancel_send, cancel_subvol);
	if (ret < 0)
		perror("Send cancel failed");
}

static int send_handle_sigint(struct btrfs_send *send, char *subvol) {
	struct sigaction sa = {
		.sa_handler = send == NULL ? SIG_DFL : send_sigint_terminate,
	};

	memset(cancel_subvol, 0, MAX_SUBV_LEN);

	if (send)
		memcpy(&cancel_send, send, sizeof(*send));
	if (subvol)
		memcpy(cancel_subvol, subvol, strlen(subvol) < MAX_SUBV_LEN ?
			strlen(cancel_subvol) : MAX_SUBV_LEN - 1);

	return sigaction(SIGINT, &sa, NULL);
}

const char * const cmd_send_start_usage[] = {
	"btrfs send start [-Bve] [-p <parent>] [-c <clone-src>] [-f <outfile>]",
#ifdef DUET_BACKUP
	"                 [-C <class> -N <classdata>] <subvol>",
#else
	"<subvol>",
#endif /* DUET_BACKUP */
	"Send the subvolume to stdout.",
	"Sends the subvolume specified by <subvol> to stdout.",
	"By default, this will send the whole subvolume. To do an incremental",
	"send, use '-p <parent>'. If you want to allow btrfs to clone from",
	"any additional local snapshots, use '-c <clone-src>' (multiple times",
	"where applicable). You must not specify clone sources unless you",
	"guarantee that these snapshots are exactly in the same state on both",
	"sides, the sender and the receiver. It is allowed to omit the",
	"'-p <parent>' option when '-c <clone-src>' options are given, in",
	"which case 'btrfs send' will determine a suitable parent among the",
	"clone sources itself.",
	"\n",
	"-B               run send in the background",
	"-v               Enable verbose debug output. Each occurrence of",
	"                 this option increases the verbose level more.",
	"-e               If sending multiple subvols at once, use the new",
	"                 format and omit the end-cmd between the subvols.",
	"-p <parent>      Send an incremental stream from <parent> to",
	"                 <subvol>.",
	"-c <clone-src>   Use this snapshot as a clone source for an ",
	"                 incremental send (multiple allowed)",
	"-f <outfile>     Output is normally written to stdout. To write to",
	"                 a file, use this option. An alternative would be to",
	"                 use pipes.",
#ifdef DUET_BACKUP
	"-C <class>       set ioprio class (see ionice(1) manpage)",
	"-N <classdata>   set ioprio classdata (see ionice(1) manpage)",
#endif /* DUET_BACKUP */
	NULL
};

int cmd_send_start(int argc, char **argv)
{
	char *subvol = NULL;
	int c, pid;
	int ret = 0;
	int do_background = 0;
	char *outname = NULL;
	struct btrfs_send send;
	u32 i;
	char *mount_root = NULL;
	char *snapshot_parent = NULL;
	u64 root_id;
	u64 parent_root_id = 0;
	int full_send = 1;
	int new_end_cmd_semantic = 0;
#ifdef DUET_BACKUP
	int ioprio_class = IOPRIO_CLASS_IDLE;
	int ioprio_classdata = 0;
	char *optstr = "Bvec:f:i:p:C:N:";
#else
	char *optstr = "Bvec:f:i:p:";
#endif /* DUET_BACKUP */

	memset(&send, 0, sizeof(send));
	send.dump_fd = fileno(stdout);

	while ((c = getopt(argc, argv, optstr)) != -1) {
		switch (c) {
		case 'B':
			do_background = 1;
			break;
		case 'v':
			g_verbose++;
			break;
		case 'e':
			new_end_cmd_semantic = 1;
			break;
		case 'c':
			subvol = realpath(optarg, NULL);
			if (!subvol) {
				ret = -errno;
				fprintf(stderr, "ERROR: realpath %s failed. "
						"%s\n", optarg, strerror(-ret));
				goto out;
			}

			ret = init_root_path(&send, subvol);
			if (ret < 0)
				goto out;

			ret = get_root_id(&send, get_subvol_name(send.root_path, subvol),
					&root_id);
			if (ret < 0) {
				fprintf(stderr, "ERROR: could not resolve "
						"root_id for %s\n", subvol);
				goto out;
			}
			add_clone_source(&send, root_id);
			subvol_uuid_search_finit(&send.sus);
			free(subvol);
			subvol = NULL;
			if (send.mnt_fd >= 0) {
				close(send.mnt_fd);
				send.mnt_fd = -1;
			}
			free(send.root_path);
			send.root_path = NULL;
			full_send = 0;
			break;
		case 'f':
			outname = optarg;
			break;
		case 'p':
			if (snapshot_parent) {
				fprintf(stderr, "ERROR: you cannot have more than one parent (-p)\n");
				ret = 1;
				goto out;
			}
			snapshot_parent = realpath(optarg, NULL);
			if (!snapshot_parent) {
				ret = -errno;
				fprintf(stderr, "ERROR: realpath %s failed. "
						"%s\n", optarg, strerror(-ret));
				goto out;
			}
			full_send = 0;
			break;
		case 'i':
			fprintf(stderr,
				"ERROR: -i was removed, use -c instead\n");
			ret = 1;
			goto out;
#ifdef DUET_BACKUP
		case 'C':
			ioprio_class = (int) strtol(optarg, NULL, 10);
			break;
		case 'N':
			ioprio_classdata = (int) strtol(optarg, NULL, 10);
			break;
#endif /* DUET_BACKUP */
		case '?':
		default:
			fprintf(stderr, "ERROR: send args invalid.\n");
			ret = 1;
			goto out;
		}
	}

	if (optind == argc) {
		fprintf(stderr, "ERROR: send needs path to snapshot\n");
		ret = 1;
		goto out;
	}

	if (outname != NULL) {
		send.dump_fd = creat(outname, 0600);
		if (send.dump_fd == -1) {
			ret = -errno;
			fprintf(stderr, "ERROR: can't create '%s': %s\n",
					outname, strerror(-ret));
			goto out;
		}
	}

	if (isatty(send.dump_fd)) {
		fprintf(stderr, 
			"ERROR: not dumping send stream into a terminal, "
			"redirect it into a file\n");
		ret = 1;
		goto out;
	}

#ifdef DUET_BACKUP
	/* Set the IO priorities */
	send.ioprio_class = ioprio_class;
	send.ioprio_classdata = ioprio_classdata;
#endif /* DUET_BACKUP */

	/* use first send subvol to determine mount_root */
	subvol = argv[optind];

	subvol = realpath(argv[optind], NULL);
	if (!subvol) {
		ret = -errno;
		fprintf(stderr, "ERROR: unable to resolve %s - %s\n",
			argv[optind], strerror(errno));
		goto out;
	}

	ret = init_root_path(&send, subvol);
	if (ret < 0)
		goto out;

	if (snapshot_parent != NULL) {
		ret = get_root_id(&send,
				get_subvol_name(send.root_path, snapshot_parent),
				&parent_root_id);
		if (ret < 0) {
			fprintf(stderr, "ERROR: could not resolve root_id "
					"for %s\n", snapshot_parent);
			goto out;
		}

		add_clone_source(&send, parent_root_id);
	}

	for (i = optind; i < argc; i++) {
		free(subvol);
		subvol = realpath(argv[i], NULL);
		if (!subvol) {
			ret = -errno;
			fprintf(stderr, "ERROR: unable to resolve %s\n", argv[i]);
			goto out;
		}

		ret = find_mount_root(subvol, &mount_root);
		if (ret < 0) {
			fprintf(stderr, "ERROR: find_mount_root failed on %s: "
					"%s\n", subvol,
				strerror(-ret));
			goto out;
		}
		if (strcmp(send.root_path, mount_root) != 0) {
			ret = -EINVAL;
			fprintf(stderr, "ERROR: all subvols must be from the "
					"same fs.\n");
			goto out;
		}
		free(mount_root);

		ret = is_subvol_ro(&send, subvol);
		if (ret < 0)
			goto out;
		if (!ret) {
			ret = -EINVAL;
			fprintf(stderr, "ERROR: %s is not read-only.\n",
					subvol);
			goto out;
		}
	}

	if (do_background) {
		pid = fork();
		if (pid == -1) {
			fprintf(stderr, "ERROR: cannot send, fork failed: "
					"%s\n", strerror(errno));
			ret = 1;
			goto out;
		}

		if (pid) {
			printf("send started at %s\n", subvol);
			ret = 0;
			goto out;
		}
	}

	send_handle_sigint(&send, subvol);

	for (i = optind; i < argc; i++) {
		int is_first_subvol;
		int is_last_subvol;

		free(subvol);
		subvol = argv[i];

		fprintf(stderr, "At subvol %s\n", subvol);

		subvol = realpath(subvol, NULL);
		if (!subvol) {
			ret = -errno;
			fprintf(stderr, "ERROR: realpath %s failed. "
					"%s\n", argv[i], strerror(-ret));
			goto out;
		}

		ret = get_root_id(&send, get_subvol_name(send.root_path, subvol),
				&root_id);
		if (ret < 0) {
			fprintf(stderr, "ERROR: could not resolve root_id "
					"for %s\n", subvol);
			goto out;
		}

		if (!full_send && !parent_root_id) {
			ret = find_good_parent(&send, root_id, &parent_root_id);
			if (ret < 0) {
				fprintf(stderr, "ERROR: parent determination failed for %lld\n",
					root_id);
				goto out;
			}
		}

		ret = is_subvol_ro(&send, subvol);
		if (ret < 0)
			goto out;
		if (!ret) {
			ret = -EINVAL;
			fprintf(stderr, "ERROR: %s is not read-only.\n",
					subvol);
			goto out;
		}

		if (new_end_cmd_semantic) {
			/* require new kernel */
			is_first_subvol = (i == optind);
			is_last_subvol = (i == argc - 1);
		} else {
			/* be compatible to old and new kernel */
			is_first_subvol = 1;
			is_last_subvol = 1;
		}
		ret = do_send(&send, root_id, parent_root_id,
			      is_first_subvol, is_last_subvol);
		if (ret < 0)
			goto out;

		/* done with this subvol, so add it to the clone sources */
		add_clone_source(&send, root_id);

		parent_root_id = 0;
		full_send = 0;
	}

	ret = 0;

	send_handle_sigint(NULL, NULL);

out:
	free(subvol);
	free(snapshot_parent);
	free(send.clone_sources);
	if (send.mnt_fd >= 0)
		close(send.mnt_fd);
	if (send.dump_fd >= 0)
		close(send.dump_fd);
	free(send.root_path);
	subvol_uuid_search_finit(&send.sus);
	return !!ret;
}

static const char * const cmd_send_cancel_usage[] = {
	"btrfs send cancel <subvol>",
	"Cancel a running send",
	NULL
};

static int cmd_send_cancel(int argc, char **argv)
{
	char *subvol;
	int ret;
	struct btrfs_send send;

	memset(&send, 0, sizeof(send));

	if (check_argc_exact(argc, 2))
		usage(cmd_send_cancel_usage);

	/* use first send subvol to determine mount_root */
	subvol = realpath(argv[1], NULL);
	if (!subvol) {
		ret = -errno;
		fprintf(stderr, "ERROR: unable to resolve %s\n", argv[optind]);
		goto out;
	}

	ret = init_root_path(&send, subvol);
	if (ret < 0)
		goto out;

	ret = do_cancel(&send, subvol);
	if (ret < 0)
		goto out;

	printf("send cancelled\n");

out:
	free(subvol);
	if (send.mnt_fd >= 0)
		close(send.mnt_fd);
	free(send.root_path);
	subvol_uuid_search_finit(&send.sus);
	return ret;
}

#ifdef DUET_BACKUP
static const char * const cmd_send_status_usage[] = {
	"btrfs send status <subvol>",
	"Show status of running or finished send",
	NULL
};

static int cmd_send_status(int argc, char **argv)
{
	char *subvol;
	int ret;
	int subvol_fd = -1;
	struct btrfs_send send;
	struct subvol_info *si;
	struct btrfs_ioctl_send_args sa;
	u64 root_id;

	memset(&send, 0, sizeof(send));

	if (check_argc_exact(argc, 2))
		usage(cmd_send_status_usage);

	/* use first send subvol to determine mount_root */
	subvol = realpath(argv[1], NULL);
	if (!subvol) {
		ret = -errno;
		fprintf(stderr, "ERROR: unable to resolve %s\n", argv[optind]);
		goto out;
	}

	ret = init_root_path(&send, subvol);
	if (ret < 0)
		goto out;

	ret = get_root_id(&send, get_subvol_name(send.root_path, subvol),
			&root_id);
	if (ret < 0) {
		fprintf(stderr, "ERROR: could not resolve "
				"root_id for %s\n", subvol);
		goto out;
	}

	si = subvol_uuid_search(&send.sus, root_id, NULL, 0, NULL,
				subvol_search_by_root_id);
	if (!si) {
		ret = -ENOENT;
		fprintf(stderr, "ERROR: could not find subvol info for %llu",
				root_id);
		goto out;
	}

	subvol_fd = openat(send.mnt_fd, si->path, O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: open %s failed. %s\n", si->path,
				strerror(-ret));
		goto out;
	}

	ret = ioctl(subvol_fd, BTRFS_IOC_SEND_PROGRESS, &sa);
	if (ret < 0) {
		fprintf(stderr, "ERROR: send status failed on %s: %s\n", subvol,
			strerror(errno));
		ret = 1;
		goto out;
	}

	printf("Sent %llu bytes, %s %u sec.\nSent %llu bytes out of order.\n"
		"Spent %u sec doing read I/O, %u sec doing write I/O.\n",
		sa.progress.sent_total_bytes,
		sa.progress.running ? "running for" : "finished after",
		sa.progress.elapsed_time, sa.progress.sent_best_effort,
		sa.progress.elapsed_rtime, sa.progress.elapsed_wtime);

	ret = 0;
out:
	free(subvol);
	if (send.mnt_fd >= 0)
		close(send.mnt_fd);
	free(send.root_path);
	subvol_uuid_search_finit(&send.sus);
	return ret;
}
#endif /* DUET_BACKUP */

const struct cmd_group send_cmd_group = {
	send_cmd_group_usage, NULL, {
		{ "start", cmd_send_start, cmd_send_start_usage, NULL, 0 },
		{ "cancel", cmd_send_cancel, cmd_send_cancel_usage, NULL, 0 },
#ifdef DUET_BACKUP
		{ "status", cmd_send_status, cmd_send_status_usage, NULL, 0 },
#endif /* DUET_BACKUP */
		NULL_CMD_STRUCT
	}
};

int cmd_send(int argc, char **argv)
{
	return handle_command_group(&send_cmd_group, argc, argv);
}
