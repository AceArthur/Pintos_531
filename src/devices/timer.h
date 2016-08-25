#ifndef DEVICES_TIMER_H
#define DEVICES_TIMER_H

#include <round.h>
#include <stdint.h>
#include <list.h>
#include "threads/thread.h"

/* Constant to indicate no sleeping threads */
#define NO_WAIT -1

/* Single element for the list of sleeping threads.
   t = pointer to the thread that called sleep.
   wait_till = total absolute clock ticks from OS boot at which point the thread
               requests to be awakened.  */
struct sleeping_thread
  {
    struct list_elem elem;
    int64_t wait_till;
    struct thread *t;
  };

/* Returns true if value A is less than value B, false
   otherwise. */
static bool
value_less (const struct list_elem *a_, const struct list_elem *b_,
            void *aux);

/* Number of timer interrupts per second. */
#define TIMER_FREQ 100

void timer_init (void);
void timer_calibrate (void);

int64_t timer_ticks (void);
int64_t timer_elapsed (int64_t);

/* Sleep and yield the CPU to other threads. */
void timer_sleep (int64_t ticks);
void timer_msleep (int64_t milliseconds);
void timer_usleep (int64_t microseconds);
void timer_nsleep (int64_t nanoseconds);

/* Busy waits. */
void timer_mdelay (int64_t milliseconds);
void timer_udelay (int64_t microseconds);
void timer_ndelay (int64_t nanoseconds);

void timer_print_stats (void);

#endif /* devices/timer.h */
