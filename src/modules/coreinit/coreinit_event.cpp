#include "coreinit.h"
#include "coreinit_alarm.h"
#include "coreinit_event.h"
#include "coreinit_memheap.h"
#include "coreinit_scheduler.h"
#include "system.h"
#include "ppcutils/stackobject.h"

namespace coreinit
{

const uint32_t
OSEvent::Tag;


/**
 * Initialises an event structure.
 */
void
OSInitEvent(OSEvent *event, bool value, OSEventMode mode)
{
   OSInitEventEx(event, value, mode, nullptr);
}


/**
 * Initialises an event structure.
 */
void
OSInitEventEx(OSEvent *event, bool value, OSEventMode mode, char *name)
{
   event->tag = OSEvent::Tag;
   event->mode = mode;
   event->value = value;
   event->name = name;
   OSInitThreadQueueEx(&event->queue, event);
}

namespace internal
{

void signalEventNoLock(OSEvent *event)
{
   assert(event);
   assert(event->tag == OSEvent::Tag);

   if (event->value != FALSE) {
      // Event has already been set
      internal::unlockScheduler();
      return;
   }

   // Set event
   event->value = TRUE;

   if (!OSIsThreadQueueEmpty(&event->queue)) {
      if (event->mode == OSEventMode::AutoReset) {
         // Reset value
         event->value = FALSE;

         // Wakeup one thread
         auto thread = OSPopFrontThreadQueue(&event->queue);

         // Cancel timeout alarm
         if (thread->waitEventTimeoutAlarm) {
            internal::cancelAlarm(thread->waitEventTimeoutAlarm);
         }

         internal::wakeupOneThreadNoLock(thread);
         internal::rescheduleAllCoreNoLock();
      } else {
         // Cancel any pending timeout alarms
         for (auto thread = event->queue.head; thread; thread = thread->link.next) {
            if (thread->waitEventTimeoutAlarm) {
               internal::cancelAlarm(thread->waitEventTimeoutAlarm);
            }
         }

         // Wakeup all threads
         internal::wakeupThreadNoLock(&event->queue);
         internal::rescheduleAllCoreNoLock();
      }
   }
}

}


/**
 * Signal an event.
 *
 * This will set the events signal value to true.
 *
 * In auto reset mode if at least one thread is in the queue it will:
 * - Reset the value back to FALSE
 * - Wake up the first thread in the waiting queue
 *
 * In manual reset mode:
 * - Wake up all threads in the waiting queue
 * - The event value remains TRUE until the user calls OSResetEvent
 */
void
OSSignalEvent(OSEvent *event)
{
   internal::lockScheduler();
   internal::signalEventNoLock(event);
   internal::unlockScheduler();
}


/**
 * Signal the event and wakeup all waiting threads regardless of mode
 *
 * This differs to OSSignalEvent only for auto reset mode where it
 * will wake up all threads instead of just one.
 *
 * If there is at least one thread woken up and the alarm is in
 * auto reset mode then the event value is reset to FALSE.
 */
void
OSSignalEventAll(OSEvent *event)
{
   internal::lockScheduler();
   assert(event);
   assert(event->tag == OSEvent::Tag);

   if (event->value != FALSE) {
      // Event has already been set
      internal::unlockScheduler();
      return;
   }

   // Set event
   event->value = TRUE;

   if (!OSIsThreadQueueEmpty(&event->queue)) {
      if (event->mode == OSEventMode::AutoReset) {
         // Reset event
         event->value = FALSE;
      }

      // Cancel any pending timeout alarms
      for (auto thread = event->queue.head; thread; thread = thread->link.next) {
         if (thread->waitEventTimeoutAlarm) {
            internal::cancelAlarm(thread->waitEventTimeoutAlarm);
         }
      }

      // Wakeup all threads
      internal::wakeupThreadNoLock(&event->queue);
      internal::rescheduleAllCoreNoLock();
   }

   internal::unlockScheduler();
}


/**
 * Reset the event value to FALSE
 */
void
OSResetEvent(OSEvent *event)
{
   internal::lockScheduler();
   assert(event);
   assert(event->tag == OSEvent::Tag);

   // Reset event
   event->value = FALSE;

   internal::unlockScheduler();
}


/**
 * Wait for the event value to become TRUE.
 *
 * If the event value is already TRUE then this will return immediately,
 * and will set the event value to FALSE if the event is in auto reset mode.
 *
 * If the event value is FALSE then the thread will sleep until the event
 * is signalled by another thread.
 */
void
OSWaitEvent(OSEvent *event)
{
   internal::lockScheduler();
   assert(event);
   assert(event->tag == OSEvent::Tag);

   if (event->value) {
      // Event is already set

      if (event->mode == OSEventMode::AutoReset) {
         // Reset event
         event->value = FALSE;
      }

      internal::unlockScheduler();
      return;
   } else {
      // Wait for event to be set
      internal::sleepThreadNoLock(&event->queue);
      internal::rescheduleSelfNoLock();
   }

   internal::unlockScheduler();
}


static AlarmCallback
pEventAlarmHandler = nullptr;

struct EventAlarmData
{
   OSEvent *event;
   OSThread *thread;
   BOOL timeout;
};

static void
EventAlarmHandler(OSAlarm *alarm, OSContext *context)
{
   // Wakeup the thread waiting on this alarm
   auto data = reinterpret_cast<EventAlarmData*>(OSGetAlarmUserData(alarm));
   data->timeout = TRUE;
   internal::wakeupOneThreadNoLock(data->thread);
}


/**
 * Wait for an event value to be TRUE with a timeout
 *
 * Behaves the same than OSWaitEvent but with a timeout.
 *
 * Returns TRUE if the event was signalled, FALSE if wait timed out.
 */
BOOL
OSWaitEventWithTimeout(OSEvent *event, OSTime timeout)
{
   ppcutils::StackObject<EventAlarmData> data;
   ppcutils::StackObject<OSAlarm> alarm;

   // TODO: Brett check logic

   internal::lockScheduler();

   // Check if event is already set
   if (event->value) {
      if (event->mode == OSEventMode::AutoReset) {
         // Reset event
         event->value = FALSE;
      }

      internal::unlockScheduler();
      return TRUE;
   }

   // Setup some alarm data for callback
   auto thread = OSGetCurrentThread();
   data->event = event;
   data->thread = thread;
   data->timeout = FALSE;

   // Set waitEventTimeoutAlarm so we can cancel it when event is signalled
   thread->waitEventTimeoutAlarm = alarm;

   // Create an alarm to trigger timeout
   OSCreateAlarm(alarm);
   OSSetAlarmUserData(alarm, data);
   OSSetAlarm(alarm, timeout, pEventAlarmHandler);

   // Wait for the event
   internal::sleepThreadNoLock(&event->queue);
   internal::rescheduleAllCoreNoLock();

   // Clear waitEventTimeoutAlarm
   thread->waitEventTimeoutAlarm = nullptr;

   BOOL result = TRUE;

   if (data->timeout) {
      // Timed out, remove from wait queue
      OSEraseFromThreadQueue(&event->queue, thread);
      result = FALSE;
   }

   internal::unlockScheduler();
   return result;
}

void
Module::registerEventFunctions()
{
   RegisterKernelFunction(OSInitEvent);
   RegisterKernelFunction(OSInitEventEx);
   RegisterKernelFunction(OSSignalEvent);
   RegisterKernelFunction(OSSignalEventAll);
   RegisterKernelFunction(OSResetEvent);
   RegisterKernelFunction(OSWaitEvent);
   RegisterKernelFunction(OSWaitEventWithTimeout);
   RegisterKernelFunction(EventAlarmHandler);
}

void
Module::initialiseEvent()
{
   pEventAlarmHandler = findExportAddress("EventAlarmHandler");
}

} // namespace coreinit
