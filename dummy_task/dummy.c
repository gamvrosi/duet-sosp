/*
 * Copyright (C) 2015 George Amvrosiadis.  All rights reserved.
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <duet/duet.h>

void usage(int err)
{
	fprintf(stderr,
		"\n"
		"dummy is a program meant to demonstrate how to use the Duet\n"
		"framework. For development purposes, it can also be used during\n"
		"testing.\n"
		"\n"
		"Usage: dummy [OPTION]...\n"
		"\n"
		"Program Options\n"
		" -f <freq>     event fetching frequency in msec (def: 10ms)\n"
		" -d <dur>      program execution time in sec\n"
		" -o            use Duet (if not set, Duet Options are ignored)\n"
		" -h            print this usage information\n"
		"\n"
		"Duet Options\n"
		" -e            register for event-based Duet (def: state-based)\n"
		" -p <path>     directory to register with Duet (def: '/')\n"
		" -g            get file path for every event received\n"
		"\n");

		exit(err);
}

int main(int argc, char *argv[])
{
	int freq = 10, duration = -1, o3 = 0, evtbased = 0, getpath = 0;
	char path[DUET_MAX_PATH] = "/";
	int ret = 0, tid, c, duet_fd = 0, itret = 0, tmp;
	long total_items = 0;
	long total_fetches = 0;
	__u32 regmask;
	struct duet_item buf[DUET_MAX_ITEMS];
	struct timespec slp = {0, 0};

	while ((c = getopt(argc, argv, "f:d:ohep:g")) != -1) {
		switch (c) {
		case 'f': /* Fetching frequency, in mseconds */
			freq = atoi(optarg);
			if (freq <= 0) {
				fprintf(stderr, "Error: invalid fetching frequency specified\n");
				usage(1);
			}
			break;
		case 'd': /* Program execution duration, in seconds */
			duration = atoi(optarg);
			if (duration < 0) {
				fprintf(stderr, "Error: invalid execution duration specified\n");
				usage(1);
			}
			break;
		case 'o': /* Use Duet */
			o3 = 1;
			break;
		case 'h': /* Display usage info */
			usage(0);
			break;
		case 'e': /* Register for event-based Duet */
			evtbased = 1;
			break;
		case 'p': /* Specify directory to register with Duet */
			if (strnlen(optarg, DUET_MAX_ITEMS + 1) > DUET_MAX_ITEMS) {
				fprintf(stderr, "Error: specified path too long\n");
				usage(1);
			}
			strncpy(path, optarg, DUET_MAX_ITEMS);
			break;
		case 'g': /* Get file path for every event */
			getpath = 1;
			break;
		default:
			fprintf(stderr, "Unknown argument!\n");
			usage(1);
		}
	}

	if (duration == -1) {
		fprintf(stderr, "Error: did not supply duration\n");
		return 1;
	}

	printf("Running dummy for %d sec. Fetching every %d ms.\n",
		duration, freq);

	/* Convert duration to mseconds and set nanosleep time */
	duration *= 1000;
	slp.tv_nsec = (freq * (long) 1E6) % (long) 1E9;
	slp.tv_sec = (freq * (long) 1E6) / (long) 1E9;

	/* Open Duet device */
	if (o3 && ((duet_fd = open_duet_dev()) == -1)) {
		fprintf(stderr, "Error: failed to open Duet device\n");
		return 1;
	}

	if (evtbased)
		regmask = DUET_PAGE_ADDED | DUET_FILE_TASK;
	else
		regmask = DUET_PAGE_EXISTS | DUET_FILE_TASK;

	/* Register with Duet framework */
	if (o3 && (duet_register(duet_fd, path, regmask, 1, "dummy", &tid))) {
		fprintf(stderr, "Error: failed to register with Duet\n");
		ret = 1;
		goto done_close;
	}

	/* Use specified fetching frequency */
	while (duration > 0) {
		if (o3) {
			itret = DUET_MAX_ITEMS;
			if (duet_fetch(duet_fd, tid, buf, &itret)) {
				fprintf(stderr, "Error: Duet fetch failed\n");
				ret = 1;
				goto done_dereg;
			}
			//fprintf(stdout, "Fetch received %d items.\n", itret);

			if (getpath) {
				for (c = 0; c < itret; c++) {
					tmp = duet_get_path(duet_fd, tid, buf[c].ino, path);
					if (tmp < 0) {
						fprintf(stderr, "Error: Duet get_path failed\n");
						ret = 1;
						goto done_dereg;
					}

					if (!tmp)
						fprintf(stdout, "Getpath code %d (evt %x). Got %s\n",
								tmp, buf[c].state, path);
					else
						fprintf(stdout, "Getpath code %d (evt %x).\n", tmp,
								buf[c].state);
				}
			}

			total_items += itret;
			total_fetches++;
		}

		if (nanosleep(&slp, NULL) < 0) {
			fprintf(stderr, "Error: nanosleep failed\n");
			ret = 1;
			goto done_dereg;
		}
		fprintf(stdout, "nanoslept, duration left %d\n", duration);

		duration -= freq;
	}

	/* Deregister with the Duet framework */
done_dereg:
	if (o3 && duet_deregister(duet_fd, tid))
		fprintf(stderr, "Error: failed to deregister with Duet\n");

done_close:
	if (o3) {
		close_duet_dev(duet_fd);
		fprintf(stdout, "Fetched %ld events, or %lf events/ms\n",
			total_items, ((double) total_items)/total_fetches);
	}

	return ret;
}
