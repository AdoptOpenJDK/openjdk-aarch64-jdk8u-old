/*
 * Copyright (c) 2013, 2017, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP

#include "gc_implementation/shared/markBitMap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapLock.hpp"
#include "gc_implementation/shenandoah/shenandoahEvacOOMHandler.hpp"
#include "gc_implementation/shenandoah/shenandoahSharedVariables.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkGroup.hpp"

class ConcurrentGCTimer;

class ShenandoahAllocTracker;
class ShenandoahAsserts;
class ShenandoahCollectionSet;
class ShenandoahCollectorPolicy;
class ShenandoahConcurrentMark;
class ShenandoahControlThread;
class ShenandoahGCSession;
class ShenandoahFreeSet;
class ShenandoahHeapRegion;
class ShenandoahHeapRegionClosure;
class ShenandoahMarkCompact;
class ShenandoahMonitoringSupport;
class ShenandoahHeuristics;
class ShenandoahMarkingContext;
class ShenandoahPhaseTimings;
class ShenandoahPacer;
class ShenandoahVerifier;
class ShenandoahWorkGang;

class ShenandoahRegionIterator : public StackObj {
private:
  volatile jint _index;
  ShenandoahHeap* _heap;

  // No implicit copying: iterators should be passed by reference to capture the state
  ShenandoahRegionIterator(const ShenandoahRegionIterator& that);
  ShenandoahRegionIterator& operator=(const ShenandoahRegionIterator& o);

public:
  ShenandoahRegionIterator();
  ShenandoahRegionIterator(ShenandoahHeap* heap);

  // Reset iterator to default state
  void reset();

  // Returns next region, or NULL if there are no more regions.
  // This is multi-thread-safe.
  inline ShenandoahHeapRegion* next();

  // This is *not* MT safe. However, in the absence of multithreaded access, it
  // can be used to determine if there is more work to do.
  bool has_next() const;
};

class ShenandoahHeapRegionClosure : public StackObj {
public:
  // typically called on each region until it returns true;
  virtual bool heap_region_do(ShenandoahHeapRegion* r) = 0;
};

class ShenandoahUpdateRefsClosure: public OopClosure {
private:
  ShenandoahHeap* _heap;

  template <class T>
  inline void do_oop_work(T* p);

public:
  ShenandoahUpdateRefsClosure();
  inline void do_oop(oop* p);
  inline void do_oop(narrowOop* p);
};

#ifdef ASSERT
class ShenandoahAssertToSpaceClosure : public OopClosure {
private:
  template <class T>
  void do_oop_nv(T* p);
public:
  void do_oop(narrowOop* p);
  void do_oop(oop* p);
};
#endif

class ShenandoahAlwaysTrueClosure : public BoolObjectClosure {
public:
  bool do_object_b(oop p) { return true; }
};

class ShenandoahForwardedIsAliveClosure: public BoolObjectClosure {
private:
  ShenandoahMarkingContext* const _mark_context;
public:
  ShenandoahForwardedIsAliveClosure();
  bool do_object_b(oop obj);
};

class ShenandoahIsAliveClosure: public BoolObjectClosure {
private:
  ShenandoahMarkingContext* const _mark_context;
public:
  ShenandoahIsAliveClosure();
  bool do_object_b(oop obj);
};

class VMStructs;

// // A "ShenandoahHeap" is an implementation of a java heap for HotSpot.
// // It uses a new pauseless GC algorithm based on Brooks pointers.
// // Derived from G1

// //
// // CollectedHeap
// //    SharedHeap
// //      ShenandoahHeap

class ShenandoahHeap : public SharedHeap {
  friend class ShenandoahAsserts;
  friend class VMStructs;
  friend class ShenandoahGCSession;
public:
  // GC state describes the important parts of collector state, that may be
  // used to make barrier selection decisions in the native and generated code.
  // Multiple bits can be set at once.
  //
  // Important invariant: when GC state is zero, the heap is stable, and no barriers
  // are required.
  enum GCStateBitPos {
    // Heap has forwarded objects: need RB, ACMP, CAS barriers.
    HAS_FORWARDED_BITPOS   = 0,

    // Heap is under marking: needs SATB barriers.
    MARKING_BITPOS    = 1,

    // Heap is under evacuation: needs WB barriers. (Set together with UNSTABLE)
    EVACUATION_BITPOS = 2,

    // Heap is under updating: needs SVRB/SVWB barriers.
    UPDATEREFS_BITPOS = 3,
  };

  enum GCState {
    STABLE        = 0,
    HAS_FORWARDED = 1 << HAS_FORWARDED_BITPOS,
    MARKING       = 1 << MARKING_BITPOS,
    EVACUATION    = 1 << EVACUATION_BITPOS,
    UPDATEREFS    = 1 << UPDATEREFS_BITPOS,
  };

  enum ShenandoahDegenPoint {
    _degenerated_unset,
    _degenerated_outside_cycle,
    _degenerated_mark,
    _degenerated_evac,
    _degenerated_updaterefs,
    _DEGENERATED_LIMIT,
  };

  static const char* degen_point_to_string(ShenandoahDegenPoint point) {
    switch (point) {
      case _degenerated_unset:
        return "<UNSET>";
      case _degenerated_outside_cycle:
        return "Outside of Cycle";
      case _degenerated_mark:
        return "Mark";
      case _degenerated_evac:
        return "Evacuation";
      case _degenerated_updaterefs:
        return "Update Refs";
      default:
        ShouldNotReachHere();
        return "ERROR";
    }
  };

private:
  ShenandoahSharedBitmap _gc_state;
  ShenandoahHeapLock _lock;
  ShenandoahCollectorPolicy* _shenandoah_policy;
  ShenandoahHeuristics* _heuristics;
  size_t _bitmap_size;
  size_t _bitmap_regions_per_slice;
  size_t _bitmap_bytes_per_slice;
  MemRegion _heap_region;
  MemRegion _bitmap0_region;
  MemRegion _bitmap1_region;
  MemRegion _aux_bitmap_region;

  ShenandoahHeapRegion** _regions;
  ShenandoahFreeSet* _free_set;
  ShenandoahCollectionSet* _collection_set;

  ShenandoahRegionIterator _update_refs_iterator;

  ShenandoahConcurrentMark* _scm;
  ShenandoahMarkCompact* _full_gc;
  ShenandoahVerifier*  _verifier;
  ShenandoahPacer*  _pacer;



  ShenandoahControlThread* _control_thread;

  ShenandoahMonitoringSupport* _monitoring_support;

  ShenandoahPhaseTimings*      _phase_timings;
  ShenandoahAllocTracker*      _alloc_tracker;

  size_t _num_regions;
  size_t _initial_size;
  uint _max_workers;

  ShenandoahWorkGang* _workers;

  volatile jlong _used;
  volatile size_t _committed;

  MarkBitMap _verification_bit_map;
  MarkBitMap _aux_bit_map;

  ShenandoahMarkingContext* _complete_marking_context;
  ShenandoahMarkingContext* _next_marking_context;

  volatile jlong _bytes_allocated_since_gc_start;

  ShenandoahSharedFlag _progress_last_gc;

  ShenandoahSharedFlag _degenerated_gc_in_progress;
  ShenandoahSharedFlag _full_gc_in_progress;
  ShenandoahSharedFlag _full_gc_move_in_progress;

  ShenandoahSharedFlag _inject_alloc_failure;

  ShenandoahSharedFlag _process_references;
  ShenandoahSharedFlag _unload_classes;

  ShenandoahSharedFlag _cancelled_gc;

  ReferenceProcessor* _ref_processor;

  ConcurrentGCTimer* _gc_timer;

  ShenandoahEvacOOMHandler _oom_evac_handler;

#ifdef ASSERT
  int     _heap_expansion_count;
#endif

public:
  ShenandoahHeap(ShenandoahCollectorPolicy* policy);

  void initialize_heuristics();

  const char* name() const /* override */;
  HeapWord* allocate_new_tlab(size_t word_size) /* override */;
  void print_on(outputStream* st) const /* override */;
  void print_extended_on(outputStream *st) const /* override */;

  ShenandoahHeap::Name kind() const  /* override */{
    return CollectedHeap::ShenandoahHeap;
  }

  jint initialize() /* override */;
  void post_initialize() /* override */;
  size_t capacity() const /* override */;
  size_t used() const /* override */;
  size_t committed() const;
  bool is_maximal_no_gc() const /* override */;
  size_t max_capacity() const /* override */;
  size_t initial_capacity() const /* override */;
  bool is_in(const void* p) const /* override */;
  bool is_scavengable(const void* addr) /* override */;
  HeapWord* mem_allocate(size_t size, bool* what) /* override */;
  bool can_elide_tlab_store_barriers() const /* override */;
  oop new_store_pre_barrier(JavaThread* thread, oop new_obj) /* override */;
  bool can_elide_initializing_store_barrier(oop new_obj) /* override */;
  bool card_mark_must_follow_store() const /* override */;
  void collect(GCCause::Cause cause) /* override */;
  void do_full_collection(bool clear_all_soft_refs) /* override */;
  AdaptiveSizePolicy* size_policy() /* override */;
  CollectorPolicy* collector_policy() const /* override */;
  void ensure_parsability(bool retire_tlabs) /* override */;
  HeapWord* block_start(const void* addr) const /* override */;
  size_t block_size(const HeapWord* addr) const /* override */;
  bool block_is_obj(const HeapWord* addr) const /* override */;
  jlong millis_since_last_gc() /* override */;
  void prepare_for_verify() /* override */;
  void print_gc_threads_on(outputStream* st) const /* override */;
  void gc_threads_do(ThreadClosure* tcl) const /* override */;
  void print_tracing_info() const /* override */;
  void verify(bool silent, VerifyOption vo) /* override */;
  bool supports_tlab_allocation() const /* override */;
  size_t tlab_capacity(Thread *thr) const /* override */;
  void object_iterate(ObjectClosure* cl) /* override */;
  void safe_object_iterate(ObjectClosure* cl) /* override */;
  size_t unsafe_max_tlab_alloc(Thread *thread) const /* override */;
  size_t max_tlab_size() const /* override */;
  void resize_all_tlabs() /* override */;
  void accumulate_statistics_all_gclabs() /* override */;
  HeapWord* tlab_post_allocation_setup(HeapWord* obj) /* override */;
  uint oop_extra_words() /* override */;
  size_t tlab_used(Thread* ignored) const /* override */;
  void stop() /* override */;
  bool is_in_partial_collection(const void* p) /* override */;
  bool supports_heap_inspection() const /* override */;

  void space_iterate(SpaceClosure* scl) /* override */;
  void oop_iterate(ExtendedOopClosure* cl) /* override */;

  Space* space_containing(const void* oop) const;
  void gc_prologue(bool b);
  void gc_epilogue(bool b);

