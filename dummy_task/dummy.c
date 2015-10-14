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
 *
 * Dummy task registering/fetching from Duet for CPU overhead measurements
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include "duet.h"

int main(int argc, char *argv[])
{
	int tid, c, freq, duration, duet_fd = 0, itret;
	long total_items = 0;
	long total_fetches = 0;
	__u8 evtmask;
	struct duet_item buf[MAX_ITEMS];
	struct timespec slp = {0, 0};
	int o3 = 0, evtbased = 0;

	freq = duration = -1;
	while ((c = getopt(argc, argv, "f:d:eo")) != -1)
		switch (c) {
		case 'f':
			/* This is frequency of fetching, in mseconds */
			freq = atoi(optarg);
			slp.tv_nsec = (freq * 1000000) % 1000000000;
			slp.tv_sec = (freq * 1000000) / 1000000000;
			fprintf(stdout, "tv_sec = %lu, tv_nsec = %lu\n", slp.tv_sec, slp.tv_nsec);
			break;
		case 'd':
			/* This is the duration of the experiment, in mseconds */
			duration = atoi(optarg);
			break;
		case 'e':
			evtbased = 1;
			break;
		case 'o':
			o3 = 1;
			break;
		default:
			fprintf(stderr, "Unknown argument!\n");
			exit(1);
		}

	if (duration == -1) {
		fprintf(stderr, "Did not supply duration. I quit.\n");
		exit(1);
	}

	//if (freq == -1)
	//	fprintf(stdout, "No fetch frequency? No fetching.\n");

	printf("Running dummy for %d seconds. Fetching every %d seconds.\n",
		duration, freq);

	/* Open Duet device */
	if (o3 && ((duet_fd = open_duet_dev()) == -1)) {
		fprintf(stderr, "failed to open Duet device\n");
		exit(1);
	}

	if (evtbased)
		evtmask = DUET_PAGE_ADDED;
	else
		evtmask = DUET_PAGE_EXISTS;

	/* Register with Duet framework */
	if (o3 && (duet_register(duet_fd, "/", evtmask, 1, "dummy", &tid))) {
		fprintf(stderr, "failed to register with Duet\n");
		exit(1);
	}

	itret = 0;
	/* If fetching frequency was specified, we'll be using it right now */
	if (freq > 0) {
		while (duration > 0) {
			itret = MAX_ITEMS;
			duet_fetch(duet_fd, tid, buf, &itret);
			//fprintf(stdout, "Fetch received %d items.\n", itret);
			total_items += itret;
			total_fetches++;
			if (nanosleep(&slp, NULL) < 0) {
				fprintf(stderr, "nanosleep failed\n");
				exit(1);
			}
			duration -= freq;
		}
	} else {
		freq = 10;
		slp.tv_nsec = (freq * 1000000) % 1000000000;
		slp.tv_sec = (freq * 1000000) / 1000000000;
		while (duration > 0) {
			nanosleep(&slp, NULL);
			duration -= freq;
		}
	}

	/* Deregister with the Duet framework */
	if (o3 && duet_deregister(duet_fd, tid))
		fprintf(stderr, "failed to deregister with Duet\n");

	if (o3) {
		close_duet_dev(duet_fd);
		fprintf(stdout, "Fetched %ld events, or %lf events/ms\n",
			total_items, ((double) total_items)/total_fetches);
	}

	return 0;
}
