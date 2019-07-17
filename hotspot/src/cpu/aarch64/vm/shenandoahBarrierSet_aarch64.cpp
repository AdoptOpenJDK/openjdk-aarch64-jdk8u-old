/*
 * Copyright (c) 2015, 2018, Red Hat, Inc. All rights reserved.
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

#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahBrooksPointer.hpp"

#include "asm/macroAssembler.hpp"
#include "interpreter/interpreter.hpp"

#define __ masm->

#ifndef CC_INTERP
void ShenandoahBarrierSet::interpreter_read_barrier(MacroAssembler* masm, Register dst) {
  if (ShenandoahReadBarrier) {
    Label is_null;
    __ cbz(dst, is_null);
    interpreter_read_barrier_not_null(masm, dst);
    __ bind(is_null);
  }
}

void ShenandoahBarrierSet::interpreter_read_barrier_not_null(MacroAssembler* masm, Register dst) {
  if (ShenandoahReadBarrier) {
    __ ldr(dst, Address(dst, ShenandoahBrooksPointer::byte_offset()));
  }
}

void ShenandoahBarrierSet::interpreter_write_barrier(MacroAssembler* masm, Register dst) {
  if (! ShenandoahWriteBarrier) {
    return interpreter_read_barrier(masm, dst);
  }

  assert(dst != rscratch1, "different regs");
  assert(dst != rscratch2, "Need rscratch2");

  Label done;

  Address gc_state(rthread, in_bytes(JavaThread::gc_state_offset()));
  __ ldrb(rscratch1, gc_state);

  // Check for heap stability
  __ mov(rscratch2, ShenandoahHeap::HAS_FORWARDED | ShenandoahHeap::EVACUATION);
  __ tst(rscratch1, rscratch2);
  __ br(Assembler::EQ, done);

  // Heap is unstable, need to perform the read-barrier even if WB is inactive
  __ ldr(dst, Address(dst, ShenandoahBrooksPointer::byte_offset()));

  // Check for evacuation-in-progress and jump to WB slow-path if needed
  __ mov(rscratch2, ShenandoahHeap::EVACUATION);
  __ tst(rscratch1, rscratch2);
  __ br(Assembler::EQ, done);

  __ lsr(rscratch1, dst, ShenandoahHeapRegion::region_size_bytes_shift_jint());
  __ mov(rscratch2,  ShenandoahHeap::in_cset_fast_test_addr());
  __ ldrb(rscratch2, Address(rscratch2, rscratch1));
  __ tst(rscratch2, 0x1);
  __ br(Assembler::EQ, done);

  // Save possibly live regs.
  RegSet live_regs = RegSet::range(r0, r4) - dst;
  __ push(live_regs, sp);
  __ strd(v0, __ pre(sp, 2 * -wordSize));

  // Call into runtime
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahBarrierSet::write_barrier_IRT), dst);

  // Move result into dst reg.
  __ mov(dst, r0);

  // Restore possibly live regs.
  __ ldrd(v0, __ post(sp, 2 * wordSize));
  __ pop(live_regs, sp);

  __ bind(done);
}

void ShenandoahHeap::compile_prepare_oop(MacroAssembler* masm, Register obj) {
  __ add(obj, obj, ShenandoahBrooksPointer::byte_size());
  __ str(obj, Address(obj, -1 * HeapWordSize));
}

void ShenandoahBarrierSet::asm_acmp_barrier(MacroAssembler* masm,
                                            Register op1, Register op2) {
  assert(UseShenandoahGC, "Should be enabled");
  if (ShenandoahAcmpBarrier) {
    Label done;
    __ br(Assembler::EQ, done);
    // The object may have been evacuated, but we won't see it without a
    // membar here.
    __ membar(Assembler::LoadStore|Assembler::LoadLoad);
    interpreter_read_barrier(masm, op1);
    interpreter_read_barrier(masm, op2);
    __ cmp(op1, op2);
    __ bind(done);
  }
}

#endif