#ifndef CC_INTERP
  void compile_prepare_oop(MacroAssembler* masm, Register obj) /* override */;
#endif

  void register_nmethod(nmethod* nm);
  void unregister_nmethod(nmethod* nm);

  /* override: object pinning support */
  bool supports_object_pinning() const { return true; }
  oop pin_object(JavaThread* thread, oop obj);
  void unpin_object(JavaThread* thread, oop obj);

  static ShenandoahHeap* heap();
  static ShenandoahHeap* heap_no_check();
  static size_t conservative_max_heap_alignment();
  static address in_cset_fast_test_addr();
  static address cancelled_gc_addr();
  static address gc_state_addr();

  ShenandoahCollectorPolicy *shenandoahPolicy() const { return _shenandoah_policy; }
  ShenandoahHeuristics*     heuristics()        const { return _heuristics; }
  ShenandoahPhaseTimings*   phase_timings()     const { return _phase_timings; }
  ShenandoahAllocTracker*   alloc_tracker()     const { return _alloc_tracker; }

  inline ShenandoahHeapRegion* const heap_region_containing(const void* addr) const;
  inline size_t heap_region_index_containing(const void* addr) const;
  inline bool requires_marking(const void* entry) const;

  template <class T>
  inline oop maybe_update_with_forwarded(T* p);

  template <class T>
  inline oop maybe_update_with_forwarded_not_null(T* p, oop obj);

  template <class T>
  inline oop update_with_forwarded_not_null(T* p, oop obj);

  void trash_cset_regions();

  void stop_concurrent_marking();

  void prepare_for_concurrent_evacuation();
  void evacuate_and_update_roots();

  void update_heap_references(bool concurrent);

  void roots_iterate(OopClosure* cl);

