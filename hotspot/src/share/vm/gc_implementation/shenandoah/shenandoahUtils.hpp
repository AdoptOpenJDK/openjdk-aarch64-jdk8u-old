/*
 * Copyright (c) 2017, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAHUTILS_HPP
#define SHARE_VM_GC_SHENANDOAHUTILS_HPP

#include "runtime/vmThread.hpp"
#include "gc_interface/gcCause.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"
#include "memory/allocation.hpp"
#include "gc_implementation/shenandoah/shenandoahPhaseTimings.hpp"

class GCTimer;
class GCTracer;

class ShenandoahGCSession : public StackObj {
private:
  GCTimer*  const _timer;
  GCTracer* const _tracer;

  TraceMemoryManagerStats _trace_cycle;
public:
  ShenandoahGCSession(GCCause::Cause cause);
  ~ShenandoahGCSession();
};

class ShenandoahGCPhase : public StackObj {
private:
  const ShenandoahPhaseTimings::Phase   _phase;
public:
  ShenandoahGCPhase(ShenandoahPhaseTimings::Phase phase);
  ~ShenandoahGCPhase();
};

// Aggregates all the things that should happen before/after the pause.
class ShenandoahGCPauseMark : public StackObj {
private:
  const SvcGCMarker       _svc_gc_mark;
  const IsGCActiveMark    _is_gc_active_mark;
  TraceMemoryManagerStats _trace_pause;
public:
  ShenandoahGCPauseMark(SvcGCMarker::reason_type type);
  ~ShenandoahGCPauseMark();
};

class ShenandoahAllocTrace : public StackObj {
private:
  double _start;
  size_t _size;
  ShenandoahHeap::AllocType _alloc_type;
public:
  ShenandoahAllocTrace(size_t words_size, ShenandoahHeap::AllocType alloc_type);
  ~ShenandoahAllocTrace();
};

class ShenandoahSafepoint : public AllStatic {
public:
  // check if Shenandoah GC safepoint is in progress
  static inline bool is_at_shenandoah_safepoint() {
    if (!SafepointSynchronize::is_at_safepoint()) return false;

    VM_Operation* vm_op = VMThread::vm_operation();
    if (vm_op == NULL) return false;

    VM_Operation::VMOp_Type type = vm_op->type();
    return type == VM_Operation::VMOp_ShenandoahInitMark ||
           type == VM_Operation::VMOp_ShenandoahFinalMarkStartEvac ||
           type == VM_Operation::VMOp_ShenandoahFinalEvac ||
           type == VM_Operation::VMOp_ShenandoahInitUpdateRefs ||
           type == VM_Operation::VMOp_ShenandoahFinalUpdateRefs ||
           type == VM_Operation::VMOp_ShenandoahFullGC ||
           type == VM_Operation::VMOp_ShenandoahDegeneratedGC;
  }
};

class ShenandoahWorkerSession : public StackObj {
  static const uint INVALID_WORKER_ID = uint(-1);
public:
  ShenandoahWorkerSession(uint worker_id);
  ~ShenandoahWorkerSession();

  static inline uint worker_id() {
    Thread* thr = Thread::current();
    uint id = thr->worker_id();
    assert(id != INVALID_WORKER_ID, "Worker session has not been created");
    return id;
  }
};

#endif // SHARE_VM_GC_SHENANDOAHUTILS_HPP
