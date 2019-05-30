/*
 * Copyright (c) 2017, 2018, Red Hat, Inc. All rights reserved.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

#include "gc_implementation/shared/suspendibleThreadSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahLogging.hpp"
#include "gc_implementation/shenandoah/shenandoahStrDedupQueue.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahStrDedupThread.hpp"
#include "gc_implementation/shenandoah/shenandoahStringDedup.hpp"
#include "gc_implementation/shenandoah/shenandoahUtils.hpp"

ShenandoahStrDedupThread::ShenandoahStrDedupThread(ShenandoahStrDedupQueueSet* queues) :
  ConcurrentGCThread(), _queues(queues), _claimed(0) {
  size_t num_queues = queues->num_queues();
  _work_list = NEW_C_HEAP_ARRAY(QueueChunkedList*, num_queues, mtGC);
  for (size_t index = 0; index < num_queues; index ++) {
    _work_list[index] = NULL;
  }

  set_name("%s", "ShenandoahStringDedupTherad");
  create_and_start();
}

ShenandoahStrDedupThread::~ShenandoahStrDedupThread() {
  ShouldNotReachHere();
}

void ShenandoahStrDedupThread::run() {
  for (;;) {
    ShenandoahStrDedupStats stats;

    assert(is_work_list_empty(), "Work list must be empty");
    // Queue has been shutdown
    if (!poll(&stats)) {
      assert(queues()->has_terminated(), "Must be terminated");
      break;
    }

    // Include thread in safepoints
    SuspendibleThreadSetJoiner sts_join;
    // Process the queue
    for (uint queue_index = 0; queue_index < queues()->num_queues(); queue_index ++) {
      QueueChunkedList* cur_list = _work_list[queue_index];

      while (cur_list != NULL) {
        stats.mark_exec();

        while (!cur_list->is_empty()) {
          oop java_string = cur_list->pop();
          stats.inc_inspected();
          assert(!ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must not at Shenandoah safepoint");

          if (oopDesc::is_null(java_string) ||
              !ShenandoahStringDedup::is_candidate(java_string)) {
            stats.inc_skipped();
          } else {
            if (ShenandoahStringDedup::deduplicate(java_string, false /* update counter */)) {
              stats.inc_deduped();
            } else {
              stats.inc_known();
            }
          }

          // Safepoint this thread if needed
          if (sts_join.should_yield()) {
            stats.mark_block();
            sts_join.yield();
            stats.mark_unblock();
          }
        }

        // Advance list only after processed. Otherwise, we may miss scanning
        // during safepoints
        _work_list[queue_index] = cur_list->next();
        queues()->release_chunked_list(cur_list);
        cur_list = _work_list[queue_index];
      }
    }

    stats.mark_done();

    ShenandoahStringDedup::dedup_stats().update(stats);

    if (ShenandoahLogDebug) {
      stats.print_statistics(tty);
    }
  }

  if (ShenandoahLogDebug) {
    ShenandoahStringDedup::print_statistics(tty);
  }
}

void ShenandoahStrDedupThread::stop() {
  queues()->terminate();
}

void ShenandoahStrDedupThread::parallel_oops_do(OopClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  size_t claimed_index;
  while ((claimed_index = claim()) < queues()->num_queues()) {
    QueueChunkedList* q = _work_list[claimed_index];
    while (q != NULL) {
      q->oops_do(cl);
      q = q->next();
    }
  }
}

void ShenandoahStrDedupThread::oops_do_slow(OopClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  for (size_t index = 0; index < queues()->num_queues(); index ++) {
    QueueChunkedList* q = _work_list[index];
    while (q != NULL) {
      q->oops_do(cl);
      q = q->next();
    }
  }
}

bool ShenandoahStrDedupThread::is_work_list_empty() const {
  assert(Thread::current() == this, "Only from dedup thread");
  for (uint index = 0; index < queues()->num_queues(); index ++) {
    if (_work_list[index] != NULL) return false;
  }
  return true;
}

void ShenandoahStrDedupThread::parallel_cleanup() {
  ShenandoahStrDedupQueueCleanupClosure cl;
  parallel_oops_do(&cl);
}

bool ShenandoahStrDedupThread::poll(ShenandoahStrDedupStats* stats) {
  assert(is_work_list_empty(), "Only poll when work list is empty");

  while (!_queues->has_terminated()) {
    {
      bool has_work = false;
      stats->mark_exec();
      // Include thread in safepoints
      SuspendibleThreadSetJoiner sts_join;

      for (uint index = 0; index < queues()->num_queues(); index ++) {
        assert(!ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Not at Shenandoah Safepoint");
        _work_list[index] = queues()->remove_work_list_atomic(index);
        if (_work_list[index] != NULL) {
          has_work = true;
        }

        // Safepoint this thread if needed
        if (sts_join.should_yield()) {
          stats->mark_block();
          sts_join.yield();
          stats->mark_unblock();
        }
      }

      if (has_work) return true;
    }

    {
      stats->mark_idle();
      MonitorLockerEx locker(queues()->lock(), Monitor::_no_safepoint_check_flag);
      locker.wait(Mutex::_no_safepoint_check_flag);
    }
  }
  return false;
}

size_t ShenandoahStrDedupThread::claim() {
  size_t index = (size_t)Atomic::add(1, (volatile jint*)&_claimed) - 1;
  return index;
}