private:
  void set_gc_state_mask(uint mask, bool value);

public:
  void set_concurrent_mark_in_progress(bool in_progress);
  void set_evacuation_in_progress(bool in_progress);
  void set_update_refs_in_progress(bool in_progress);
  void set_degenerated_gc_in_progress(bool in_progress);
  void set_full_gc_in_progress(bool in_progress);
  void set_full_gc_move_in_progress(bool in_progress);
  void set_has_forwarded_objects(bool cond);

  char gc_state();

  void set_process_references(bool pr);
  void set_unload_classes(bool uc);

  inline bool is_stable() const;
  inline bool is_idle() const;
  inline bool is_concurrent_mark_in_progress() const;
  inline bool is_update_refs_in_progress() const;
  inline bool is_evacuation_in_progress() const;
  inline bool is_degenerated_gc_in_progress() const;
  inline bool is_full_gc_in_progress() const;
  inline bool is_full_gc_move_in_progress() const;
  inline bool has_forwarded_objects() const;
  inline bool is_gc_in_progress_mask(uint mask) const;

  bool process_references() const;
  bool unload_classes() const;

  void force_satb_flush_all_threads();

  bool last_gc_made_progress() const;

  inline bool region_in_collection_set(size_t region_index) const;

  void acquire_pending_refs_lock();
  void release_pending_refs_lock();

  // Mainly there to avoid accidentally calling the templated
  // method below with ShenandoahHeapRegion* which would be *wrong*.
  inline bool in_collection_set(ShenandoahHeapRegion* r) const;

  template <class T>
  inline bool in_collection_set(T obj) const;

  // Evacuates object src. Returns the evacuated object if this thread
  // succeeded, otherwise rolls back the evacuation and returns the
  // evacuated object by the competing thread. 'succeeded' is an out
  // param and set to true if this thread succeeded, otherwise to false.
  inline oop  evacuate_object(oop src, Thread* thread, bool& evacuated);
  inline bool cancelled_gc() const;
  inline bool try_cancel_gc();
  inline void clear_cancelled_gc();

  inline ShenandoahHeapRegion* const get_region(size_t region_idx) const;
  void heap_region_iterate(ShenandoahHeapRegionClosure& cl) const;

  ShenandoahFreeSet* free_set()             const { return _free_set; }
  ShenandoahCollectionSet* collection_set() const { return _collection_set; }

  void increase_used(size_t bytes);
  void decrease_used(size_t bytes);

  void set_used(size_t bytes);

  void increase_committed(size_t bytes);
  void decrease_committed(size_t bytes);

  void increase_allocated(size_t bytes);

  void notify_mutator_alloc_words(size_t words, bool waste);

  void reset_next_mark_bitmap();

  inline ShenandoahMarkingContext* complete_marking_context() const {
    return _complete_marking_context;
  }

  inline ShenandoahMarkingContext* next_marking_context() const {
    return _next_marking_context;
  }

  bool commit_bitmap_slice(ShenandoahHeapRegion *r);
  bool uncommit_bitmap_slice(ShenandoahHeapRegion *r);
  bool is_bitmap_slice_committed(ShenandoahHeapRegion* r, bool skip_self = false);

  void print_heap_regions_on(outputStream* st) const;

  size_t bytes_allocated_since_gc_start();
  void reset_bytes_allocated_since_gc_start();

  void trash_humongous_region_at(ShenandoahHeapRegion *r);

  ShenandoahMonitoringSupport* monitoring_support();
  ShenandoahConcurrentMark* concurrentMark() { return _scm; }
  ShenandoahMarkCompact* full_gc() { return _full_gc; }
  ShenandoahVerifier* verifier();
  ShenandoahPacer* pacer() const;

  ReferenceProcessor* ref_processor() { return _ref_processor;}

  ShenandoahWorkGang* workers() const { return _workers;}

  uint max_workers();

  void assert_gc_workers(uint nworker) PRODUCT_RETURN;

  ShenandoahHeapRegion* next_compaction_region(const ShenandoahHeapRegion* r);

  void heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_cset_regions = false, bool skip_humongous_continuation = false) const;

  // Delete entries for dead interned string and clean up unreferenced symbols
  // in symbol table, possibly in parallel.
  void unload_classes_and_cleanup_tables(bool full_gc);

  inline size_t num_regions() const { return _num_regions; }

  // Call before starting evacuation.
  void enter_evacuation();
  // Call after finished with evacuation.
  void leave_evacuation();

