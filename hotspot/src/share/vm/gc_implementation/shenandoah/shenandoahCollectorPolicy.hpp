/*
 * Copyright (c) 2013, 2015, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHCOLLECTORPOLICY_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHCOLLECTORPOLICY_HPP

#include "gc_implementation/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/collectorPolicy.hpp"
#include "runtime/arguments.hpp"
#include "utilities/numberSeq.hpp"

class ShenandoahCollectionSet;
class ShenandoahFreeSet;
class ShenandoahHeap;
class ShenandoahHeuristics;

class STWGCTimer;
class ConcurrentGCTimer;
class outputStream;

class ShenandoahCollectorPolicy: public CollectorPolicy {
private:
  size_t _success_concurrent_gcs;
  size_t _success_degenerated_gcs;
  size_t _success_full_gcs;
  size_t _alloc_failure_degenerated;
  size_t _alloc_failure_degenerated_upgrade_to_full;
  size_t _alloc_failure_full;
  size_t _explicit_concurrent;
  size_t _explicit_full;
  size_t _degen_points[ShenandoahHeap::_DEGENERATED_LIMIT];

  ShenandoahSharedFlag _in_shutdown;

  ShenandoahTracer* _tracer;

  size_t _cycle_counter;


public:
  ShenandoahCollectorPolicy();

  void post_heap_initialize() {};

  BarrierSet::Name barrier_set_name();

  HeapWord* mem_allocate_work(size_t size,
                              bool is_tlab,
                              bool* gc_overhead_limit_was_exceeded);

  HeapWord* satisfy_failed_allocation(size_t size, bool is_tlab);

  void initialize_alignments();

  // TODO: This is different from gc_end: that one encompasses one VM operation.
  // These two encompass the entire cycle.
  void record_cycle_start();

  void record_success_concurrent();
  void record_success_degenerated();
  void record_success_full();
  void record_alloc_failure_to_degenerated(ShenandoahHeap::ShenandoahDegenPoint point);
  void record_alloc_failure_to_full();
  void record_degenerated_upgrade_to_full();
  void record_explicit_to_concurrent();
  void record_explicit_to_full();

  void record_shutdown();
  bool is_at_shutdown();

  ShenandoahTracer* tracer() {return _tracer;}

  size_t cycle_counter() const;

  void print_gc_stats(outputStream* out) const;
};


#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCOLLECTORPOLICY_HPP
