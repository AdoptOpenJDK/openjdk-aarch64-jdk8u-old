/*
 * Copyright (c) 2017, 2019, Red Hat, Inc. All rights reserved.
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
#include "code/codeCache.hpp"
#include "code/nmethod.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahCodeRoots.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/vmThread.hpp"

ShenandoahParallelCodeCacheIterator::ShenandoahParallelCodeCacheIterator() :
        _claimed_idx(0), _finished(false) {
}

void ShenandoahParallelCodeCacheIterator::parallel_blobs_do(CodeBlobClosure* f) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint");

  /*
   * Parallel code heap walk.
   *
   * This code makes all threads scan all code heaps, but only one thread would execute the
   * closure on given blob. This is achieved by recording the "claimed" blocks: if a thread
   * had claimed the block, it can process all blobs in it. Others have to fast-forward to
   * next attempt without processing.
   *
   * Late threads would return immediately if iterator is finished.
   */

  if (_finished) {
    return;
  }

  int stride = 256; // educated guess
  int stride_mask = stride - 1;
  assert (is_power_of_2(stride), "sanity");

  int count = 0;
  bool process_block = true;

  for (CodeBlob *cb = CodeCache::first(); cb != NULL; cb = CodeCache::next(cb)) {
    int current = count++;
    if ((current & stride_mask) == 0) {
      process_block = (current >= _claimed_idx) &&
                      (Atomic::cmpxchg(current + stride, &_claimed_idx, current) == current);
    }
    if (process_block) {
      if (cb->is_alive()) {
        f->do_code_blob(cb);
#ifdef ASSERT
        if (cb->is_nmethod())
          ((nmethod*)cb)->verify_scavenge_root_oops();
#endif
      }
    }
  }

  _finished = true;
}

class ShenandoahNMethodOopDetector : public OopClosure {
private:
  ResourceMark rm; // For growable array allocation below.
  GrowableArray<oop*> _oops;

public:
  ShenandoahNMethodOopDetector() : _oops(10) {};

  void do_oop(oop* o) {
    _oops.append(o);
  }

  void do_oop(narrowOop* o) {
    fatal("NMethods should not have compressed oops embedded.");
  }

  GrowableArray<oop*>* oops() {
    return &_oops;
  }

  bool has_oops() {
    return !_oops.is_empty();
  }
};

ShenandoahCodeRoots::PaddedLock ShenandoahCodeRoots::_recorded_nms_lock;
GrowableArray<ShenandoahNMethod*>* ShenandoahCodeRoots::_recorded_nms;

void ShenandoahCodeRoots::initialize() {
  _recorded_nms_lock._lock = 0;
  _recorded_nms = new (ResourceObj::C_HEAP, mtGC) GrowableArray<ShenandoahNMethod*>(100, true, mtGC);
}

void ShenandoahCodeRoots::add_nmethod(nmethod* nm) {
  switch (ShenandoahCodeRootsStyle) {
    case 0:
    case 1:
      break;
    case 2: {
      ShenandoahNMethodOopDetector detector;
      nm->oops_do(&detector);

      if (detector.has_oops()) {
        ShenandoahNMethod* nmr = new ShenandoahNMethod(nm, detector.oops());
        nmr->assert_alive_and_correct();

        ShenandoahCodeRootsLock lock(true);

        int idx = _recorded_nms->find(nm, ShenandoahNMethod::find_with_nmethod);
        if (idx != -1) {
          ShenandoahNMethod* old = _recorded_nms->at(idx);
          _recorded_nms->at_put(idx, nmr);
          delete old;
        } else {
          _recorded_nms->append(nmr);
        }
      }
      break;
    }
    default:
      ShouldNotReachHere();
  }
};

void ShenandoahCodeRoots::remove_nmethod(nmethod* nm) {
  switch (ShenandoahCodeRootsStyle) {
    case 0:
    case 1: {
      break;
    }
    case 2: {
      ShenandoahNMethodOopDetector detector;
      nm->oops_do(&detector, /* allow_zombie = */ true);

      if (detector.has_oops()) {
        ShenandoahCodeRootsLock lock(true);

        int idx = _recorded_nms->find(nm, ShenandoahNMethod::find_with_nmethod);
        assert(idx != -1, err_msg("nmethod " PTR_FORMAT " should be registered", p2i(nm)));
        ShenandoahNMethod* old = _recorded_nms->at(idx);
        old->assert_same_oops(detector.oops());
        _recorded_nms->delete_at(idx);
        delete old;
      }
      break;
    }
    default:
      ShouldNotReachHere();
  }
}

ShenandoahCodeRootsIterator::ShenandoahCodeRootsIterator() :
        _heap(ShenandoahHeap::heap()),
        _claimed(0) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint");
  assert(!Thread::current()->is_Worker_thread(), "Should not be acquired by workers");
  switch (ShenandoahCodeRootsStyle) {
    case 0:
    case 1:
      break;
    case 2:
      ShenandoahCodeRoots::acquire_lock(false);
      break;
    default:
      ShouldNotReachHere();
  }
}

