/* Dummy task registering/fetching from Duet for CPU overhead measurements */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include "duet.h"

int main(int argc, char *argv[])
{
	int c, freq, duration, duet_fd = 0, itret;
	long total_items = 0;
	long total_fetches = 0;
	__u8 tid;
	struct duet_item buf[MAX_ITEMS];
	struct timespec slp = {0, 0};
	int o3 = 0;

	freq = duration = -1;
	while ((c = getopt(argc, argv, "f:d:o")) != -1)
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

	if (freq == -1)
		fprintf(stdout, "No fetch frequency? No fetching.\n");

	printf("Running dummy for %d seconds. Fetching every %d seconds.\n",
		duration, freq);

	/* Open Duet device */
	if (o3 && ((duet_fd = open_duet_dev()) == -1)) {
		fprintf(stderr, "failed to open Duet device\n");
		exit(1);
	}

	/* Register with Duet framework: EXISTS for state, ADDED for events */
	if (o3 && (duet_register(&tid, duet_fd, "rsync", 1, DUET_PAGE_EXISTS, "/"))) {
		fprintf(stderr, "failed to register with Duet\n");
		exit(1);
	}

	itret = 0;
	/* If fetching frequency was specified, we'll be doing it right now */
	if (freq != -1) {
		while (duration > 0) {
			duet_fetch(tid, duet_fd, MAX_ITEMS, buf, &itret);
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
		while (duration > 0)
			nanosleep(&slp, NULL);
	}

	/* Deregister with the Duet framework */
	if (duet_deregister(tid, duet_fd))
		fprintf(stderr, "failed to deregister with Duet\n");

	close_duet_dev(duet_fd);
	fprintf(stdout, "Fetched %ld events, or %lf events/ms\n", total_items,
		((double) total_items)/total_fetches);
	return 0;
}
