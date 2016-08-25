#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <list.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

/* Semaphore to sync sleep_list and related data access */
static struct semaphore sleep_sema;
/* List of all sleeping threads. Access only via sync of sleep_lock */
static struct list sleep_list;
/* Temp variable to house sleeping thread */
static struct sleeping_thread *temp_sl_thread;
/* Loop iterator variable for list */
static struct list_elem *e;

/* Cached value for the next wait_till time to awaken a sleeping thread.
   Equals NONE_WAITING if no thread is sleeping. */
static int64_t next_wait_till;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt.

   Also initilize list to maintain 'sleeping_list' of threads that called thread_sleep */
void
timer_init (void) 
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");

  next_wait_till = NO_WAIT;
  list_init (&sleep_list);
  sema_init(&sleep_sema,1);

}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on.

   Blocks current thread till atleast the elipsed tick passes.
   */
void
timer_sleep (int64_t t)
{

  ASSERT (intr_get_level () == INTR_ON);

  if(t <=0)
    return;

  /* Add current thread to sleep_list after setting it's WAIT_TILL value,
     and then calls thread_block() */
  sema_down (&sleep_sema);
  int64_t lticks = ticks;
  struct sleeping_thread *e = (struct sleeping_thread *) malloc (sizeof (struct sleeping_thread));
  ASSERT (e != NULL);
  e->t = thread_current ();
  e->wait_till = t + lticks;

  if (next_wait_till==NO_WAIT)
    {
      next_wait_till=e->wait_till;
    }
  else if (next_wait_till > e->wait_till)
    {
      next_wait_till=e->wait_till;
    }

  list_insert_ordered (&sleep_list, &e->elem, value_less, NULL);
  sema_up (&sleep_sema);

  enum intr_level old_level;
  old_level = intr_disable ();
  thread_block ();
  intr_set_level (old_level);
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  /*
      mlfqs calculations
      # timer_ticks() % TIMER_FREQ==0 #
      1. load_avg = (59/60)*load_avg + (1/60)*ready_threads
         ready_threads= number of thread runnning or ready to run
      2.recent_cpu = (2*load_avg)/(2*load_avg+1) *recent_cpu + nice [for all thread]
      #EVERY 4 TICKS#
      3. priority for all processes 	=PRI_MAX -(recent_cpu/4)- (nice*2)
      #EVERY TICK#
      4.thread_current -> recent_cpu ++ [for running thread only]
  */
  if (thread_mlfqs)
    {
      enum intr_level old_level;
      old_level = intr_disable ();
      struct thread *th = thread_current ();

      //after each tick
      thread_mlfqs_update_recent_cpu ();

      //Do every 4th tick calculations
      if (timer_ticks() % 4 == 0)
        thread_mlfqs_update_thread_priority ();

      //do every 1 second calculations
      //calculate ready_threads
      if(timer_ticks () % (TIMER_FREQ) == 0)
        thread_mlfqs_sec_calculate ();

      intr_set_level (old_level);
    }
  ticks++;
  thread_tick ();

  if (sema_try_down(&sleep_sema))
  {
    if (next_wait_till != NO_WAIT && next_wait_till <= ticks)
      {
        while (!list_empty(&sleep_list))
          {
            e = list_front (&sleep_list);
            temp_sl_thread = list_entry (e, struct sleeping_thread, elem);
            if (temp_sl_thread->wait_till > ticks)
              {
                next_wait_till = temp_sl_thread->wait_till;
                break;
              }
            else
              {
                //check if THREAD_DYING needs handling here
                //check if sync is req. here
                //check for loop termination conditions if the last elem in list is popped here
                ASSERT (temp_sl_thread->t != NULL)
                if (temp_sl_thread->t->status == THREAD_BLOCKED)
                  {
                    temp_sl_thread = list_entry (list_pop_front (&sleep_list), struct sleeping_thread, elem);
                    thread_unblock (temp_sl_thread->t);
                    //free(&temp_sl_thread);
                  }
              }
          }

        if (list_empty(&sleep_list))
          next_wait_till = NO_WAIT;
      }
    sema_up (&sleep_sema);
  }
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}

/* Returns true if value A is less than value B, false
   otherwise. */
static bool
value_less (const struct list_elem *a_, const struct list_elem *b_,
            void *aux UNUSED)
{
  const struct sleeping_thread *a = list_entry (a_, struct sleeping_thread, elem);
  const struct sleeping_thread *b = list_entry (b_, struct sleeping_thread, elem);

  return a->wait_till < b->wait_till;
}
