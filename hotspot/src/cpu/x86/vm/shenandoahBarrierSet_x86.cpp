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

#include "precompiled.hpp"
#include "gc_implementation/shenandoah/shenandoahBrooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"

#include "asm/macroAssembler.hpp"
#include "interpreter/interpreter.hpp"

#define __ masm->

#ifndef CC_INTERP

void ShenandoahBarrierSet::interpreter_read_barrier(MacroAssembler* masm, Register dst) {
  if (ShenandoahReadBarrier) {
    Label is_null;
    __ testptr(dst, dst);
    __ jcc(Assembler::zero, is_null);
    interpreter_read_barrier_not_null(masm, dst);
    __ bind(is_null);
  }
}

void ShenandoahBarrierSet::interpreter_read_barrier_not_null(MacroAssembler* masm, Register dst) {
  if (ShenandoahReadBarrier) {
    __ movptr(dst, Address(dst, ShenandoahBrooksPointer::byte_offset()));
  }
}

void ShenandoahBarrierSet::interpreter_write_barrier(MacroAssembler* masm, Register dst) {
  if (! ShenandoahWriteBarrier) {
    return interpreter_read_barrier(masm, dst);
  }

#ifdef _LP64
  assert(dst != rscratch1, "different regs");

  Label done;

  Address gc_state(r15_thread, in_bytes(JavaThread::gc_state_offset()));

  // Now check if evacuation is in progress.
  __ testb(gc_state, ShenandoahHeap::HAS_FORWARDED | ShenandoahHeap::EVACUATION);
  __ jcc(Assembler::zero, done);

  // Heap is unstable, need to perform the read-barrier even if WB is inactive
  interpreter_read_barrier_not_null(masm, dst);

  __ testb(gc_state, ShenandoahHeap::EVACUATION);
  __ jcc(Assembler::zero, done);
  __ push(rscratch1);
  __ push(rscratch2);

  __ movptr(rscratch1, dst);
  __ shrptr(rscratch1, ShenandoahHeapRegion::region_size_bytes_shift_jint());
  __ movptr(rscratch2, (intptr_t) ShenandoahHeap::in_cset_fast_test_addr());
  __ movbool(rscratch2, Address(rscratch2, rscratch1, Address::times_1));
  __ testb(rscratch2, 0x1);

  __ pop(rscratch2);
  __ pop(rscratch1);

  __ jcc(Assembler::zero, done);

  __ push(rscratch1);

  // Save possibly live regs.
  if (dst != rax) {
    __ push(rax);
  }
  if (dst != rbx) {
    __ push(rbx);
  }
  if (dst != rcx) {
    __ push(rcx);
  }
  if (dst != rdx) {
    __ push(rdx);
  }
  if (dst != c_rarg1) {
    __ push(c_rarg1);
  }

  __ subptr(rsp, 2 * wordSize);
  __ movdbl(Address(rsp, 0), xmm0);

  // Call into runtime
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahBarrierSet::write_barrier_IRT), dst);
  __ mov(rscratch1, rax);

  // Restore possibly live regs.
  __ movdbl(xmm0, Address(rsp, 0));
  __ addptr(rsp, 2 * Interpreter::stackElementSize);

  if (dst != c_rarg1) {
    __ pop(c_rarg1);
  }
  if (dst != rdx) {
    __ pop(rdx);
  }
  if (dst != rcx) {
    __ pop(rcx);
  }
  if (dst != rbx) {
    __ pop(rbx);
  }
  if (dst != rax) {
    __ pop(rax);
  }

  // Move result into dst reg.
  __ mov(dst, rscratch1);

  __ pop(rscratch1);

  __ bind(done);
#else
  Unimplemented();
#endif
}

void ShenandoahBarrierSet::asm_acmp_barrier(MacroAssembler* masm, Register op1, Register op2) {
  assert (UseShenandoahGC, "Should be enabled");
  if (ShenandoahAcmpBarrier) {
    Label done;
    __ jccb(Assembler::equal, done);
    interpreter_read_barrier(masm, op1);
    interpreter_read_barrier(masm, op2);
    __ cmpptr(op1, op2);
    __ bind(done);
  }
}

void ShenandoahHeap::compile_prepare_oop(MacroAssembler* masm, Register obj) {
#ifdef _LP64
  __ incrementq(obj, ShenandoahBrooksPointer::byte_size());
#else
  __ incrementl(obj, ShenandoahBrooksPointer::byte_size());
#endif
  __ movptr(Address(obj, ShenandoahBrooksPointer::byte_offset()), obj);
}
#endif
