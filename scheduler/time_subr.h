/* Copyright (C) 1998 Oregon Graduate Institute of Science & Technology
 *
 * See the file COPYRIGHT, which should have been supplied with this
 * package, for licensing details.  We may be contacted through email
 * at <quasar-demo@cse.ogi.edu>.
 */

#ifndef __TIME_H
#define __TIME_H
#include <sys/types.h>
#include <sys/time.h>

#ifndef rdtscll
#define rdtscll(result) \
	__asm__ __volatile__("rdtsc" : "=A" (result) : /* No inputs */ )
#endif /* rdtscll */

void my_usleep(unsigned long usec);
struct timeval *get_time(struct timeval *tv);
long get_msduration(struct timeval *tv1, struct timeval *tv2);
long get_usduration(struct timeval *tv1, struct timeval *tv2);
int machine_speed_hz(void);

int cycles2usecs(u_int64_t cycles);
u_int64_t usecs2cycles(int usecs);
#endif /* __TIME_H */
