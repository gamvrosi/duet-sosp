/* Copyright (C) 1998 Oregon Graduate Institute of Science & Technology
 *
 * See the file COPYRIGHT, which should have been supplied with this
 * package, for licensing details.  We may be contacted through email
 * at <quasar-demo@cse.ogi.edu>.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "time_subr.h"

struct timeval *
get_time(struct timeval *tv)
{
  int err;

  if ((err = gettimeofday(tv, NULL)) != 0) {
    timerclear(tv);
    return NULL;
  }
  return tv;
}

/* returns tv2 - tv1, the difference of the two time values. If type is 'm'
   return time difference in msec. If type is 'u', return time difference in
   usec. */
static inline long 
get_duration(struct timeval *tv1, struct timeval *tv2, char type)
{
  struct timeval tva; /* the smaller value */
  struct timeval tvb; /* the bigger value */
  long diff;
  int neg = 0;

  if (timercmp(tv1, tv2, >)) { /* tv1 > tv2 */
    tvb = *tv1;
    tva = *tv2;
    neg = 1;
  } else {
    tvb = *tv2;
    tva = *tv1;
  }
  if (tvb.tv_usec < tva.tv_usec) {
    tvb.tv_sec--;
    tvb.tv_usec += 1000000;
  }
  if (type == 'u') {
    /* The user shouldn't call this function if tv_sec values differ by greater
     * than 2048 */ 
    if ((tvb.tv_sec - tva.tv_sec) > 0x800) {
      fprintf(stderr, "Overflow error in get_duration for microsecond time\n");
    }
    diff = (tvb.tv_sec - tva.tv_sec) * 1000000 + (tvb.tv_usec - tva.tv_usec);
  } else {
    if ((tvb.tv_sec - tva.tv_sec) > 0x200000) {
      fprintf(stderr, "Overflow error in get_duration for millisecond time\n");
    }
    diff = (tvb.tv_sec - tva.tv_sec) * 1000 + (tvb.tv_usec - tva.tv_usec)/1000;
  }
  return (neg == 1 ? 0 - diff : diff);
}

/* returns tv2 - tv1, the difference of the two time values in msec */
long 
get_msduration(struct timeval *tv1, struct timeval *tv2)
{
        return get_duration(tv1, tv2, 'm');
}

/* returns tv2 - tv1, the difference of the two time values in usec */
long 
get_usduration(struct timeval *tv1, struct timeval *tv2)
{
        return get_duration(tv1, tv2, 'u');
}

/* The predefined one uses itimer, and conflicts with the itimer used by other
   applications for timing purpose. */
void 
my_usleep(unsigned long usec)
{
  struct timeval val;

  if (usec <= 0) return;
  val.tv_sec = usec / 1000000;
  val.tv_usec = usec % 1000000;
  if (select(0, NULL, NULL, NULL, &val) == -1 && errno != 4)
  {
    perror("sleep with select");
    exit(1);
  }
}

/* Provides an estimate of the machine speed in Mhz to approximately 3 digits
   of accuracy. Assumes that gettimeofday returns reasonable numbers. -Ashvin
*/

static u_int64_t cycles_per_second = 0;

int
machine_speed_hz()
{
        if (!cycles_per_second) {
                int i;
                long t;

                u_int64_t p1, p2;
                u_int64_t n1, n2, tmp;

                struct timeval tp1;
                struct timeval tp2;

                double f1, f2;

                // warm everything up...
                rdtscll(p1);
                rdtscll(n1);
                rdtscll(p2);
                rdtscll(n2);
                gettimeofday(&tp1, NULL);
                gettimeofday(&tp2, NULL);

                rdtscll(p1);
                gettimeofday(&tp1, NULL);
                rdtscll(n1);

                for(i = 0; i < 5000000; i++) { /* run and do something */
                        rdtscll(tmp);
                }

                rdtscll(n2);
                gettimeofday(&tp2, NULL);
                rdtscll(p2);

                t = get_usduration(&tp1, &tp2);

                f1 = ((float)(n2 - n1))/t;
                f2 = ((float)(p2 - p1))/t;

                cycles_per_second = (u_int64_t)(((f1 + f2) * 500000) + 0.5);
        }

        return (int)cycles_per_second;
}

u_int64_t
usecs2cycles(int usecs) {
    return cycles_per_second * usecs / (u_int64_t)1000000;
}

int
cycles2usecs(u_int64_t cycles) {
    return (int)(cycles * 1000000 / cycles_per_second);
}
