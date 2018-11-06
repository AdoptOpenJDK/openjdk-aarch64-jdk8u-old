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

#include "precompiled.hpp"
#include "memory/allocation.hpp"

#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shenandoah/shenandoahGCTraceTime.hpp"
#include "gc_implementation/shared/parallelCleaning.hpp"

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahAllocTracker.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahControlThread.hpp"
#include "gc_implementation/shenandoah/shenandoahFreeSet.hpp"
#include "gc_implementation/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc_implementation/shenandoah/shenandoahMetrics.hpp"
#include "gc_implementation/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahPacer.hpp"
#include "gc_implementation/shenandoah/shenandoahPacer.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/shenandoahUtils.hpp"
#include "gc_implementation/shenandoah/shenandoahVerifier.hpp"
#include "gc_implementation/shenandoah/shenandoahCodeRoots.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkGroup.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkerPolicy.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "gc_implementation/shenandoah/heuristics/shenandoahAdaptiveHeuristics.hpp"
#include "gc_implementation/shenandoah/heuristics/shenandoahAggressiveHeuristics.hpp"
#include "gc_implementation/shenandoah/heuristics/shenandoahCompactHeuristics.hpp"
#include "gc_implementation/shenandoah/heuristics/shenandoahPassiveHeuristics.hpp"
#include "gc_implementation/shenandoah/heuristics/shenandoahStaticHeuristics.hpp"

#include "memory/metaspace.hpp"
#include "runtime/vmThread.hpp"
#include "services/mallocTracker.hpp"

ShenandoahUpdateRefsClosure::ShenandoahUpdateRefsClosure() : _heap(ShenandoahHeap::heap()) {}

#ifdef ASSERT
template <class T>
void ShenandoahAssertToSpaceClosure::do_oop_nv(T* p) {
  T o = oopDesc::load_heap_oop(p);
  if (! oopDesc::is_null(o)) {
    oop obj = oopDesc::decode_heap_oop_not_null(o);
    shenandoah_assert_not_forwarded(p, obj);
  }
}

void ShenandoahAssertToSpaceClosure::do_oop(narrowOop* p) { do_oop_nv(p); }
void ShenandoahAssertToSpaceClosure::do_oop(oop* p)       { do_oop_nv(p); }
#endif

const char* ShenandoahHeap::name() const {
  return "Shenandoah";
}

class ShenandoahPretouchTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator _regions;
  const size_t _bitmap_size;
  const size_t _page_size;
  char* _bitmap0_base;
  char* _bitmap1_base;
public:
  ShenandoahPretouchTask(char* bitmap0_base, char* bitmap1_base, size_t bitmap_size,
                         size_t page_size) :
    AbstractGangTask("Shenandoah PreTouch"),
    _bitmap_size(bitmap_size),
    _page_size(page_size),
    _bitmap0_base(bitmap0_base),
    _bitmap1_base(bitmap1_base) {}

  virtual void work(uint worker_id) {
    ShenandoahHeapRegion* r = _regions.next();
    while (r != NULL) {
      os::pretouch_memory((char*) r->bottom(), (char*) r->end());

      size_t start = r->region_number()       * ShenandoahHeapRegion::region_size_bytes() / MarkBitMap::heap_map_factor();
      size_t end   = (r->region_number() + 1) * ShenandoahHeapRegion::region_size_bytes() / MarkBitMap::heap_map_factor();
      assert (end <= _bitmap_size, err_msg("end is sane: " SIZE_FORMAT " < " SIZE_FORMAT, end, _bitmap_size));

      os::pretouch_memory(_bitmap0_base + start, _bitmap0_base + end);
      os::pretouch_memory(_bitmap1_base + start, _bitmap1_base + end);

      r = _regions.next();
    }
  }
};

jint ShenandoahHeap::initialize() {
  CollectedHeap::pre_initialize();

  BrooksPointer::initial_checks();

  initialize_heuristics();

  size_t init_byte_size = collector_policy()->initial_heap_byte_size();
  size_t max_byte_size = collector_policy()->max_heap_byte_size();
  size_t heap_alignment = collector_policy()->heap_alignment();

  if (ShenandoahAlwaysPreTouch) {
    // Enabled pre-touch means the entire heap is committed right away.
    init_byte_size = max_byte_size;
  }

  Universe::check_alignment(max_byte_size,
                            ShenandoahHeapRegion::region_size_bytes(),
                            "shenandoah heap");
  Universe::check_alignment(init_byte_size,
                            ShenandoahHeapRegion::region_size_bytes(),
                            "shenandoah heap");

  ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size,
                                                 heap_alignment);

  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)heap_rs.base());
  _reserved.set_end((HeapWord*)(heap_rs.base() + heap_rs.size()));

  set_barrier_set(new ShenandoahBarrierSet(this));
  ReservedSpace pgc_rs = heap_rs.first_part(max_byte_size);

  _num_regions = ShenandoahHeapRegion::region_count();
  size_t num_committed_regions = init_byte_size / ShenandoahHeapRegion::region_size_bytes();
  _initial_size = num_committed_regions * ShenandoahHeapRegion::region_size_bytes();
  _committed = _initial_size;

  log_info(gc, heap)("Initialize Shenandoah heap with initial size " SIZE_FORMAT " bytes", init_byte_size);
  if (!os::commit_memory(pgc_rs.base(), _initial_size, false)) {
    vm_exit_out_of_memory(_initial_size, OOM_MMAP_ERROR, "Shenandoah failed to initialize heap");
  }

  size_t reg_size_words = ShenandoahHeapRegion::region_size_words();
  size_t reg_size_bytes = ShenandoahHeapRegion::region_size_bytes();

  _regions = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, _num_regions, mtGC);
  _free_set = new ShenandoahFreeSet(this, _num_regions);

  _collection_set = new ShenandoahCollectionSet(this, (HeapWord*)pgc_rs.base());

  if (ShenandoahPacing) {
    _pacer = new ShenandoahPacer(this);
    _pacer->setup_for_idle();
  } else {
    _pacer = NULL;
  }

  assert((((size_t) base()) & ShenandoahHeapRegion::region_size_bytes_mask()) == 0,
         err_msg("misaligned heap: "PTR_FORMAT, p2i(base())));

  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               20 /*G1SATBProcessCompletedThreshold */,
                                               Shared_SATB_Q_lock);

  // Reserve space for prev and next bitmap.
  size_t bitmap_page_size = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();
  _bitmap_size = MarkBitMap::compute_size(heap_rs.size());
  _bitmap_size = align_size_up(_bitmap_size, bitmap_page_size);
  _heap_region = MemRegion((HeapWord*) heap_rs.base(), heap_rs.size() / HeapWordSize);

  size_t bitmap_bytes_per_region = reg_size_bytes / MarkBitMap::heap_map_factor();

  guarantee(bitmap_bytes_per_region != 0,
            err_msg("Bitmap bytes per region should not be zero"));
  guarantee(is_power_of_2(bitmap_bytes_per_region),
            err_msg("Bitmap bytes per region should be power of two: " SIZE_FORMAT, bitmap_bytes_per_region));

  if (bitmap_page_size > bitmap_bytes_per_region) {
    _bitmap_regions_per_slice = bitmap_page_size / bitmap_bytes_per_region;
    _bitmap_bytes_per_slice = bitmap_page_size;
  } else {
    _bitmap_regions_per_slice = 1;
    _bitmap_bytes_per_slice = bitmap_bytes_per_region;
  }

  guarantee(_bitmap_regions_per_slice >= 1,
            err_msg("Should have at least one region per slice: " SIZE_FORMAT,
                    _bitmap_regions_per_slice));

  guarantee(((_bitmap_bytes_per_slice) % bitmap_page_size) == 0,
            err_msg("Bitmap slices should be page-granular: bps = " SIZE_FORMAT ", page size = " SIZE_FORMAT,
                    _bitmap_bytes_per_slice, bitmap_page_size));

  ReservedSpace bitmap0(_bitmap_size, bitmap_page_size);
  MemTracker::record_virtual_memory_type(bitmap0.base(), mtGC);
  _bitmap0_region = MemRegion((HeapWord*) bitmap0.base(), bitmap0.size() / HeapWordSize);

  ReservedSpace bitmap1(_bitmap_size, bitmap_page_size);
  MemTracker::record_virtual_memory_type(bitmap1.base(), mtGC);
  _bitmap1_region = MemRegion((HeapWord*) bitmap1.base(), bitmap1.size() / HeapWordSize);

  size_t bitmap_init_commit = _bitmap_bytes_per_slice *
                              align_size_up(num_committed_regions, _bitmap_regions_per_slice) / _bitmap_regions_per_slice;
  bitmap_init_commit = MIN2(_bitmap_size, bitmap_init_commit);
  os::commit_memory_or_exit((char *) (_bitmap0_region.start()), bitmap_init_commit, false,
                            "couldn't allocate initial bitmap");
  os::commit_memory_or_exit((char *) (_bitmap1_region.start()), bitmap_init_commit, false,
                            "couldn't allocate initial bitmap");

  size_t page_size = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();

  if (ShenandoahVerify) {
    ReservedSpace verify_bitmap(_bitmap_size, page_size);
    os::commit_memory_or_exit(verify_bitmap.base(), verify_bitmap.size(), false,
                              "couldn't allocate verification bitmap");
    MemTracker::record_virtual_memory_type(verify_bitmap.base(), mtGC);
    MemRegion verify_bitmap_region = MemRegion((HeapWord *) verify_bitmap.base(), verify_bitmap.size() / HeapWordSize);
    _verification_bit_map.initialize(_heap_region, verify_bitmap_region);
    _verifier = new ShenandoahVerifier(this, &_verification_bit_map);
  }

  _complete_marking_context = new ShenandoahMarkingContext(_heap_region, _bitmap0_region, _num_regions);
  _next_marking_context = new ShenandoahMarkingContext(_heap_region, _bitmap1_region, _num_regions);

  {
    ShenandoahHeapLocker locker(lock());
    for (size_t i = 0; i < _num_regions; i++) {
      ShenandoahHeapRegion* r = new ShenandoahHeapRegion(this,
                                                         (HeapWord*) pgc_rs.base() + reg_size_words * i,
                                                         reg_size_words,
                                                         i,
                                                         i < num_committed_regions);

      _complete_marking_context->set_top_at_mark_start(i, r->bottom());
      _next_marking_context->set_top_at_mark_start(i, r->bottom());
      _regions[i] = r;
      assert(!collection_set()->is_in(i), "New region should not be in collection set");
    }

    _free_set->rebuild();
  }

  if (ShenandoahAlwaysPreTouch) {
    assert (!AlwaysPreTouch, "Should have been overridden");

    // For NUMA, it is important to pre-touch the storage under bitmaps with worker threads,
    // before initialize() below zeroes it with initializing thread. For any given region,
    // we touch the region and the corresponding bitmaps from the same thread.
    ShenandoahPushWorkerScope scope(workers(), _max_workers, false);

    log_info(gc, heap)("Parallel pretouch " SIZE_FORMAT " regions with " SIZE_FORMAT " byte pages",
                       _num_regions, page_size);
    ShenandoahPretouchTask cl(bitmap0.base(), bitmap1.base(), _bitmap_size, page_size);
    _workers->run_task(&cl);
  }


  // Reserve aux bitmap for use in object_iterate(). We don't commit it here.
  ReservedSpace aux_bitmap(_bitmap_size, bitmap_page_size);
  MemTracker::record_virtual_memory_type(aux_bitmap.base(), mtGC);
  _aux_bitmap_region = MemRegion((HeapWord*) aux_bitmap.base(), aux_bitmap.size() / HeapWordSize);
  _aux_bit_map.initialize(_heap_region, _aux_bitmap_region);

  _monitoring_support = new ShenandoahMonitoringSupport(this);

  _phase_timings = new ShenandoahPhaseTimings();

  if (ShenandoahAllocationTrace) {
    _alloc_tracker = new ShenandoahAllocTracker();
  }

  ShenandoahStringDedup::initialize();

  _control_thread = new ShenandoahControlThread();

  ShenandoahCodeRoots::initialize();

  return JNI_OK;
}

#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable:4355 ) // 'this' : used in base member initializer list
#endif

void ShenandoahHeap::initialize_heuristics() {
  if (ShenandoahGCHeuristics != NULL) {
    if (strcmp(ShenandoahGCHeuristics, "aggressive") == 0) {
      _heuristics = new ShenandoahAggressiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "static") == 0) {
      _heuristics = new ShenandoahStaticHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "adaptive") == 0) {
      _heuristics = new ShenandoahAdaptiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "passive") == 0) {
      _heuristics = new ShenandoahPassiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "compact") == 0) {
      _heuristics = new ShenandoahCompactHeuristics();
    } else {
      vm_exit_during_initialization("Unknown -XX:ShenandoahGCHeuristics option");
    }

    if (_heuristics->is_diagnostic() && !UnlockDiagnosticVMOptions) {
      vm_exit_during_initialization(
              err_msg("Heuristics \"%s\" is diagnostic, and must be enabled via -XX:+UnlockDiagnosticVMOptions.",
                      _heuristics->name()));
    }
    if (_heuristics->is_experimental() && !UnlockExperimentalVMOptions) {
      vm_exit_during_initialization(
              err_msg("Heuristics \"%s\" is experimental, and must be enabled via -XX:+UnlockExperimentalVMOptions.",
                      _heuristics->name()));
    }
    log_info(gc, init)("Shenandoah heuristics: %s",
                       _heuristics->name());
  } else {
    ShouldNotReachHere();
  }
}

ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) :
  SharedHeap(policy),
  _shenandoah_policy(policy),
  _regions(NULL),
  _free_set(NULL),
  _collection_set(NULL),
  _update_refs_iterator(this),
  _bytes_allocated_since_gc_start(0),
  _max_workers((uint)MAX2(ConcGCThreads, ParallelGCThreads)),
  _ref_processor(NULL),
  _complete_marking_context(NULL),
  _next_marking_context(NULL),
  _aux_bit_map(),
  _verifier(NULL),
  _pacer(NULL),
#ifdef ASSERT
  _heap_expansion_count(0),
#endif
  _gc_timer(new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer()),
  _phase_timings(NULL),
  _alloc_tracker(NULL)
{
  log_info(gc, init)("GC threads: " UINTX_FORMAT " parallel, " UINTX_FORMAT " concurrent", ParallelGCThreads, ConcGCThreads);
  log_info(gc, init)("Reference processing: %s", ParallelRefProcEnabled ? "parallel" : "serial");

  _scm = new ShenandoahConcurrentMark();
  _full_gc = new ShenandoahMarkCompact();
  _used = 0;

  _max_workers = MAX2(_max_workers, 1U);
  _workers = new ShenandoahWorkGang("Shenandoah GC Threads", _max_workers,
                            /* are_GC_task_threads */true,
                            /* are_ConcurrentGC_threads */false);
  if (_workers == NULL) {
    vm_exit_during_initialization("Failed necessary allocation.");
  } else {
    _workers->initialize_workers();
  }
}

#ifdef _MSC_VER
#pragma warning( pop )
#endif

class ShenandoahResetNextBitmapTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator _regions;

public:
  ShenandoahResetNextBitmapTask() :
    AbstractGangTask("Parallel Reset Bitmap Task") {}

  void work(uint worker_id) {
    ShenandoahHeapRegion* region = _regions.next();
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahMarkingContext* const ctx = heap->next_marking_context();
    while (region != NULL) {
      if (heap->is_bitmap_slice_committed(region)) {
        HeapWord* bottom = region->bottom();
        HeapWord* top = ctx->top_at_mark_start(region->region_number());
        if (top > bottom) {
          ctx->clear_bitmap(bottom, top);
        }
        assert(ctx->is_bitmap_clear_range(bottom, region->end()), "must be clear");
      }
      region = _regions.next();
    }
  }
};

void ShenandoahHeap::reset_next_mark_bitmap() {
  assert_gc_workers(_workers->active_workers());

  ShenandoahResetNextBitmapTask task;
  _workers->run_task(&task);
}

void ShenandoahHeap::print_on(outputStream* st) const {
  st->print_cr("Shenandoah Heap");
  st->print_cr(" " SIZE_FORMAT "K total, " SIZE_FORMAT "K committed, " SIZE_FORMAT "K used",
               capacity() / K, committed() / K, used() / K);
  st->print_cr(" " SIZE_FORMAT " x " SIZE_FORMAT"K regions",
               num_regions(), ShenandoahHeapRegion::region_size_bytes() / K);

  st->print("Status: ");
  if (has_forwarded_objects())               st->print("has forwarded objects, ");
  if (is_concurrent_mark_in_progress())      st->print("marking, ");
  if (is_evacuation_in_progress())           st->print("evacuating, ");
  if (is_update_refs_in_progress())          st->print("updating refs, ");
  if (is_degenerated_gc_in_progress())       st->print("degenerated gc, ");
  if (is_full_gc_in_progress())              st->print("full gc, ");
  if (is_full_gc_move_in_progress())         st->print("full gc move, ");

  if (cancelled_gc()) {
    st->print("cancelled");
  } else {
    st->print("not cancelled");
  }
  st->cr();

  st->print_cr("Reserved region:");
  st->print_cr(" - [" PTR_FORMAT ", " PTR_FORMAT ") ",
               p2i(reserved_region().start()),
               p2i(reserved_region().end()));

  st->cr();
  MetaspaceAux::print_on(st);

  if (Verbose) {
    print_heap_regions_on(st);
  }
}

class ShenandoahInitGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    if (thread != NULL && (thread->is_Java_thread() || thread->is_Worker_thread())) {
      thread->gclab().initialize(true);
    }
  }
};

void ShenandoahHeap::post_initialize() {
  if (UseTLAB) {
    MutexLocker ml(Threads_lock);

    ShenandoahInitGCLABClosure init_gclabs;
    Threads::java_threads_do(&init_gclabs);
    _workers->threads_do(&init_gclabs);
  }

  _scm->initialize(_max_workers);
  _full_gc->initialize(_gc_timer);

  ref_processing_init();

  _heuristics->initialize();
}

size_t ShenandoahHeap::used() const {
  OrderAccess::acquire();
  return (size_t) _used;
}

size_t ShenandoahHeap::committed() const {
  OrderAccess::acquire();
  return _committed;
}

void ShenandoahHeap::increase_committed(size_t bytes) {
  assert_heaplock_or_safepoint();
  _committed += bytes;
}

void ShenandoahHeap::decrease_committed(size_t bytes) {
  assert_heaplock_or_safepoint();
  _committed -= bytes;
}

void ShenandoahHeap::increase_used(size_t bytes) {
  Atomic::add(bytes, &_used);
}

void ShenandoahHeap::set_used(size_t bytes) {
  OrderAccess::release_store_fence(&_used, bytes);
}

void ShenandoahHeap::decrease_used(size_t bytes) {
  assert(used() >= bytes, "never decrease heap size by more than we've left");
  Atomic::add(-(jlong)bytes, &_used);
}

void ShenandoahHeap::increase_allocated(size_t bytes) {
  Atomic::add(bytes, &_bytes_allocated_since_gc_start);
}

void ShenandoahHeap::notify_mutator_alloc_words(size_t words, bool waste) {
  size_t bytes = words * HeapWordSize;
  if (!waste) {
    increase_used(bytes);
  }
  increase_allocated(bytes);
  if (ShenandoahPacing) {
    control_thread()->pacing_notify_alloc(words);
    if (waste) {
      pacer()->claim_for_alloc(words, true);
    }
  }
}

size_t ShenandoahHeap::capacity() const {
  return num_regions() * ShenandoahHeapRegion::region_size_bytes();
}

bool ShenandoahHeap::is_maximal_no_gc() const {
  Unimplemented();
  return true;
}

size_t ShenandoahHeap::max_capacity() const {
  return _num_regions * ShenandoahHeapRegion::region_size_bytes();
}

size_t ShenandoahHeap::initial_capacity() const {
  return _initial_size;
}

bool ShenandoahHeap::is_in(const void* p) const {
  HeapWord* heap_base = (HeapWord*) base();
  HeapWord* last_region_end = heap_base + ShenandoahHeapRegion::region_size_words() * num_regions();
  return p >= heap_base && p < last_region_end;
}

bool ShenandoahHeap::is_in_partial_collection(const void* p ) {
  Unimplemented();
  return false;
}

bool ShenandoahHeap::is_scavengable(const void* p) {
  return true;
}

void ShenandoahHeap::op_uncommit(double shrink_before) {
  assert (ShenandoahUncommit, "should be enabled");

  size_t count = 0;
  for (size_t i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* r = get_region(i);
    if (r->is_empty_committed() && (r->empty_time() < shrink_before)) {
      ShenandoahHeapLocker locker(lock());
      if (r->is_empty_committed()) {
        r->make_uncommitted();
        count++;
      }
    }
    SpinPause(); // allow allocators to take the lock
  }

  if (count > 0) {
    log_info(gc)("Uncommitted " SIZE_FORMAT "M. Heap: " SIZE_FORMAT "M reserved, " SIZE_FORMAT "M committed, " SIZE_FORMAT "M used",
                 count * ShenandoahHeapRegion::region_size_bytes() / M, capacity() / M, committed() / M, used() / M);
    _control_thread->notify_heap_changed();
  }
}

HeapWord* ShenandoahHeap::allocate_from_gclab_slow(Thread* thread, size_t size) {
  // Retain tlab and allocate object in shared space if
  // the amount free in the tlab is too large to discard.
  if (thread->gclab().free() > thread->gclab().refill_waste_limit()) {
    thread->gclab().record_slow_allocation(size);
    return NULL;
  }

  // Discard gclab and allocate a new one.
  // To minimize fragmentation, the last GCLAB may be smaller than the rest.
  size_t new_gclab_size = thread->gclab().compute_size(size);

  thread->gclab().clear_before_allocation();

  if (new_gclab_size == 0) {
    return NULL;
  }

  // Allocated object should fit in new GCLAB, and new_gclab_size should be larger than min
  size_t min_size = MAX2(size + ThreadLocalAllocBuffer::alignment_reserve(), ThreadLocalAllocBuffer::min_size());
  new_gclab_size = MAX2(new_gclab_size, min_size);

  // Allocate a new GCLAB...
  size_t actual_size = 0;
  HeapWord* obj = allocate_new_gclab(min_size, new_gclab_size, &actual_size);

  if (obj == NULL) {
    return NULL;
  }

  assert (size <= actual_size, "allocation should fit");

  if (ZeroTLAB) {
    // ..and clear it.
    Copy::zero_to_words(obj, actual_size);
  } else {
    // ...and zap just allocated object.
#ifdef ASSERT
    // Skip mangling the space corresponding to the object header to
    // ensure that the returned space is not considered parsable by
    // any concurrent GC thread.
    size_t hdr_size = oopDesc::header_size();
    Copy::fill_to_words(obj + hdr_size, actual_size - hdr_size, badHeapWordVal);
#endif // ASSERT
  }
  thread->gclab().fill(obj, obj + size, actual_size);
  return obj;
}

HeapWord* ShenandoahHeap::allocate_new_tlab(size_t word_size) {
  ShenandoahAllocationRequest req = ShenandoahAllocationRequest::for_tlab(word_size);
  return allocate_memory(req);
}

HeapWord* ShenandoahHeap::allocate_new_gclab(size_t min_size,
                                             size_t word_size,
                                             size_t* actual_size) {
  ShenandoahAllocationRequest req = ShenandoahAllocationRequest::for_gclab(min_size, word_size);
  HeapWord* res = allocate_memory(req);
  if (res != NULL) {
    *actual_size = req.actual_size();
  } else {
    *actual_size = 0;
  }
  return res;
}

ShenandoahHeap* ShenandoahHeap::heap() {
  CollectedHeap* heap = Universe::heap();
  assert(heap != NULL, "Unitialized access to ShenandoahHeap::heap()");
  assert(heap->kind() == CollectedHeap::ShenandoahHeap, "not a shenandoah heap");
  return (ShenandoahHeap*) heap;
}

ShenandoahHeap* ShenandoahHeap::heap_no_check() {
  CollectedHeap* heap = Universe::heap();
  return (ShenandoahHeap*) heap;
}

HeapWord* ShenandoahHeap::allocate_memory(ShenandoahAllocationRequest& req) {
  ShenandoahAllocTrace trace_alloc(req.size(), req.type());

  intptr_t pacer_epoch = 0;
  bool in_new_region = false;
  HeapWord* result = NULL;

  if (req.is_mutator_alloc()) {
    if (ShenandoahPacing) {
      pacer()->pace_for_alloc(req.size());
      pacer_epoch = pacer()->epoch();
    }

    if (!ShenandoahAllocFailureALot || !should_inject_alloc_failure()) {
      result = allocate_memory_under_lock(req, in_new_region);
    }

    // Allocation failed, block until control thread reacted, then retry allocation.
    //
    // It might happen that one of the threads requesting allocation would unblock
    // way later after GC happened, only to fail the second allocation, because
    // other threads have already depleted the free storage. In this case, a better
    // strategy is to try again, as long as GC makes progress.
    //
    // Then, we need to make sure the allocation was retried after at least one
    // Full GC, which means we want to try more than ShenandoahFullGCThreshold times.

    size_t tries = 0;

    while (result == NULL && last_gc_made_progress()) {
      tries++;
      control_thread()->handle_alloc_failure(req.size());
      result = allocate_memory_under_lock(req, in_new_region);
    }

    while (result == NULL && tries <= ShenandoahFullGCThreshold) {
      tries++;
      control_thread()->handle_alloc_failure(req.size());
      result = allocate_memory_under_lock(req, in_new_region);
    }

  } else {
    assert(req.is_gc_alloc(), "Can only accept GC allocs here");
    result = allocate_memory_under_lock(req, in_new_region);
    // Do not call handle_alloc_failure() here, because we cannot block.
    // The allocation failure would be handled by the WB slowpath with handle_alloc_failure_evac().
  }

  if (in_new_region) {
    control_thread()->notify_heap_changed();
  }

  if (result != NULL) {
    size_t requested = req.size();
    size_t actual = req.actual_size();

    assert (req.is_lab_alloc() || (requested == actual),
            err_msg("Only LAB allocations are elastic: %s, requested = " SIZE_FORMAT ", actual = " SIZE_FORMAT,
                    alloc_type_to_string(req.type()), requested, actual));

    if (req.is_mutator_alloc()) {
      notify_mutator_alloc_words(actual, false);

      // If we requested more than we were granted, give the rest back to pacer.
      // This only matters if we are in the same pacing epoch: do not try to unpace
      // over the budget for the other phase.
      if (ShenandoahPacing && (pacer_epoch > 0) && (requested > actual)) {
        pacer()->unpace_for_alloc(pacer_epoch, requested - actual);
      }
    } else {
      increase_used(actual*HeapWordSize);
    }
  }

  return result;
}

HeapWord* ShenandoahHeap::allocate_memory_under_lock(ShenandoahAllocationRequest& req, bool& in_new_region) {
  ShenandoahHeapLocker locker(lock());
  return _free_set->allocate(req, in_new_region);
}

HeapWord*  ShenandoahHeap::mem_allocate(size_t size,
                                        bool*  gc_overhead_limit_was_exceeded) {
  ShenandoahAllocationRequest req = ShenandoahAllocationRequest::for_shared(size + BrooksPointer::word_size());
  HeapWord* filler = allocate_memory(req);
  HeapWord* result = filler + BrooksPointer::word_size();
  if (filler != NULL) {
    BrooksPointer::initialize(oop(result));

    assert(! in_collection_set(result), "never allocate in targetted region");
    return result;
  } else {
    return NULL;
  }
}

class ShenandoahEvacuateUpdateRootsClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap* _heap;
  Thread* _thread;
public:
  ShenandoahEvacuateUpdateRootsClosure() :
          _heap(ShenandoahHeap::heap()), _thread(Thread::current()) {
  }

private:
  template <class T>
  void do_oop_work(T* p) {
    assert(_heap->is_evacuation_in_progress(), "Only do this when evacuation is in progress");

    T o = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(o)) {
      oop obj = oopDesc::decode_heap_oop_not_null(o);
      if (_heap->in_collection_set(obj)) {
        shenandoah_assert_marked_complete(p, obj);
        oop resolved = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
        if (oopDesc::unsafe_equals(resolved, obj)) {
          bool evac;
          resolved = _heap->evacuate_object(obj, _thread, evac);
        }
        oopDesc::encode_store_heap_oop(p, resolved);
      }
    }
  }

public:
  void do_oop(oop* p) {
    do_oop_work(p);
  }
  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }
};

class ShenandoahEvacuateRootsClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap* _heap;
  Thread* _thread;
public:
  ShenandoahEvacuateRootsClosure() :
          _heap(ShenandoahHeap::heap()), _thread(Thread::current()) {
  }

private:
  template <class T>
  void do_oop_work(T* p) {
    T o = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(o)) {
      oop obj = oopDesc::decode_heap_oop_not_null(o);
      if (_heap->in_collection_set(obj)) {
        oop resolved = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
        if (oopDesc::unsafe_equals(resolved, obj)) {
          bool evac;
          _heap->evacuate_object(obj, _thread, evac);
        }
      }
    }
  }

public:
  void do_oop(oop* p) {
    do_oop_work(p);
  }
  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }
};

class ShenandoahParallelEvacuateRegionObjectClosure : public ObjectClosure {
private:
  ShenandoahHeap* const _heap;
  Thread* const _thread;
public:
  ShenandoahParallelEvacuateRegionObjectClosure(ShenandoahHeap* heap) :
    _heap(heap), _thread(Thread::current()) {}

  void do_object(oop p) {
    shenandoah_assert_marked_complete(NULL, p);
    if (oopDesc::unsafe_equals(p, ShenandoahBarrierSet::resolve_forwarded_not_null(p))) {
      bool evac;
      _heap->evacuate_object(p, _thread, evac);
    }
  }
};

class ShenandoahParallelEvacuationTask : public AbstractGangTask {
private:
  ShenandoahHeap* const _sh;
  ShenandoahCollectionSet* const _cs;

public:
  ShenandoahParallelEvacuationTask(ShenandoahHeap* sh,
                         ShenandoahCollectionSet* cs) :
    AbstractGangTask("Parallel Evacuation Task"),
    _sh(sh),
    _cs(cs) {}

  void work(uint worker_id) {
    ShenandoahWorkerSession worker_session(worker_id);
    ShenandoahEvacOOMScope oom_evac_scope;

    ShenandoahParallelEvacuateRegionObjectClosure cl(_sh);
    ShenandoahHeapRegion* r;
    while ((r =_cs->claim_next()) != NULL) {
      assert(r->has_live(), "all-garbage regions are reclaimed early");
      _sh->marked_object_iterate(r, &cl);

      if (ShenandoahPacing) {
        _sh->pacer()->report_evac(r->used() >> LogHeapWordSize);
      }

      if (_sh->cancelled_gc()) {
        break;
      }
    }
  }
};

void ShenandoahHeap::trash_cset_regions() {
  ShenandoahHeapLocker locker(lock());

  ShenandoahCollectionSet* set = collection_set();
  ShenandoahHeapRegion* r;
  set->clear_current_index();
  while ((r = set->next()) != NULL) {
    r->make_trash();
  }
  collection_set()->clear();
}

void ShenandoahHeap::print_heap_regions_on(outputStream* st) const {
  st->print_cr("Heap Regions:");
  st->print_cr("EU=empty-uncommitted, EC=empty-committed, R=regular, H=humongous start, HC=humongous continuation, CS=collection set, T=trash, P=pinned");
  st->print_cr("BTE=bottom/top/end, U=used, T=TLAB allocs, G=GCLAB allocs, S=shared allocs, L=live data");
  st->print_cr("R=root, CP=critical pins, TAMS=top-at-mark-start (previous, next)");

  for (size_t i = 0; i < num_regions(); i++) {
    get_region(i)->print_on(st);
  }
}

void ShenandoahHeap::trash_humongous_region_at(ShenandoahHeapRegion* start) {
  assert(start->is_humongous_start(), "reclaim regions starting with the first one");

  oop humongous_obj = oop(start->bottom() + BrooksPointer::word_size());
  size_t size = humongous_obj->size() + BrooksPointer::word_size();
  size_t required_regions = ShenandoahHeapRegion::required_regions(size * HeapWordSize);
  size_t index = start->region_number() + required_regions - 1;

  assert(!start->has_live(), "liveness must be zero");

  for(size_t i = 0; i < required_regions; i++) {
     // Reclaim from tail. Otherwise, assertion fails when printing region to trace log,
     // as it expects that every region belongs to a humongous region starting with a humongous start region.
     ShenandoahHeapRegion* region = get_region(index --);

    assert(region->is_humongous(), "expect correct humongous start or continuation");
    assert(!in_collection_set(region), "Humongous region should not be in collection set");

    region->make_trash();
  }
}

#ifdef ASSERT
class ShenandoahCheckCollectionSetClosure: public ShenandoahHeapRegionClosure {
  bool heap_region_do(ShenandoahHeapRegion* r) {
    assert(! ShenandoahHeap::heap()->in_collection_set(r), "Should have been cleared by now");
    return false;
  }
};
#endif

void ShenandoahHeap::prepare_for_concurrent_evacuation() {
  if (!cancelled_gc()) {
    make_parsable(true);

    if (ShenandoahVerify) {
      verifier()->verify_after_concmark();
    }

    trash_cset_regions();

    // NOTE: This needs to be done during a stop the world pause, because
    // putting regions into the collection set concurrently with Java threads
    // will create a race. In particular, acmp could fail because when we
    // resolve the first operand, the containing region might not yet be in
    // the collection set, and thus return the original oop. When the 2nd
    // operand gets resolved, the region could be in the collection set
    // and the oop gets evacuated. If both operands have originally been
    // the same, we get false negatives.

    {
      ShenandoahHeapLocker locker(lock());
      _collection_set->clear();
      _free_set->clear();

#ifdef ASSERT
      ShenandoahCheckCollectionSetClosure ccsc;
      heap_region_iterate(&ccsc);
#endif

      heuristics()->choose_collection_set(_collection_set);
      _free_set->rebuild();
    }

    if (ShenandoahVerify) {
      verifier()->verify_before_evacuation();
    }
  }
}


class ShenandoahRetireGCLABClosure : public ThreadClosure {
private:
  bool _retire;
public:
  ShenandoahRetireGCLABClosure(bool retire) : _retire(retire) {};

  void do_thread(Thread* thread) {
    assert(thread->gclab().is_initialized(), err_msg("GCLAB should be initialized for %s", thread->name()));
    thread->gclab().make_parsable(_retire);
  }
};

void ShenandoahHeap::make_parsable(bool retire_tlabs) {
  if (UseTLAB) {
    CollectedHeap::ensure_parsability(retire_tlabs);
    ShenandoahRetireGCLABClosure cl(retire_tlabs);
    Threads::java_threads_do(&cl);
    _workers->threads_do(&cl);
  }
}

class ShenandoahEvacuateUpdateRootsTask : public AbstractGangTask {
  ShenandoahRootEvacuator* _rp;
public:

  ShenandoahEvacuateUpdateRootsTask(ShenandoahRootEvacuator* rp) :
    AbstractGangTask("Shenandoah evacuate and update roots"),
    _rp(rp)
  {
    // Nothing else to do.
  }

  void work(uint worker_id) {
    ShenandoahWorkerSession worker_session(worker_id);
    ShenandoahEvacOOMScope oom_evac_scope;
    ShenandoahEvacuateUpdateRootsClosure cl;

    MarkingCodeBlobClosure blobsCl(&cl, CodeBlobToOopClosure::FixRelocations);
    _rp->process_evacuate_roots(&cl, &blobsCl, worker_id);
  }
};

class ShenandoahFixRootsTask : public AbstractGangTask {
  ShenandoahRootEvacuator* _rp;
public:

  ShenandoahFixRootsTask(ShenandoahRootEvacuator* rp) :
    AbstractGangTask("Shenandoah update roots"),
    _rp(rp)
  {
    // Nothing else to do.
  }

  void work(uint worker_id) {
    ShenandoahWorkerSession worker_session(worker_id);
    ShenandoahEvacOOMScope oom_evac_scope;
    ShenandoahUpdateRefsClosure cl;
    MarkingCodeBlobClosure blobsCl(&cl, CodeBlobToOopClosure::FixRelocations);

    _rp->process_evacuate_roots(&cl, &blobsCl, worker_id);
  }
};
void ShenandoahHeap::evacuate_and_update_roots() {

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only iterate roots while world is stopped");

  {
    ShenandoahRootEvacuator rp(this, workers()->active_workers(), ShenandoahPhaseTimings::init_evac);
    ShenandoahEvacuateUpdateRootsTask roots_task(&rp);
    workers()->run_task(&roots_task);
  }

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

  if (cancelled_gc()) {
    // If initial evacuation has been cancelled, we need to update all references
    // after all workers have finished. Otherwise we might run into the following problem:
    // GC thread 1 cannot allocate anymore, thus evacuation fails, leaves from-space ptr of object X.
    // GC thread 2 evacuates the same object X to to-space
    // which leaves a truly dangling from-space reference in the first root oop*. This must not happen.
    // clear() and update_pointers() must always be called in pairs,
    // cannot nest with above clear()/update_pointers().
    COMPILER2_PRESENT(DerivedPointerTable::clear());
    ShenandoahRootEvacuator rp(this, workers()->active_workers(), ShenandoahPhaseTimings::init_evac);
    ShenandoahFixRootsTask update_roots_task(&rp);
    workers()->run_task(&update_roots_task);
    COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
  }
}


void ShenandoahHeap::roots_iterate(OopClosure* cl) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only iterate roots while world is stopped");

  CodeBlobToOopClosure blobsCl(cl, false);
  CLDToOopClosure cldCl(cl);

  ShenandoahRootProcessor rp(this, 1, ShenandoahPhaseTimings::_num_phases);
  rp.process_all_roots(cl, NULL, &cldCl, &blobsCl, NULL, 0);
}

bool ShenandoahHeap::supports_tlab_allocation() const {
  return true;
}

size_t  ShenandoahHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  // Returns size in bytes
  return MIN2(_free_set->unsafe_peek_free(), ShenandoahHeapRegion::max_tlab_size_bytes());
}

size_t ShenandoahHeap::max_tlab_size() const {
  // Returns size in words
  return ShenandoahHeapRegion::max_tlab_size_words();
}

class ShenandoahResizeGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    assert(thread->gclab().is_initialized(), err_msg("GCLAB should be initialized for %s", thread->name()));
    thread->gclab().resize();
  }
};

void ShenandoahHeap::resize_all_tlabs() {
  CollectedHeap::resize_all_tlabs();

  ShenandoahResizeGCLABClosure cl;
  Threads::java_threads_do(&cl);
  _workers->threads_do(&cl);
}

class ShenandoahAccumulateStatisticsGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    assert(thread->gclab().is_initialized(), err_msg("GCLAB should be initialized for %s", thread->name()));
    thread->gclab().accumulate_statistics();
    thread->gclab().initialize_statistics();
  }
};

void ShenandoahHeap::accumulate_statistics_all_gclabs() {
  ShenandoahAccumulateStatisticsGCLABClosure cl;
  Threads::java_threads_do(&cl);
  _workers->threads_do(&cl);
}

bool  ShenandoahHeap::can_elide_tlab_store_barriers() const {
  return true;
}

oop ShenandoahHeap::new_store_pre_barrier(JavaThread* thread, oop new_obj) {
  // Overridden to do nothing.
  return new_obj;
}

bool  ShenandoahHeap::can_elide_initializing_store_barrier(oop new_obj) {
  return true;
}

bool ShenandoahHeap::card_mark_must_follow_store() const {
  return false;
}

bool ShenandoahHeap::supports_heap_inspection() const {
  return false;
}

void ShenandoahHeap::collect(GCCause::Cause cause) {
  _control_thread->handle_explicit_gc(cause);
}

void ShenandoahHeap::do_full_collection(bool clear_all_soft_refs) {
  //assert(false, "Shouldn't need to do full collections");
}

AdaptiveSizePolicy* ShenandoahHeap::size_policy() {
  Unimplemented();
  return NULL;

}

CollectorPolicy* ShenandoahHeap::collector_policy() const {
  return _shenandoah_policy;
}

void ShenandoahHeap::resize_tlabs() {
  CollectedHeap::resize_all_tlabs();
}

void ShenandoahHeap::accumulate_statistics_tlabs() {
  CollectedHeap::accumulate_statistics_all_tlabs();
}

HeapWord* ShenandoahHeap::block_start(const void* addr) const {
  Space* sp = heap_region_containing(addr);
  if (sp != NULL) {
    return sp->block_start(addr);
  }
  return NULL;
}

size_t ShenandoahHeap::block_size(const HeapWord* addr) const {
  Space* sp = heap_region_containing(addr);
  assert(sp != NULL, "block_size of address outside of heap");
  return sp->block_size(addr);
}

bool ShenandoahHeap::block_is_obj(const HeapWord* addr) const {
  Space* sp = heap_region_containing(addr);
  return sp->block_is_obj(addr);
}

jlong ShenandoahHeap::millis_since_last_gc() {
  return 0;
}

void ShenandoahHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint()) {
    make_parsable(false);
  }
}

void ShenandoahHeap::print_gc_threads_on(outputStream* st) const {
  workers()->print_worker_threads_on(st);
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::print_worker_threads_on(st);
  }
}

void ShenandoahHeap::gc_threads_do(ThreadClosure* tcl) const {
  workers()->threads_do(tcl);
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::threads_do(tcl);
  }
}

void ShenandoahHeap::print_tracing_info() const {
  if (PrintGC || TraceGen0Time || TraceGen1Time) {
    ResourceMark rm;
    outputStream* out = gclog_or_tty;
    phase_timings()->print_on(out);

    out->cr();
    out->cr();

    shenandoahPolicy()->print_gc_stats(out);

    out->cr();
    out->cr();

    if (ShenandoahPacing) {
      pacer()->print_on(out);
    }

    out->cr();
    out->cr();

    if (ShenandoahAllocationTrace) {
      assert(alloc_tracker() != NULL, "Must be");
      alloc_tracker()->print_on(out);
    } else {
      out->print_cr("  Allocation tracing is disabled, use -XX:+ShenandoahAllocationTrace to enable.");
    }
  }
}

void ShenandoahHeap::verify(bool silent, VerifyOption vo) {
  if (ShenandoahSafepoint::is_at_shenandoah_safepoint() || ! UseTLAB) {
    if (ShenandoahVerify) {
      verifier()->verify_generic(vo);
    } else {
      // TODO: Consider allocating verification bitmaps on demand,
      // and turn this on unconditionally.
    }
  }
}
size_t ShenandoahHeap::tlab_capacity(Thread *thr) const {
  return _free_set->capacity();
}

class ObjectIterateScanRootClosure : public ExtendedOopClosure {
private:
  MarkBitMap* _bitmap;
  Stack<oop,mtGC>* _oop_stack;

  template <class T>
  void do_oop_work(T* p) {
    T o = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(o)) {
      oop obj = oopDesc::decode_heap_oop_not_null(o);
      obj = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      assert(obj->is_oop(), "must be a valid oop");
      if (!_bitmap->isMarked((HeapWord*) obj)) {
        _bitmap->mark((HeapWord*) obj);
        _oop_stack->push(obj);
      }
    }
  }
public:
  ObjectIterateScanRootClosure(MarkBitMap* bitmap, Stack<oop,mtGC>* oop_stack) :
    _bitmap(bitmap), _oop_stack(oop_stack) {}
  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

/*
 * This is public API, used in preparation of object_iterate().
 * Since we don't do linear scan of heap in object_iterate() (see comment below), we don't
 * need to make the heap parsable. For Shenandoah-internal linear heap scans that we can
 * control, we call SH::make_parsable().
 */
void ShenandoahHeap::ensure_parsability(bool retire_tlabs) {
  // No-op.
}

/*
 * Iterates objects in the heap. This is public API, used for, e.g., heap dumping.
 *
 * We cannot safely iterate objects by doing a linear scan at random points in time. Linear
 * scanning needs to deal with dead objects, which may have dead Klass* pointers (e.g.
 * calling oopDesc::size() would crash) or dangling reference fields (crashes) etc. Linear
 * scanning therefore depends on having a valid marking bitmap to support it. However, we only
 * have a valid marking bitmap after successful marking. In particular, we *don't* have a valid
 * marking bitmap during marking, after aborted marking or during/after cleanup (when we just
 * wiped the bitmap in preparation for next marking).
 *
 * For all those reasons, we implement object iteration as a single marking traversal, reporting
 * objects as we mark+traverse through the heap, starting from GC roots. JVMTI IterateThroughHeap
 * is allowed to report dead objects, but is not required to do so.
 */
void ShenandoahHeap::object_iterate(ObjectClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "safe iteration is only available during safepoints");
  if (!os::commit_memory((char*)_aux_bitmap_region.start(), _aux_bitmap_region.byte_size(), false)) {
    log_warning(gc)("Could not commit native memory for auxiliary marking bitmap for heap iteration");
    return;
  }

  Stack<oop,mtGC> oop_stack;

  // First, we process all GC roots. This populates the work stack with initial objects.
  ShenandoahRootProcessor rp(this, 1, ShenandoahPhaseTimings::_num_phases);
  ObjectIterateScanRootClosure oops(&_aux_bit_map, &oop_stack);
  CLDToOopClosure clds(&oops, false);
  CodeBlobToOopClosure blobs(&oops, false);
  rp.process_all_roots(&oops, &oops, &clds, &blobs, NULL, 0);

  // Work through the oop stack to traverse heap.
  while (! oop_stack.is_empty()) {
    oop obj = oop_stack.pop();
    assert(obj->is_oop(), "must be a valid oop");
    cl->do_object(obj);
    obj->oop_iterate(&oops);
  }

  assert(oop_stack.is_empty(), "should be empty");

  if (!os::uncommit_memory((char*)_aux_bitmap_region.start(), _aux_bitmap_region.byte_size())) {
    log_warning(gc)("Could not uncommit native memory for auxiliary marking bitmap for heap iteration");
  }
}

void ShenandoahHeap::safe_object_iterate(ObjectClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "safe iteration is only available during safepoints");
  object_iterate(cl);
}

void ShenandoahHeap::oop_iterate(ExtendedOopClosure* cl) {
  ObjectToOopClosure cl2(cl);
  object_iterate(&cl2);
}

class ShenandoahSpaceClosureRegionClosure: public ShenandoahHeapRegionClosure {
  SpaceClosure* _cl;
public:
  ShenandoahSpaceClosureRegionClosure(SpaceClosure* cl) : _cl(cl) {}
  bool heap_region_do(ShenandoahHeapRegion* r) {
    _cl->do_space(r);
    return false;
  }
};

void  ShenandoahHeap::space_iterate(SpaceClosure* cl) {
  ShenandoahSpaceClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

Space*  ShenandoahHeap::space_containing(const void* oop) const {
  Space* res = heap_region_containing(oop);
  return res;
}

void  ShenandoahHeap::gc_prologue(bool b) {
  Unimplemented();
}

void  ShenandoahHeap::gc_epilogue(bool b) {
  Unimplemented();
}

// Apply blk->heap_region_do() on all committed regions in address order,
// terminating the iteration early if heap_region_do() returns true.
void ShenandoahHeap::heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_cset_regions, bool skip_humongous_continuation) const {
  for (size_t i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* current  = get_region(i);
    if (skip_humongous_continuation && current->is_humongous_continuation()) {
      continue;
    }
    if (skip_cset_regions && in_collection_set(current)) {
      continue;
    }
    if (blk->heap_region_do(current)) {
      return;
    }
  }
}

class ShenandoahClearLivenessClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeap* sh;
public:
  ShenandoahClearLivenessClosure(ShenandoahHeap* heap) : sh(heap) {}

  bool heap_region_do(ShenandoahHeapRegion* r) {
    r->clear_live_data();
    sh->next_marking_context()->set_top_at_mark_start(r->region_number(), r->top());
    return false;
  }
};


void ShenandoahHeap::op_init_mark() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");

  assert(next_marking_context()->is_bitmap_clear(), "need clear marking bitmap");

  if (ShenandoahVerify) {
    verifier()->verify_before_concmark();
  }

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::accumulate_stats);
    accumulate_statistics_tlabs();
  }

  set_concurrent_mark_in_progress(true);
  // We need to reset all TLABs because we'd lose marks on all objects allocated in them.
  if (UseTLAB) {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::make_parsable);
    make_parsable(true);
  }

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::clear_liveness);
    ShenandoahClearLivenessClosure clc(this);
    heap_region_iterate(&clc);
  }

  // Make above changes visible to worker threads
  OrderAccess::fence();

  concurrentMark()->init_mark_roots();

  if (UseTLAB) {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::resize_tlabs);
    resize_tlabs();
  }

  if (ShenandoahPacing) {
    pacer()->setup_for_mark();
  }
}

void ShenandoahHeap::op_mark() {
  concurrentMark()->mark_from_roots();
}

void ShenandoahHeap::op_final_mark() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");

  // It is critical that we
  // evacuate roots right after finishing marking, so that we don't
  // get unmarked objects in the roots.

  if (!cancelled_gc()) {
    concurrentMark()->finish_mark_from_roots();
    stop_concurrent_marking();

    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::complete_liveness);

      // All allocations past TAMS are implicitly live, adjust the region data.
      // Bitmaps/TAMS are swapped at this point, so we need to poll complete bitmap.
      for (size_t i = 0; i < num_regions(); i++) {
        ShenandoahHeapRegion* r = get_region(i);
        if (!r->is_active()) continue;

        HeapWord* tams = complete_marking_context()->top_at_mark_start(r->region_number());
        HeapWord* top = r->top();
        if (top > tams) {
          r->increase_live_data_alloc_words(pointer_delta(top, tams));
        }
      }
    }

    {
      ShenandoahGCPhase prepare_evac(ShenandoahPhaseTimings::prepare_evac);
      prepare_for_concurrent_evacuation();
    }

    // If collection set has candidates, start evacuation.
    // Otherwise, bypass the rest of the cycle.
    if (!collection_set()->is_empty()) {
      set_evacuation_in_progress(true);
      // From here on, we need to update references.
      set_has_forwarded_objects(true);

      ShenandoahGCPhase init_evac(ShenandoahPhaseTimings::init_evac);
      evacuate_and_update_roots();
    }

    if (ShenandoahPacing) {
      pacer()->setup_for_evac();
    }
  } else {
    concurrentMark()->cancel();
    stop_concurrent_marking();

    if (process_references()) {
      // Abandon reference processing right away: pre-cleaning must have failed.
      ReferenceProcessor *rp = ref_processor();
      rp->disable_discovery();
      rp->abandon_partial_discovery();
      rp->verify_no_references_recorded();
    }
  }
}

void ShenandoahHeap::op_final_evac() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");

  set_evacuation_in_progress(false);
  if (ShenandoahVerify) {
    verifier()->verify_after_evacuation();
  }
}

void ShenandoahHeap::op_evac() {
  ShenandoahParallelEvacuationTask task(this, _collection_set);
  workers()->run_task(&task);
}

void ShenandoahHeap::op_updaterefs() {
  update_heap_references(true);
}

void ShenandoahHeap::op_cleanup() {
  ShenandoahGCPhase phase_recycle(ShenandoahPhaseTimings::conc_cleanup_recycle);
  free_set()->recycle_trash();
}

void ShenandoahHeap::op_cleanup_bitmaps() {
  op_cleanup();

  ShenandoahGCPhase phase_reset(ShenandoahPhaseTimings::conc_cleanup_reset_bitmaps);
  reset_next_mark_bitmap();
}

void ShenandoahHeap::op_preclean() {
  concurrentMark()->preclean_weak_refs();
}

void ShenandoahHeap::op_full(GCCause::Cause cause) {
  ShenandoahMetricsSnapshot metrics;
  metrics.snap_before();

  full_gc()->do_it(cause);

  metrics.snap_after();
  metrics.print();

  if (metrics.is_good_progress("Full GC")) {
    _progress_last_gc.set();
  } else {
    // Nothing to do. Tell the allocation path that we have failed to make
    // progress, and it can finally fail.
    _progress_last_gc.unset();
  }
}

void ShenandoahHeap::op_degenerated(ShenandoahDegenPoint point) {
  // Degenerated GC is STW, but it can also fail. Current mechanics communicates
  // GC failure via cancelled_concgc() flag. So, if we detect the failure after
  // some phase, we have to upgrade the Degenerate GC to Full GC.

  clear_cancelled_gc();

  ShenandoahMetricsSnapshot metrics;
  metrics.snap_before();

  switch (point) {
    // The cases below form the Duff's-like device: it describes the actual GC cycle,
    // but enters it at different points, depending on which concurrent phase had
    // degenerated.

    case _degenerated_outside_cycle:
      // We have degenerated from outside the cycle, which means something is bad with
      // the heap, most probably heavy humongous fragmentation, or we are very low on free
      // space. It makes little sense to wait for Full GC to reclaim as much as it can, when
      // we can do the most aggressive degen cycle, which includes processing references and
      // class unloading, unless those features are explicitly disabled.
      //
      // Note that we can only do this for "outside-cycle" degens, otherwise we would risk
      // changing the cycle parameters mid-cycle during concurrent -> degenerated handover.
      set_process_references(ShenandoahRefProcFrequency != 0);
      set_unload_classes(ClassUnloading);

      op_init_mark();
      if (cancelled_gc()) {
        op_degenerated_fail();
        return;
      }

    case _degenerated_mark:
      op_final_mark();
      if (cancelled_gc()) {
        op_degenerated_fail();
        return;
      }

      op_cleanup();

    case _degenerated_evac:
      // If heuristics thinks we should do the cycle, this flag would be set,
      // and we can do evacuation. Otherwise, it would be the shortcut cycle.
      if (is_evacuation_in_progress()) {

        // Degeneration under oom-evac protocol might have left some objects in
        // collection set un-evacuated. Restart evacuation from the beginning to
        // capture all objects. For all the objects that are already evacuated,
        // it would be a simple check, which is supposed to be fast. This is also
        // safe to do even without degeneration, as CSet iterator is at beginning
        // in preparation for evacuation anyway.
        collection_set()->clear_current_index();

        op_evac();
        if (cancelled_gc()) {
          op_degenerated_fail();
          return;
        }
      }

      // If heuristics thinks we should do the cycle, this flag would be set,
      // and we need to do update-refs. Otherwise, it would be the shortcut cycle.
      if (has_forwarded_objects()) {
        op_init_updaterefs();
        if (cancelled_gc()) {
          op_degenerated_fail();
          return;
        }
      }

    case _degenerated_updaterefs:
      if (has_forwarded_objects()) {
        op_final_updaterefs();
        if (cancelled_gc()) {
          op_degenerated_fail();
          return;
        }
      }

      op_cleanup_bitmaps();
      break;

    default:
      ShouldNotReachHere();
  }

  if (ShenandoahVerify) {
    verifier()->verify_after_degenerated();
  }

  metrics.snap_after();
  metrics.print();

  // Check for futility and fail. There is no reason to do several back-to-back Degenerated cycles,
  // because that probably means the heap is overloaded and/or fragmented.
  if (!metrics.is_good_progress("Degenerated GC")) {
    _progress_last_gc.unset();
    cancel_gc(GCCause::_shenandoah_upgrade_to_full_gc);
    op_degenerated_futile();
  } else {
    _progress_last_gc.set();
  }
}

void ShenandoahHeap::op_degenerated_fail() {
  log_info(gc)("Cannot finish degeneration, upgrading to Full GC");
  shenandoahPolicy()->record_degenerated_upgrade_to_full();
  op_full(GCCause::_shenandoah_upgrade_to_full_gc);
}

void ShenandoahHeap::op_degenerated_futile() {
  shenandoahPolicy()->record_degenerated_upgrade_to_full();
  op_full(GCCause::_shenandoah_upgrade_to_full_gc);
}

void ShenandoahHeap::swap_mark_contexts() {
  ShenandoahMarkingContext* tmp = _complete_marking_context;
  _complete_marking_context = _next_marking_context;
  _next_marking_context = tmp;
}

void ShenandoahHeap::stop_concurrent_marking() {
  assert(is_concurrent_mark_in_progress(), "How else could we get here?");
  if (!cancelled_gc()) {
    // If we needed to update refs, and concurrent marking has been cancelled,
    // we need to finish updating references.
    set_has_forwarded_objects(false);
    swap_mark_contexts();
  }
  set_concurrent_mark_in_progress(false);
}

void ShenandoahHeap::force_satb_flush_all_threads() {
  if (!is_concurrent_mark_in_progress()) {
    // No need to flush SATBs
    return;
  }

  MutexLocker ml(Threads_lock);
  JavaThread::set_force_satb_flush_all_threads(true);

  // The threads are not "acquiring" their thread-local data, but it does not
  // hurt to "release" the updates here anyway.
  OrderAccess::fence();
}


void ShenandoahHeap::set_gc_state_mask(uint mask, bool value) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should really be Shenandoah safepoint");
  _gc_state.set_cond(mask, value);
  JavaThread::set_gc_state_all_threads(_gc_state.raw_value());
}

void ShenandoahHeap::set_concurrent_mark_in_progress(bool in_progress) {
  set_gc_state_mask(MARKING, in_progress);
  JavaThread::satb_mark_queue_set().set_active_all_threads(in_progress, !in_progress);
}

void ShenandoahHeap::set_evacuation_in_progress(bool in_progress) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only call this at safepoint");
  set_gc_state_mask(EVACUATION, in_progress);
}

HeapWord* ShenandoahHeap::tlab_post_allocation_setup(HeapWord* obj) {
  // Initialize Brooks pointer for the next object
  HeapWord* result = obj + BrooksPointer::word_size();
  BrooksPointer::initialize(oop(result));
  return result;
}

uint ShenandoahHeap::oop_extra_words() {
  return BrooksPointer::word_size();
}

ShenandoahForwardedIsAliveClosure::ShenandoahForwardedIsAliveClosure() :
  _mark_context(ShenandoahHeap::heap()->next_marking_context()) {
}

ShenandoahIsAliveClosure::ShenandoahIsAliveClosure() :
  _mark_context(ShenandoahHeap::heap()->next_marking_context()) {
}

bool ShenandoahForwardedIsAliveClosure::do_object_b(oop obj) {
  if (oopDesc::is_null(obj)) {
    return false;
  }
  obj = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
  shenandoah_assert_not_forwarded_if(NULL, obj, ShenandoahHeap::heap()->is_concurrent_mark_in_progress());
  return _mark_context->is_marked(obj);
}

bool ShenandoahIsAliveClosure::do_object_b(oop obj) {
  if (oopDesc::is_null(obj)) {
    return false;
  }
  shenandoah_assert_not_forwarded(NULL, obj);
  return _mark_context->is_marked(obj);
}

void ShenandoahHeap::ref_processing_init() {
  MemRegion mr = reserved_region();

  assert(_max_workers > 0, "Sanity");

  _ref_processor =
    new ReferenceProcessor(mr,    // span
                           ParallelRefProcEnabled,  // MT processing
                           _max_workers,            // Degree of MT processing
                           true,                    // MT discovery
                           _max_workers,            // Degree of MT discovery
                           false,                   // Reference discovery is not atomic
                           NULL);                   // No closure, should be installed before use

  shenandoah_assert_rp_isalive_not_installed();
}

void ShenandoahHeap::acquire_pending_refs_lock() {
  _control_thread->slt()->manipulatePLL(SurrogateLockerThread::acquirePLL);
}

void ShenandoahHeap::release_pending_refs_lock() {
  _control_thread->slt()->manipulatePLL(SurrogateLockerThread::releaseAndNotifyPLL);
}

GCTracer* ShenandoahHeap::tracer() {
  return shenandoahPolicy()->tracer();
}

size_t ShenandoahHeap::tlab_used(Thread* thread) const {
  return _free_set->used();
}

void ShenandoahHeap::cancel_gc(GCCause::Cause cause) {
  if (try_cancel_gc()) {
    FormatBuffer<> msg("Cancelling GC: %s", GCCause::to_string(cause));
    log_info(gc)("%s", msg.buffer());
    Events::log(Thread::current(), "%s", msg.buffer());
  }
}

uint ShenandoahHeap::max_workers() {
  return _max_workers;
}

void ShenandoahHeap::stop() {
  // The shutdown sequence should be able to terminate when GC is running.

  // Step 0. Notify policy to disable event recording.
  _shenandoah_policy->record_shutdown();

  // Step 1. Notify control thread that we are in shutdown.
  // Note that we cannot do that with stop(), because stop() is blocking and waits for the actual shutdown.
  // Doing stop() here would wait for the normal GC cycle to complete, never falling through to cancel below.
  _control_thread->prepare_for_graceful_shutdown();

  // Step 2. Notify GC workers that we are cancelling GC.
  cancel_gc(GCCause::_shenandoah_stop_vm);

  // Step 3. Wait until GC worker exits normally.
  _control_thread->stop();

  // Step 4. Stop String Dedup thread if it is active
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::stop();
  }
}

void ShenandoahHeap::unload_classes_and_cleanup_tables(bool full_gc) {
  assert(ClassUnloading || full_gc, "Class unloading should be enabled");

  ShenandoahPhaseTimings::Phase phase_root =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_purge :
          ShenandoahPhaseTimings::purge;

  ShenandoahPhaseTimings::Phase phase_unload =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_purge_class_unload :
          ShenandoahPhaseTimings::purge_class_unload;

  ShenandoahPhaseTimings::Phase phase_cldg =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_purge_cldg :
          ShenandoahPhaseTimings::purge_cldg;

  ShenandoahPhaseTimings::Phase phase_par =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_purge_par :
          ShenandoahPhaseTimings::purge_par;

  ShenandoahPhaseTimings::Phase phase_par_classes =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_purge_par_classes :
          ShenandoahPhaseTimings::purge_par_classes;

  ShenandoahPhaseTimings::Phase phase_par_codecache =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_purge_par_codecache :
          ShenandoahPhaseTimings::purge_par_codecache;

  ShenandoahPhaseTimings::Phase phase_par_symbstring =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_purge_par_symbstring :
          ShenandoahPhaseTimings::purge_par_symbstring;

  ShenandoahPhaseTimings::Phase phase_par_sync =
          full_gc ?
          ShenandoahPhaseTimings::full_gc_purge_par_sync :
          ShenandoahPhaseTimings::purge_par_sync;

  ShenandoahGCPhase root_phase(phase_root);

  ShenandoahIsAliveSelector alive;
  BoolObjectClosure* is_alive = alive.is_alive_closure();

  bool purged_class;

  // Unload classes and purge SystemDictionary.
  {
    ShenandoahGCPhase phase(phase_unload);
    purged_class = SystemDictionary::do_unloading(is_alive,
                                                  full_gc /* do_cleaning*/ );
  }

  {
    ShenandoahGCPhase phase(phase_par);
    uint active = _workers->active_workers();
    ParallelCleaningTask unlink_task(is_alive, true, true, active, purged_class);
    _workers->run_task(&unlink_task);

    ShenandoahPhaseTimings* p = phase_timings();
    ParallelCleaningTimes times = unlink_task.times();

    // "times" report total time, phase_tables_cc reports wall time. Divide total times
    // by active workers to get average time per worker, that would add up to wall time.
    p->record_phase_time(phase_par_classes,    times.klass_work_us() / active);
    p->record_phase_time(phase_par_codecache,  times.codecache_work_us() / active);
    p->record_phase_time(phase_par_symbstring, times.tables_work_us() / active);
    p->record_phase_time(phase_par_sync,       times.sync_us() / active);
  }

  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahPhaseTimings::Phase phase_par_string_dedup =
            full_gc ?
            ShenandoahPhaseTimings::full_gc_purge_par_string_dedup :
            ShenandoahPhaseTimings::purge_par_string_dedup;
    ShenandoahGCPhase phase(phase_par_string_dedup);
    ShenandoahStringDedup::parallel_cleanup();
  }


  {
    ShenandoahGCPhase phase(phase_cldg);
    ClassLoaderDataGraph::purge();
  }
}

void ShenandoahHeap::set_has_forwarded_objects(bool cond) {
  set_gc_state_mask(HAS_FORWARDED, cond);
}

bool ShenandoahHeap::last_gc_made_progress() const {
  return _progress_last_gc.is_set();
}

void ShenandoahHeap::set_process_references(bool pr) {
  _process_references.set_cond(pr);
}

void ShenandoahHeap::set_unload_classes(bool uc) {
  _unload_classes.set_cond(uc);
}

bool ShenandoahHeap::process_references() const {
  return _process_references.is_set();
}

bool ShenandoahHeap::unload_classes() const {
  return _unload_classes.is_set();
}

//fixme this should be in heapregionset
ShenandoahHeapRegion* ShenandoahHeap::next_compaction_region(const ShenandoahHeapRegion* r) {
  size_t region_idx = r->region_number() + 1;
  ShenandoahHeapRegion* next = get_region(region_idx);
  guarantee(next->region_number() == region_idx, "region number must match");
  while (next->is_humongous()) {
    region_idx = next->region_number() + 1;
    next = get_region(region_idx);
    guarantee(next->region_number() == region_idx, "region number must match");
  }
  return next;
}

ShenandoahMonitoringSupport* ShenandoahHeap::monitoring_support() {
  return _monitoring_support;
}

address ShenandoahHeap::in_cset_fast_test_addr() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  assert(heap->collection_set() != NULL, "Sanity");
  return (address) heap->collection_set()->biased_map_address();
}

address ShenandoahHeap::cancelled_gc_addr() {
  return (address) ShenandoahHeap::heap()->_cancelled_gc.addr_of();
}

address ShenandoahHeap::gc_state_addr() {
  return (address) ShenandoahHeap::heap()->_gc_state.addr_of();
}

size_t ShenandoahHeap::conservative_max_heap_alignment() {
  return ShenandoahMaxRegionSize;
}

size_t ShenandoahHeap::bytes_allocated_since_gc_start() {
  return OrderAccess::load_acquire(&_bytes_allocated_since_gc_start);
}

void ShenandoahHeap::reset_bytes_allocated_since_gc_start() {
  OrderAccess::release_store_fence(&_bytes_allocated_since_gc_start, (size_t)0);
}

ShenandoahPacer* ShenandoahHeap::pacer() const {
  assert (_pacer != NULL, "sanity");
  return _pacer;
}

void ShenandoahHeap::set_degenerated_gc_in_progress(bool in_progress) {
  _degenerated_gc_in_progress.set_cond(in_progress);
}

void ShenandoahHeap::set_full_gc_in_progress(bool in_progress) {
  _full_gc_in_progress.set_cond(in_progress);
}

void ShenandoahHeap::set_full_gc_move_in_progress(bool in_progress) {
  assert (is_full_gc_in_progress(), "should be");
  _full_gc_move_in_progress.set_cond(in_progress);
}

void ShenandoahHeap::set_update_refs_in_progress(bool in_progress) {
  set_gc_state_mask(UPDATEREFS, in_progress);
}

void ShenandoahHeap::register_nmethod(nmethod* nm) {
  ShenandoahCodeRoots::add_nmethod(nm);
}

void ShenandoahHeap::unregister_nmethod(nmethod* nm) {
  ShenandoahCodeRoots::remove_nmethod(nm);
}

oop ShenandoahHeap::pin_object(JavaThread* thr, oop o) {
  o = barrier_set()->write_barrier(o);
  ShenandoahHeapLocker locker(lock());
  heap_region_containing(o)->make_pinned();
  return o;
}

void ShenandoahHeap::unpin_object(JavaThread* thr, oop o) {
  o = barrier_set()->read_barrier(o);
  ShenandoahHeapLocker locker(lock());
  heap_region_containing(o)->make_unpinned();
}

GCTimer* ShenandoahHeap::gc_timer() const {
  return _gc_timer;
}

#ifdef ASSERT
void ShenandoahHeap::assert_gc_workers(uint nworkers) {
  assert(nworkers > 0 && nworkers <= max_workers(), "Sanity");

  if (ShenandoahSafepoint::is_at_shenandoah_safepoint()) {
    if (UseDynamicNumberOfGCThreads ||
        (FLAG_IS_DEFAULT(ParallelGCThreads) && ForceDynamicNumberOfGCThreads)) {
      assert(nworkers <= ParallelGCThreads, "Cannot use more than it has");
    } else {
      // Use ParallelGCThreads inside safepoints
      assert(nworkers == ParallelGCThreads, "Use ParalleGCThreads within safepoints");
    }
  } else {
    if (UseDynamicNumberOfGCThreads ||
        (FLAG_IS_DEFAULT(ConcGCThreads) && ForceDynamicNumberOfGCThreads)) {
      assert(nworkers <= ConcGCThreads, "Cannot use more than it has");
    } else {
      // Use ConcGCThreads outside safepoints
      assert(nworkers == ConcGCThreads, "Use ConcGCThreads outside safepoints");
    }
  }
}
#endif

ShenandoahUpdateHeapRefsClosure::ShenandoahUpdateHeapRefsClosure() :
  _heap(ShenandoahHeap::heap()) {}

ShenandoahVerifier* ShenandoahHeap::verifier() {
  guarantee(ShenandoahVerify, "Should be enabled");
  assert (_verifier != NULL, "sanity");
  return _verifier;
}

class ShenandoahUpdateHeapRefsTask : public AbstractGangTask {
private:
  ShenandoahHeap* _heap;
  ShenandoahRegionIterator* _regions;
  bool _concurrent;

public:
  ShenandoahUpdateHeapRefsTask(ShenandoahRegionIterator* regions, bool concurrent) :
    AbstractGangTask("Concurrent Update References Task"),
    _heap(ShenandoahHeap::heap()),
    _regions(regions),
    _concurrent(concurrent) {
  }

  void work(uint worker_id) {
    ShenandoahWorkerSession worker_session(worker_id);
    ShenandoahUpdateHeapRefsClosure cl;
    ShenandoahHeapRegion* r = _regions->next();
    ShenandoahMarkingContext* const ctx = _heap->complete_marking_context();
    while (r != NULL) {
      if (_heap->in_collection_set(r)) {
        HeapWord* bottom = r->bottom();
        HeapWord* top = ctx->top_at_mark_start(r->region_number());
        if (top > bottom) {
          ctx->clear_bitmap(bottom, top);
        }
      } else {
        if (r->is_active()) {
          _heap->marked_object_oop_safe_iterate(r, &cl);
        }
      }
      if (ShenandoahPacing) {
        HeapWord* top_at_start_ur = r->concurrent_iteration_safe_limit();
        assert (top_at_start_ur >= r->bottom(), "sanity");
        _heap->pacer()->report_updaterefs(pointer_delta(top_at_start_ur, r->bottom()));
      }
      if (_heap->cancelled_gc()) {
        return;
      }
      r = _regions->next();
    }
  }
};

void ShenandoahHeap::update_heap_references(bool concurrent) {
  ShenandoahUpdateHeapRefsTask task(&_update_refs_iterator, concurrent);
  workers()->run_task(&task);
}

void ShenandoahHeap::op_init_updaterefs() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at safepoint");

  set_evacuation_in_progress(false);

  if (ShenandoahVerify) {
    verifier()->verify_before_updaterefs();
  }

  set_update_refs_in_progress(true);
  make_parsable(true);
  for (uint i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* r = get_region(i);
    r->set_concurrent_iteration_safe_limit(r->top());
  }

  // Reset iterator.
  _update_refs_iterator.reset();

  if (ShenandoahPacing) {
    pacer()->setup_for_updaterefs();
  }
}

void ShenandoahHeap::op_final_updaterefs() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at safepoint");

  // Check if there is left-over work, and finish it
  if (_update_refs_iterator.has_next()) {
    ShenandoahGCPhase final_work(ShenandoahPhaseTimings::final_update_refs_finish_work);

    // Finish updating references where we left off.
    clear_cancelled_gc();
    update_heap_references(false);
  }

  // Clear cancelled GC, if set. On cancellation path, the block before would handle
  // everything. On degenerated paths, cancelled gc would not be set anyway.
  if (cancelled_gc()) {
    clear_cancelled_gc();
  }
  assert(!cancelled_gc(), "Should have been done right before");

  concurrentMark()->update_roots(ShenandoahPhaseTimings::final_update_refs_roots);

  ShenandoahGCPhase final_update_refs(ShenandoahPhaseTimings::final_update_refs_recycle);

  trash_cset_regions();
  set_has_forwarded_objects(false);
  set_update_refs_in_progress(false);

  if (ShenandoahVerify) {
    verifier()->verify_after_updaterefs();
  }

  {
    ShenandoahHeapLocker locker(lock());
    _free_set->rebuild();
  }
}

#ifdef ASSERT
void ShenandoahHeap::assert_heaplock_not_owned_by_current_thread() {
  _lock.assert_not_owned_by_current_thread();
}

void ShenandoahHeap::assert_heaplock_owned_by_current_thread() {
  _lock.assert_owned_by_current_thread();
}

void ShenandoahHeap::assert_heaplock_or_safepoint() {
  _lock.assert_owned_by_current_thread_or_safepoint();
}
#endif

void ShenandoahHeap::print_extended_on(outputStream *st) const {
  print_on(st);
  print_heap_regions_on(st);
}

bool ShenandoahHeap::is_bitmap_slice_committed(ShenandoahHeapRegion* r, bool skip_self) {
  size_t slice = r->region_number() / _bitmap_regions_per_slice;

  size_t regions_from = _bitmap_regions_per_slice * slice;
  size_t regions_to   = MIN2(num_regions(), _bitmap_regions_per_slice * (slice + 1));
  for (size_t g = regions_from; g < regions_to; g++) {
    assert (g / _bitmap_regions_per_slice == slice, "same slice");
    if (skip_self && g == r->region_number()) continue;
    if (get_region(g)->is_committed()) {
      return true;
    }
  }
  return false;
}

bool ShenandoahHeap::commit_bitmap_slice(ShenandoahHeapRegion* r) {
  assert_heaplock_owned_by_current_thread();

  if (is_bitmap_slice_committed(r, true)) {
    // Some other region from the group is already committed, meaning the bitmap
    // slice is already committed, we exit right away.
    return true;
  }

  // Commit the bitmap slice:
  size_t slice = r->region_number() / _bitmap_regions_per_slice;
  size_t off = _bitmap_bytes_per_slice * slice;
  size_t len = _bitmap_bytes_per_slice;
  if (!os::commit_memory((char*)_bitmap0_region.start() + off, len, false)) {
    return false;
  }
  if (!os::commit_memory((char*)_bitmap1_region.start() + off, len, false)) {
    return false;
  }
  return true;
}

bool ShenandoahHeap::uncommit_bitmap_slice(ShenandoahHeapRegion *r) {
  assert_heaplock_owned_by_current_thread();

  if (is_bitmap_slice_committed(r, true)) {
    // Some other region from the group is still committed, meaning the bitmap
    // slice is should stay committed, exit right away.
    return true;
  }

  // Uncommit the bitmap slice:
  size_t slice = r->region_number() / _bitmap_regions_per_slice;
  size_t off = _bitmap_bytes_per_slice * slice;
  size_t len = _bitmap_bytes_per_slice;
  if (!os::uncommit_memory((char*)_bitmap0_region.start() + off, len)) {
    return false;
  }
  if (!os::uncommit_memory((char*)_bitmap1_region.start() + off, len)) {
    return false;
  }
  return true;
}

void ShenandoahHeap::vmop_entry_init_mark() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitMark op;
  VMThread::execute(&op); // jump to entry_init_mark() under safepoint
}

void ShenandoahHeap::vmop_entry_final_mark() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_mark_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalMarkStartEvac op;
  VMThread::execute(&op); // jump to entry_final_mark under safepoint
}

void ShenandoahHeap::vmop_entry_final_evac() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_evac_gross);

  VM_ShenandoahFinalEvac op;
  VMThread::execute(&op); // jump to entry_final_evac under safepoint
}

void ShenandoahHeap::vmop_entry_init_updaterefs() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_refs_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitUpdateRefs op;
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_entry_final_updaterefs() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalUpdateRefs op;
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_entry_full(GCCause::Cause cause) {
  TraceCollectorStats tcs(monitoring_support()->full_stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::full_gc_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFullGC op(cause);
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_degenerated(ShenandoahDegenPoint point) {
  TraceCollectorStats tcs(monitoring_support()->full_stw_collection_counters());
  ShenandoahGCPhase total(ShenandoahPhaseTimings::total_pause_gross);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::degen_gc_gross);

  VM_ShenandoahDegeneratedGC degenerated_gc((int)point);
  VMThread::execute(&degenerated_gc);
}

void ShenandoahHeap::entry_init_mark() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark);

  const char* msg = init_mark_event_message();
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_init_marking(),
                              "init marking");

  op_init_mark();
}

void ShenandoahHeap::entry_final_mark() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_mark);

  const char* msg = final_mark_event_message();
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_final_marking(),
                              "final marking");

  op_final_mark();
}

void ShenandoahHeap::entry_final_evac() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_evac);

  const char* msg = "Pause Final Evac";
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id());
  EventMark em("%s", msg);

  op_final_evac();
}

void ShenandoahHeap::entry_init_updaterefs() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_refs);

  static const char* msg = "Pause Init Update Refs";
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id());
  EventMark em("%s", msg);

  // No workers used in this phase, no setup required

  op_init_updaterefs();
}

void ShenandoahHeap::entry_final_updaterefs() {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs);

  static const char* msg = "Pause Final Update Refs";
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_final_update_ref(),
                              "final reference update");

  op_final_updaterefs();
}

void ShenandoahHeap::entry_full(GCCause::Cause cause) {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::full_gc);

  static const char* msg = "Pause Full";
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_fullgc(),
                              "full gc");

  op_full(cause);
}

void ShenandoahHeap::entry_degenerated(int point) {
  ShenandoahGCPhase total_phase(ShenandoahPhaseTimings::total_pause);
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::degen_gc);

  ShenandoahDegenPoint dpoint = (ShenandoahDegenPoint)point;
  const char* msg = degen_event_message(dpoint);
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_stw_degenerated(),
                              "stw degenerated gc");

  set_degenerated_gc_in_progress(true);
  op_degenerated(dpoint);
  set_degenerated_gc_in_progress(false);
}

void ShenandoahHeap::entry_mark() {
  TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());

  const char* msg = conc_mark_event_message();
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_marking(),
                              "concurrent marking");

  try_inject_alloc_failure();
  op_mark();
}

void ShenandoahHeap::entry_evac() {
  ShenandoahGCPhase conc_evac_phase(ShenandoahPhaseTimings::conc_evac);
  TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());

  static const char *msg = "Concurrent evacuation";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_evac(),
                              "concurrent evacuation");

  try_inject_alloc_failure();
  op_evac();
}

void ShenandoahHeap::entry_updaterefs() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_update_refs);

  static const char* msg = "Concurrent update references";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_update_ref(),
                              "concurrent reference update");

  try_inject_alloc_failure();
  op_updaterefs();
}
void ShenandoahHeap::entry_cleanup() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_cleanup);

  static const char* msg = "Concurrent cleanup";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  // This phase does not use workers, no need for setup

  try_inject_alloc_failure();
  op_cleanup();
}

void ShenandoahHeap::entry_cleanup_bitmaps() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_cleanup);

  static const char* msg = "Concurrent cleanup";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_cleanup(),
                              "concurrent cleanup");

  try_inject_alloc_failure();
  op_cleanup_bitmaps();
}

void ShenandoahHeap::entry_preclean() {
  if (ShenandoahPreclean && process_references()) {
    ShenandoahGCPhase conc_preclean(ShenandoahPhaseTimings::conc_preclean);

    static const char* msg = "Concurrent precleaning";
    GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
    EventMark em("%s", msg);

    ShenandoahWorkerScope scope(workers(),
                                ShenandoahWorkerPolicy::calc_workers_for_conc_preclean(),
                                "concurrent preclean");

    try_inject_alloc_failure();
    op_preclean();
  }
}

void ShenandoahHeap::entry_uncommit(double shrink_before) {
  static const char *msg = "Concurrent uncommit";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_uncommit);

  op_uncommit(shrink_before);
}

void ShenandoahHeap::try_inject_alloc_failure() {
  if (ShenandoahAllocFailureALot && !cancelled_gc() && ((os::random() % 1000) > 950)) {
    _inject_alloc_failure.set();
    os::naked_short_sleep(1);
    if (cancelled_gc()) {
      log_info(gc)("Allocation failure was successfully injected");
    }
  }
}

bool ShenandoahHeap::should_inject_alloc_failure() {
  return _inject_alloc_failure.is_set() && _inject_alloc_failure.try_unset();
}

void ShenandoahHeap::enter_evacuation() {
  _oom_evac_handler.enter_evacuation();
}

void ShenandoahHeap::leave_evacuation() {
  _oom_evac_handler.leave_evacuation();
}

ShenandoahRegionIterator::ShenandoahRegionIterator() :
  _index(0),
  _heap(ShenandoahHeap::heap()) {}

ShenandoahRegionIterator::ShenandoahRegionIterator(ShenandoahHeap* heap) :
  _index(0),
  _heap(heap) {}

void ShenandoahRegionIterator::reset() {
  _index = 0;
}

bool ShenandoahRegionIterator::has_next() const {
  return _index < (jint)_heap->num_regions();
}

void ShenandoahHeap::heap_region_iterate(ShenandoahHeapRegionClosure& cl) const {
  ShenandoahRegionIterator regions;
  ShenandoahHeapRegion* r = regions.next();
  while (r != NULL) {
    if (cl.heap_region_do(r)) {
      break;
    }
    r = regions.next();
  }
}

char ShenandoahHeap::gc_state() {
  return _gc_state.raw_value();
}

const char* ShenandoahHeap::init_mark_event_message() const {
  bool update_refs = has_forwarded_objects();
  bool proc_refs = process_references();
  bool unload_cls = unload_classes();

  if (update_refs && proc_refs && unload_cls) {
    return "Pause Init Mark (update refs) (process refs) (unload classes)";
  } else if (update_refs && proc_refs) {
    return "Pause Init Mark (update refs) (process refs)";
  } else if (update_refs && unload_cls) {
    return "Pause Init Mark (update refs) (unload classes)";
  } else if (proc_refs && unload_cls) {
    return "Pause Init Mark (process refs) (unload classes)";
  } else if (update_refs) {
    return "Pause Init Mark (update refs)";
  } else if (proc_refs) {
    return "Pause Init Mark (process refs)";
  } else if (unload_cls) {
    return "Pause Init Mark (unload classes)";
  } else {
    return "Pause Init Mark";
  }
}

const char* ShenandoahHeap::final_mark_event_message() const {
  bool update_refs = has_forwarded_objects();
  bool proc_refs = process_references();
  bool unload_cls = unload_classes();

  if (update_refs && proc_refs && unload_cls) {
    return "Pause Final Mark (update refs) (process refs) (unload classes)";
  } else if (update_refs && proc_refs) {
    return "Pause Final Mark (update refs) (process refs)";
  } else if (update_refs && unload_cls) {
    return "Pause Final Mark (update refs) (unload classes)";
  } else if (proc_refs && unload_cls) {
    return "Pause Final Mark (process refs) (unload classes)";
  } else if (update_refs) {
    return "Pause Final Mark (update refs)";
  } else if (proc_refs) {
    return "Pause Final Mark (process refs)";
  } else if (unload_cls) {
    return "Pause Final Mark (unload classes)";
  } else {
    return "Pause Final Mark";
  }
}

const char* ShenandoahHeap::conc_mark_event_message() const {
  bool update_refs = has_forwarded_objects();
  bool proc_refs = process_references();
  bool unload_cls = unload_classes();

  if (update_refs && proc_refs && unload_cls) {
    return "Concurrent marking (update refs) (process refs) (unload classes)";
  } else if (update_refs && proc_refs) {
    return "Concurrent marking (update refs) (process refs)";
  } else if (update_refs && unload_cls) {
    return "Concurrent marking (update refs) (unload classes)";
  } else if (proc_refs && unload_cls) {
    return "Concurrent marking (process refs) (unload classes)";
  } else if (update_refs) {
    return "Concurrent marking (update refs)";
  } else if (proc_refs) {
    return "Concurrent marking (process refs)";
  } else if (unload_cls) {
    return "Concurrent marking (unload classes)";
  } else {
    return "Concurrent marking";
  }
}

const char* ShenandoahHeap::degen_event_message(ShenandoahDegenPoint point) const {
  switch (point) {
    case _degenerated_unset:
      return "Pause Degenerated GC (<UNSET>)";
    case _degenerated_outside_cycle:
      return "Pause Degenerated GC (Outside of Cycle)";
    case _degenerated_mark:
      return "Pause Degenerated GC (Mark)";
    case _degenerated_evac:
      return "Pause Degenerated GC (Evacuation)";
    case _degenerated_updaterefs:
      return "Pause Degenerated GC (Update Refs)";
    default:
      ShouldNotReachHere();
      return "ERROR";
  }
}


BoolObjectClosure* ShenandoahIsAliveSelector::is_alive_closure() {
  return ShenandoahHeap::heap()->has_forwarded_objects() ? reinterpret_cast<BoolObjectClosure*>(&_fwd_alive_cl)
                                                         : reinterpret_cast<BoolObjectClosure*>(&_alive_cl);
}
