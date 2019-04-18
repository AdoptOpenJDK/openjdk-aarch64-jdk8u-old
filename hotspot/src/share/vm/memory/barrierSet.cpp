/*
 * Copyright (c) 1997, 2011, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
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
#include "gc_interface/collectedHeap.hpp"
#include "memory/barrierSet.inline.hpp"
#include "memory/universe.hpp"

// count is number of array elements being written
void BarrierSet::static_write_ref_array_pre(HeapWord* start, size_t count) {
  assert(count <= (size_t)max_intx, "count too large");
#if 0
  warning("Pre: \t" INTPTR_FORMAT "[" SIZE_FORMAT "]\t",
                   start,            count);
#endif
  if (UseCompressedOops) {
    Universe::heap()->barrier_set()->write_ref_array_pre((narrowOop*)start, (int)count, false);
  } else {
    Universe::heap()->barrier_set()->write_ref_array_pre(      (oop*)start, (int)count, false);
  }
}

// count is number of array elements being written
void BarrierSet::write_ref_array(HeapWord* start, size_t count) {
  assert(count <= (size_t)max_intx, "count too large");
  HeapWord* end = (HeapWord*)((char*)start + (count*heapOopSize));
  // In the case of compressed oops, start and end may potentially be misaligned;
  // so we need to conservatively align the first downward (this is not
  // strictly necessary for current uses, but a case of good hygiene and,
  // if you will, aesthetics) and the second upward (this is essential for
  // current uses) to a HeapWord boundary, so we mark all cards overlapping
  // this write. If this evolves in the future to calling a
  // logging barrier of narrow oop granularity, like the pre-barrier for G1
  // (mentioned here merely by way of example), we will need to change this
  // interface, so it is "exactly precise" (if i may be allowed the adverbial
  // redundancy for emphasis) and does not include narrow oop slots not
  // included in the original write interval.
  HeapWord* aligned_start = (HeapWord*)align_size_down((uintptr_t)start, HeapWordSize);
  HeapWord* aligned_end   = (HeapWord*)align_size_up  ((uintptr_t)end,   HeapWordSize);
  // If compressed oops were not being used, these should already be aligned
  assert(UseCompressedOops || (aligned_start == start && aligned_end == end),
         "Expected heap word alignment of start and end");
#if 0
  warning("Post:\t" INTPTR_FORMAT "[" SIZE_FORMAT "] : [" INTPTR_FORMAT "," INTPTR_FORMAT ")\t",
                   start,            count,              aligned_start,   aligned_end);
#endif
  write_ref_array_work(MemRegion(aligned_start, aligned_end));
}

// count is number of array elements being written
void BarrierSet::static_write_ref_array_post(HeapWord* start, size_t count) {
  // simply delegate to instance method
  Universe::heap()->barrier_set()->write_ref_array(start, count);
}

bool BarrierSet::obj_equals(oop obj1, oop obj2) {
  return oopDesc::unsafe_equals(obj1, obj2);
}

bool BarrierSet::obj_equals(narrowOop obj1, narrowOop obj2) {
  return oopDesc::unsafe_equals(obj1, obj2);
}