private:
  template<class T>
  inline void marked_object_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit);

  template<class T>
  inline void marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit);

public:
  template<class T>
  inline void marked_object_iterate(ShenandoahHeapRegion* region, T* cl);

  template<class T>
  inline void marked_object_safe_iterate(ShenandoahHeapRegion* region, T* cl);

  template<class T>
  inline void marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl);

  template<class T>
  inline void marked_object_oop_safe_iterate(ShenandoahHeapRegion* region, T* cl);

  GCTimer* gc_timer() const;
  GCTracer* tracer();

  void swap_mark_contexts();

  void cancel_gc(GCCause::Cause cause);

  ShenandoahHeapLock* lock() { return &_lock; }
  void assert_heaplock_owned_by_current_thread() PRODUCT_RETURN;
  void assert_heaplock_not_owned_by_current_thread() PRODUCT_RETURN;
  void assert_heaplock_or_safepoint() PRODUCT_RETURN;

public:
  typedef enum {
    _alloc_shared,      // Allocate common, outside of TLAB
    _alloc_shared_gc,   // Allocate common, outside of GCLAB
    _alloc_tlab,        // Allocate TLAB
    _alloc_gclab,       // Allocate GCLAB
    _ALLOC_LIMIT,
  } AllocType;

  static const char* alloc_type_to_string(AllocType type) {
    switch (type) {
      case _alloc_shared:
        return "Shared";
      case _alloc_shared_gc:
        return "Shared GC";
      case _alloc_tlab:
        return "TLAB";
      case _alloc_gclab:
        return "GCLAB";
      default:
        ShouldNotReachHere();
        return "";
    }
  }

  class ShenandoahAllocationRequest : StackObj {
  private:
    size_t _min_size;
    size_t _requested_size;
    size_t _actual_size;
    AllocType _alloc_type;
#ifdef ASSERT
    bool _actual_size_set;
#endif

    ShenandoahAllocationRequest(size_t _min_size, size_t _requested_size, AllocType _alloc_type) :
            _min_size(_min_size), _requested_size(_requested_size),
            _actual_size(0), _alloc_type(_alloc_type)
#ifdef ASSERT
            , _actual_size_set(false)
#endif
    {}

  public:
    static inline ShenandoahAllocationRequest for_tlab(size_t requested_size) {
      return ShenandoahAllocationRequest(requested_size, requested_size, ShenandoahHeap::_alloc_tlab);
    }

    static inline ShenandoahAllocationRequest for_gclab(size_t min_size, size_t requested_size) {
      return ShenandoahAllocationRequest(min_size, requested_size, ShenandoahHeap::_alloc_gclab);
    }

    static inline ShenandoahAllocationRequest for_shared_gc(size_t requested_size) {
      return ShenandoahAllocationRequest(0, requested_size, ShenandoahHeap::_alloc_shared_gc);
    }

    static inline ShenandoahAllocationRequest for_shared(size_t requested_size) {
      return ShenandoahAllocationRequest(0, requested_size, ShenandoahHeap::_alloc_shared);
    }

    inline size_t size() {
      return _requested_size;
    }

    inline AllocType type() {
      return _alloc_type;
    }

    inline size_t min_size() {
      assert (is_lab_alloc(), "Only access for LAB allocs");
      return _min_size;
    }

    inline size_t actual_size() {
      assert (_actual_size_set, "Should be set");
      return _actual_size;
    }

    inline void set_actual_size(size_t v) {
#ifdef ASSERT
      assert (!_actual_size_set, "Should not be set");
      _actual_size_set = true;
#endif
      _actual_size = v;
    }

    inline bool is_mutator_alloc() {
      switch (_alloc_type) {
        case _alloc_tlab:
        case _alloc_shared:
          return true;
        case _alloc_gclab:
        case _alloc_shared_gc:
          return false;
        default:
          ShouldNotReachHere();
          return false;
      }
    }

    inline bool is_gc_alloc() {
      switch (_alloc_type) {
        case _alloc_tlab:
        case _alloc_shared:
          return false;
        case _alloc_gclab:
        case _alloc_shared_gc:
          return true;
        default:
          ShouldNotReachHere();
          return false;
      }
    }

    inline bool is_lab_alloc() {
      switch (_alloc_type) {
        case _alloc_tlab:
        case _alloc_gclab:
          return true;
        case _alloc_shared:
        case _alloc_shared_gc:
          return false;
        default:
          ShouldNotReachHere();
          return false;
      }
    }
  };