ShenandoahCodeRootsIterator::~ShenandoahCodeRootsIterator() {
  switch (ShenandoahCodeRootsStyle) {
    case 0:
    case 1: {
      // No need to do anything here
      break;
    }
    case 2: {
      ShenandoahCodeRoots::release_lock(false);
      break;
    }
    default:
      ShouldNotReachHere();
  }
}

template<bool CSET_FILTER>
void ShenandoahCodeRootsIterator::dispatch_parallel_blobs_do(CodeBlobClosure *f) {
  switch (ShenandoahCodeRootsStyle) {
    case 0: {
      if (_seq_claimed.try_set()) {
        CodeCache::blobs_do(f);
      }
      break;
    }
    case 1: {
      _par_iterator.parallel_blobs_do(f);
      break;
    }
    case 2: {
      ShenandoahCodeRootsIterator::fast_parallel_blobs_do<CSET_FILTER>(f);
      break;
    }
    default:
      ShouldNotReachHere();
  }
}

ShenandoahAllCodeRootsIterator ShenandoahCodeRoots::iterator() {
  return ShenandoahAllCodeRootsIterator();
}

ShenandoahCsetCodeRootsIterator ShenandoahCodeRoots::cset_iterator() {
  return ShenandoahCsetCodeRootsIterator();
}

void ShenandoahAllCodeRootsIterator::possibly_parallel_blobs_do(CodeBlobClosure *f) {
  ShenandoahCodeRootsIterator::dispatch_parallel_blobs_do<false>(f);
}

void ShenandoahCsetCodeRootsIterator::possibly_parallel_blobs_do(CodeBlobClosure *f) {
  ShenandoahCodeRootsIterator::dispatch_parallel_blobs_do<true>(f);
}

template <bool CSET_FILTER>
void ShenandoahCodeRootsIterator::fast_parallel_blobs_do(CodeBlobClosure *f) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint");

  size_t stride = 256; // educated guess

  GrowableArray<ShenandoahNMethod*>* list = ShenandoahCodeRoots::_recorded_nms;

  jlong max = list->length();
  while (_claimed < max) {
    size_t cur = (size_t)Atomic::add(stride, &_claimed); // Note: in JDK 8, add() returns old value
    size_t start = cur;
    size_t end = MIN2(cur + stride, (size_t) max);
    if (start >= (size_t)max) break;

    for (size_t idx = start; idx < end; idx++) {
      ShenandoahNMethod* nmr = list->at((int) idx);
      nmr->assert_alive_and_correct();

      if (CSET_FILTER && !nmr->has_cset_oops(_heap)) {
        continue;
      }

      f->do_code_blob(nmr->nm());
    }
  }
}

ShenandoahNMethod::ShenandoahNMethod(nmethod* nm, GrowableArray<oop*>* oops) {
  _nm = nm;
  _oops = NEW_C_HEAP_ARRAY(oop*, oops->length(), mtGC);
  _oops_count = oops->length();
  for (int c = 0; c < _oops_count; c++) {
    _oops[c] = oops->at(c);
  }
}

ShenandoahNMethod::~ShenandoahNMethod() {
  if (_oops != NULL) {
    FREE_C_HEAP_ARRAY(oop*, _oops, mtGC);
  }
}

bool ShenandoahNMethod::has_cset_oops(ShenandoahHeap *heap) {
  for (int c = 0; c < _oops_count; c++) {
    oop o = oopDesc::load_heap_oop(_oops[c]);
    if (heap->in_collection_set(o)) {
      return true;
    }
  }
  return false;
}

#ifdef ASSERT
void ShenandoahNMethod::assert_alive_and_correct() {
  assert(_nm->is_alive(), "only alive nmethods here");
  assert(_oops_count > 0, "should have filtered nmethods without oops before");
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  for (int c = 0; c < _oops_count; c++) {
    oop *loc = _oops[c];
    assert(_nm->code_contains((address) loc) || _nm->oops_contains(loc), "nmethod should contain the oop*");
    oop o = oopDesc::load_heap_oop(loc);
    shenandoah_assert_correct_except(loc, o,
             o == NULL ||
             heap->is_full_gc_move_in_progress() ||
             (VMThread::vm_operation() != NULL) && (VMThread::vm_operation()->type() == VM_Operation::VMOp_HeapWalkOperation)
    );
  }
}

void ShenandoahNMethod::assert_same_oops(GrowableArray<oop*>* oops) {
  assert(_oops_count == oops->length(), "should have the same number of oop*");
  for (int c = 0; c < _oops_count; c++) {
    assert(_oops[c] == oops->at(c), "should be the same oop*");
  }
}
#endif
