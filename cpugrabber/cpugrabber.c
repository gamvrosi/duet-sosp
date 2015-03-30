/*
 * Copyright (C) 1998 Oregon Graduate Institute of Science & Technology
 *
 * See the file COPYRIGHT, which should have been supplied with this package,
 * for licensing details.  We may be contacted through email at
 * <quasar-help@cse.ogi.edu>.
 */

#include <sys/param.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <popt.h>
#include "time_subr.h"

#define __STR(n) #n
#define STR(n) __STR(n)

/*
 * Make sure gcc doesn't try to be clever and move things around
 * on us. We need to use _exactly_ the address the user gave us,
 * not some alias that contains the same information.
 */

/* Make sure CONFIG_SMP is defined if you are running on an SMP */

#define CONFIG_SMP

#ifdef CONFIG_SMP
#define LOCK "lock ; "
#else
#define LOCK ""
#endif

typedef struct { volatile int counter; } atomic_t;
#define atomic_read(v)		((v)->counter)
#define atomic_set(v,i)		(((v)->counter) = (i))

/* Increment variable atomically. This ensures that the running program 
   and the signal handler don't access the "iterations" variable non-atomically
   */
static __inline__ void atomic_inc(atomic_t *v)
{
	__asm__ __volatile__(
		LOCK "incl %0"
		:"=m" (v->counter)
		:"m" (v->counter));
}

 // This variable tracks the amount of cpu grabbered
static atomic_t iterations;
static int time_interval = 0; // calculate for this time and then restart
// the length of the sigalarm interval (normally the same as time_interval)
static int alarm_interval = 0; 
static int sleep_time = 0; // sleep for so long between each restart
static int stop_time = 0; // total calculation time (ignores sleep time)
static int delay = 0; // boolean: wait for key press to start
// boolean: just wait forever before starting the program. useful for
// scheduling under pss but not really doing any work
static int wait = 0; 
static int loops = 0;

#ifdef CONFIG_OGI_PS_SCHEDULER
static int importance = 0; // importance of pss process
static int pss_flags = 0; // flags for pss scheduler
static int pss_allocation = 0; // allocation under pss scheduler
#ifdef PSS_EPOCH_IMPL
#define DEFAULT_PSS_PERIOD PSS_MAX_PERIOD
#else /* PSS_EPOCH_IMPL */
#define DEFAULT_PSS_PERIOD 20000 /* rather arbitrary */
#endif /* PSS_EPOCH_IMPL */
static int period = DEFAULT_PSS_PERIOD;
#endif /* CONFIG_OGI_PS_SCHEDULER */

#define USE_GETTIMEOFDAY

#ifdef USE_GETTIMEOFDAY
struct timeval start_tv;
struct timeval end_tv;
#define get_current_time(a) get_time(a)
#else /* USE_GETTIMEOFDAY */
int64_t start_tv;
int64_t end_tv;
#define get_current_time(a) rdtscll(*a) 
#endif /* USE_GETTIMEOFDAY */

void restart_timer(int itime)
{
	struct itimerval val;

	val.it_interval.tv_sec = val.it_value.tv_sec = itime / 1000;
	val.it_interval.tv_usec = val.it_value.tv_usec = 
		(itime % 1000) * 1000;
	if (setitimer(ITIMER_REAL, &val, NULL) < 0) {
		perror("setitimer");
		exit(1);
	}
}

void start_timer(void)
{
	if (stop_time == 0) {
		alarm_interval = time_interval;
		loops = 0;
	} else if (time_interval == 0) {
		alarm_interval = stop_time;
		loops = 1;
	} else {
		alarm_interval = MIN(time_interval, stop_time);
		loops = stop_time / time_interval;
	}
	if (alarm_interval == 0) {
		loops = 0;
		return; // no timer
	}
	alarm_interval++;
	restart_timer(alarm_interval);
}

static void
sigalarm(int signum)
{
	static int n = 0;
#ifdef USE_GETTIMEOFDAY
	unsigned long mstimediff;
#else /* USE_GETTIMEOFDAY */
        int64_t mstimediff;
#endif /* USE_GETTIMEOFDAY */

	get_current_time(&end_tv); // end timing
#ifdef USE_GETTIMEOFDAY
	mstimediff = get_msduration(&start_tv, &end_tv);
#else /* USE_GETTIMEOFDAY */
	mstimediff = end_tv - start_tv;
#endif /* USE_GETTIMEOFDAY */

	n++;

#ifdef USE_GETTIMEOFDAY
	fprintf(stderr, "n = %d, time = %ld.%.6ld\n"
		"signum = %d, iteration count = %d, time difference = %ld ms\n"
		"iterations/unit time = %f\n", n, start_tv.tv_sec,
		start_tv.tv_usec, signum, atomic_read(&iterations), 
		mstimediff, (float)atomic_read(&iterations)/mstimediff);
#else /* USE_GETTIMEOFDAY */
	fprintf(stderr, "n = %d, time = %Ld\n"
		"signum = %d, iteration count = %d, "
                "time difference = %Ld cycles\n"
		"iterations/unit time = %.10f\n", n, start_tv, signum,
                atomic_read(&iterations), mstimediff, 
                (float)atomic_read(&iterations)/mstimediff);
#endif /* USE_GETTIMEOFDAY */

	if ((loops > 0) && (n >= loops)) {
		exit(0); // we have done enough
	}
	restart_timer(0); // stop the timer
	atomic_set(&iterations, 0);
	if (signum == SIGALRM) {
 		if (sleep_time > 0) {
			fprintf(stderr, "sleep_time = %d\n", sleep_time);
			my_usleep(sleep_time * 1000);
		}
	}
	restart_timer(alarm_interval);
	get_current_time(&start_tv); // restart timing
}

static void
sigint(int signum)
{
	sigalarm(signum);
	exit(0);
}

poptContext context;   /* context for parsing command-line options */

static void
usage()
{
        poptPrintUsage(context, stderr, 0);
        exit(1);
}

/* The program prints out the number of iterations per millisecond that can be
 * executed under the current system load. Used to measure user-level idle
 * cycles in the system. */

int
main(int argc, const char **argv)
{
	struct sigaction act;
	int nice_value = 20;
        char c;

        struct poptOption options_table[] = {
#ifdef CONFIG_OGI_PS_SCHEDULER
                { NULL, 'a', POPT_ARG_INT, &pss_allocation, 'a',
                  "run under pss scheduler with fixed allocation", 
                  "min: 1, max: 1000, default: none"}, 
                { NULL, 'i', POPT_ARG_INT, &importance, 'i',
                  "importance of pss process", "min: 1, max: 5, default: 1"}, 
                { NULL, 'p', POPT_ARG_INT, &period, 'p', 
                  "period of pss process", 
                  "min: " STR(PSS_MIN_PERIOD) ", max: " STR(PSS_MAX_PERIOD) 
                  ", default: " STR(DEFAULT_PSS_PERIOD)},
#endif /* CONFIG_OGI_PS_SCHEDULER */
                { NULL, 'd', POPT_ARG_NONE, &delay, 'd', 
                  "delay starting the program until a key press", NULL},
                { NULL, 'n', POPT_ARG_INT, &nice_value, 'n',
                  "nice-value", "min: -19, max: 20, default: 20"}, 
                { NULL, 'r', POPT_ARG_INT, &time_interval, 'r', 
                  "restart calculation every interval", "in milliseconds"},
                { NULL, 's', POPT_ARG_INT, &sleep_time, 's', 
                  "sleep time between restart intervals", "in milliseconds"},
                { NULL, 't', POPT_ARG_INT, &stop_time, 't', 
                  "total running time (excluding sleep time)", 
                  "in milliseconds"},
                { NULL, 'w', POPT_ARG_NONE, &wait, 'w', 
                  "makes the program wait infinitely", NULL},
                POPT_AUTOHELP
                { NULL, 0, 0, NULL, 0 }
        };

        context = poptGetContext(NULL, argc, argv, options_table, 0);
        while ((c = poptGetNextOpt(context)) >= 0);
        if (c < -1) { /* an error occurred during option processing */
             fprintf(stderr, "%s: %s\n",
                     poptBadOption(context, POPT_BADOPTION_NOALIAS),
                     poptStrerror(c));
             exit(1);
        }

#ifdef CONFIG_OGI_PS_SCHEDULER
        if (pss_allocation) {
                pss_flags |= PSS_FIXED_PROPORTION;
                if (importance == 0) {
                        importance = 1; // assign default importance
                }
                if (pss_allocation == 0) {
                        usage();
                }
        }
        if ((importance) && (importance <= 0)) {
                usage();
        }
        if (period < PSS_MIN_PERIOD && (period > PSS_MAX_PERIOD)) {
                usage();
        }
#endif /* CONFIG_OGI_PS_SCHEDULER */
        if (time_interval && (time_interval <= 0)) {
                usage();
        }
        if ((nice_value) && ((nice_value <= -20) || (nice_value > 20))) {
                usage();
        }
        if (sleep_time && (sleep_time <= 0)) {
                usage();
        }
        if (stop_time && (stop_time <= 0)) {
                usage();
        }

	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);

	act.sa_handler = sigint;
	if (sigaction(SIGINT, &act, NULL) < 0) {
		perror("sigaction");
		exit(1);
	}
	act.sa_handler = sigalarm;
	if (sigaction(SIGALRM, &act, NULL) < 0) {
		perror("sigaction");
		exit(1);
	}
#ifdef CONFIG_OGI_PS_SCHEDULER	
	if (pss_allocation && importance) {
                int err;
		err = pss_schedule_process(getpid(), pss_allocation, period, 
                                           importance, pss_flags);
                if (err) {
                        perror("pss_schedule_process");
                        exit(1);
                }
	}
#endif /* CONFIG_OGI_PS_SCHEDULER */
	if (wait) {
		act.sa_handler = SIG_DFL;
		if (sigaction(SIGINT, &act, NULL) < 0) {
			perror("sigaction");
			exit(1);
		}
		sleep(100000000);
	}
	if (delay) {
		char dummy;
		printf("Press return to continue ... ");
		dummy = scanf("%c", &dummy);
	}
	if (nice(nice_value) < 0) {
		perror("nice");
		exit(1);
	}
        atomic_set(&iterations, 0);
	start_timer();
	get_current_time(&start_tv);
	for(;;) { /* grab CPU */
		/* iterations++; */
                atomic_inc(&iterations);
	}
}