private:
  HeapWord* allocate_memory_under_lock(ShenandoahAllocationRequest& request, bool& in_new_region);
  HeapWord* allocate_memory(ShenandoahAllocationRequest& request);

  // Shenandoah functionality.
  inline HeapWord* allocate_from_gclab(Thread* thread, size_t size);
  HeapWord* allocate_from_gclab_slow(Thread* thread, size_t size);
  HeapWord* allocate_new_gclab(size_t min_size, size_t word_size, size_t* actual_size);

  template<class T>
  inline void do_object_marked_complete(T* cl, oop obj);

  ShenandoahControlThread* control_thread() { return _control_thread; }

  inline oop atomic_compare_exchange_oop(oop n, narrowOop* addr, oop c);
  inline oop atomic_compare_exchange_oop(oop n, oop* addr, oop c);

  void ref_processing_init();

public:
  void make_parsable(bool retire_tlabs);
  void accumulate_statistics_tlabs();
  void resize_tlabs();

public:
  // Entry points to STW GC operations, these cause a related safepoint, that then
  // call the entry method below
  void vmop_entry_init_mark();
  void vmop_entry_final_mark();
  void vmop_entry_final_evac();
  void vmop_entry_init_updaterefs();
  void vmop_entry_final_updaterefs();
  void vmop_entry_full(GCCause::Cause cause);
  void vmop_degenerated(ShenandoahDegenPoint point);

  // Entry methods to normally STW GC operations. These set up logging, monitoring
  // and workers for net VM operation
  void entry_init_mark();
  void entry_final_mark();
  void entry_final_evac();
  void entry_init_updaterefs();
  void entry_final_updaterefs();
  void entry_full(GCCause::Cause cause);
  void entry_degenerated(int point);

  // Entry methods to normally concurrent GC operations. These set up logging, monitoring
  // for concurrent operation.
  void entry_mark();
  void entry_preclean();
  void entry_cleanup();
  void entry_cleanup_bitmaps();
  void entry_evac();
  void entry_updaterefs();
  void entry_uncommit(double shrink_before);

private:
  // Actual work for the phases
  void op_init_mark();
  void op_final_mark();
  void op_final_evac();
  void op_init_updaterefs();
  void op_final_updaterefs();
  void op_full(GCCause::Cause cause);
  void op_degenerated(ShenandoahDegenPoint point);
  void op_degenerated_fail();
  void op_degenerated_futile();

  void op_mark();
  void op_preclean();
  void op_cleanup();
  void op_evac();
  void op_updaterefs();
  void op_cleanup_bitmaps();
  void op_uncommit(double shrink_before);

  // Messages for GC trace event, they have to be immortal for
  // passing around the logging/tracing systems
  const char* init_mark_event_message() const;
  const char* final_mark_event_message() const;
  const char* conc_mark_event_message() const;
  const char* degen_event_message(ShenandoahDegenPoint point) const;

private:
  void try_inject_alloc_failure();
  bool should_inject_alloc_failure();
};

class ShenandoahIsAliveSelector : public StackObj {
private:
  ShenandoahIsAliveClosure _alive_cl;
  ShenandoahForwardedIsAliveClosure _fwd_alive_cl;
public:
  BoolObjectClosure* is_alive_closure();
};


#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP
