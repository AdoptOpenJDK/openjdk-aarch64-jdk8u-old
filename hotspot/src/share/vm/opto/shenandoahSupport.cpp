/*
 * Copyright (c) 2015, Red Hat, Inc. and/or its affiliates.
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
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "opto/callnode.hpp"
#include "opto/connode.hpp"
#include "opto/phaseX.hpp"
#include "opto/rootnode.hpp"
#include "opto/runtime.hpp"
#include "opto/shenandoahSupport.hpp"
#include "opto/subnode.hpp"

Node* ShenandoahBarrierNode::skip_through_barrier(Node* n) {
  if (!UseShenandoahGC) {
    return n;
  }
  if (n == NULL) {
    return NULL;
  } else if (n->is_ShenandoahBarrier()) {
    return n->in(ValueIn);
  } else if (n->is_Phi() &&
             n->req() == 3 &&
             n->in(1) != NULL &&
             n->in(1)->is_ShenandoahBarrier() &&
             n->in(2) != NULL &&
             n->in(2)->bottom_type() == TypePtr::NULL_PTR &&
             n->in(0) != NULL &&
             n->in(0)->in(1) != NULL &&
             n->in(0)->in(1)->is_IfProj() &&
             n->in(0)->in(2) != NULL &&
             n->in(0)->in(2)->is_IfProj() &&
             n->in(0)->in(1)->in(0) != NULL &&
             n->in(0)->in(1)->in(0) == n->in(0)->in(2)->in(0) &&
             n->in(1)->in(ShenandoahBarrierNode::ValueIn)->Opcode() == Op_CastPP) {
    Node* iff = n->in(0)->in(1)->in(0);
    Node* res = n->in(1)->in(ShenandoahBarrierNode::ValueIn)->in(1);
    if (iff->is_If() &&
        iff->in(1) != NULL &&
        iff->in(1)->is_Bool() &&
        iff->in(1)->as_Bool()->_test._test == BoolTest::ne &&
        iff->in(1)->in(1) != NULL &&
        iff->in(1)->in(1)->Opcode() == Op_CmpP &&
        iff->in(1)->in(1)->in(1) != NULL &&
        iff->in(1)->in(1)->in(1) == res &&
        iff->in(1)->in(1)->in(2) != NULL &&
        iff->in(1)->in(1)->in(2)->bottom_type() == TypePtr::NULL_PTR) {
      return res;
    }
  }
  return n;
}

bool ShenandoahBarrierNode::needs_barrier(PhaseTransform* phase, ShenandoahBarrierNode* orig, Node* n, Node* rb_mem, bool allow_fromspace) {
  Unique_Node_List visited;
  return needs_barrier_impl(phase, orig, n, rb_mem, allow_fromspace, visited);
}

bool ShenandoahBarrierNode::needs_barrier_impl(PhaseTransform* phase, ShenandoahBarrierNode* orig, Node* n, Node* rb_mem, bool allow_fromspace, Unique_Node_List &visited) {

  if (visited.member(n)) {
    return false; // Been there.
  }
  visited.push(n);

  if (n->is_Allocate()) {
    // tty->print_cr("killed barrier for newly allocated object");
    return false;
  }

  if (n->is_CallJava() || n->Opcode() == Op_CallLeafNoFP) {
    return true;
  }

  const Type* type = phase->type(n);
  if (type == Type::TOP) {
    return false;
  }
  if (type->make_ptr()->higher_equal(TypePtr::NULL_PTR)) {
    // tty->print_cr("killed barrier for NULL object");
    return false;
  }
  if (type->make_oopptr() && type->make_oopptr()->const_oop() != NULL) {
    // tty->print_cr("killed barrier for constant object");
    return false;
  }

  if (ShenandoahOptimizeStableFinals) {
    const TypeAryPtr* ary = type->isa_aryptr();
    if (ary && ary->is_stable() && allow_fromspace) {
      return false;
    }
  }

  if (n->is_CheckCastPP() || n->is_ConstraintCast()) {
    return needs_barrier_impl(phase, orig, n->in(1), rb_mem, allow_fromspace, visited);
  }
  if (n->is_Parm()) {
    return true;
  }
  if (n->is_Proj()) {
    return needs_barrier_impl(phase, orig, n->in(0), rb_mem, allow_fromspace, visited);
  }
  if (n->is_Phi()) {
    bool need_barrier = false;
    for (uint i = 1; i < n->req() && ! need_barrier; i++) {
      Node* input = n->in(i);
      if (input == NULL) {
        need_barrier = true; // Phi not complete yet?
      } else if (needs_barrier_impl(phase, orig, input, rb_mem, allow_fromspace, visited)) {
        need_barrier = true;
      }
    }
    return need_barrier;
  }
  if (n->is_CMove()) {
    return needs_barrier_impl(phase, orig, n->in(CMoveNode::IfFalse), rb_mem, allow_fromspace, visited) ||
           needs_barrier_impl(phase, orig, n->in(CMoveNode::IfTrue ), rb_mem, allow_fromspace, visited);
  }
  if (n->Opcode() == Op_CreateEx) {
    return true;
  }
  if (n->Opcode() == Op_ShenandoahWriteBarrier) {
    // tty->print_cr("skipped barrier for chained write barrier object");
    return false;
  }
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    if (rb_mem == n->in(Memory)) {
      // tty->print_cr("Eliminated chained read barrier");
      return false;
    } else {
      return true;
    }
  }

  if (n->Opcode() == Op_LoadP ||
      n->Opcode() == Op_LoadN ||
      n->Opcode() == Op_GetAndSetP ||
      n->Opcode() == Op_GetAndSetN) {
    return true;
  }
  if (n->Opcode() == Op_DecodeN ||
      n->Opcode() == Op_EncodeP) {
    return needs_barrier_impl(phase, orig, n->in(1), rb_mem, allow_fromspace, visited);
  }

#ifdef ASSERT
  tty->print("need barrier on?: "); n->dump();
  ShouldNotReachHere();
#endif
  return true;
}

/**
 * In Shenandoah, we need barriers on acmp (and similar instructions that compare two
 * oops) to avoid false negatives. If it compares a from-space and a to-space
 * copy of an object, a regular acmp would return false, even though both are
 * the same. The acmp barrier compares the two objects, and when they are
 * *not equal* it does a read-barrier on both, and compares them again. When it
 * failed because of different copies of the object, we know that the object
 * must already have been evacuated (and therefore doesn't require a write-barrier).
 */
void ShenandoahBarrierNode::do_cmpp_if(GraphKit& kit, Node*& taken_branch, Node*& untaken_branch, Node*& taken_memory, Node*& untaken_memory) {
  assert(taken_memory == NULL && untaken_memory == NULL, "unexpected memory inputs");
  if (!UseShenandoahGC || !ShenandoahAcmpBarrier || ShenandoahVerifyOptoBarriers) {
    return;
  }
  if (taken_branch->is_top() || untaken_branch->is_top()) {
    // one of the branches is known to be untaken
    return;
  }
  assert(taken_branch->is_IfProj() && untaken_branch->is_IfProj(), "if projections only");
  assert(taken_branch->in(0) == untaken_branch->in(0), "should come from same if");
  IfNode* iff = taken_branch->in(0)->as_If();
  BoolNode* bol = iff->in(1)->as_Bool();
  Node* cmp = bol->in(1);
  if (cmp->Opcode() != Op_CmpP) {
    return;
  }
  Node* a = cmp->in(1);
  Node* b = cmp->in(2);
  const Type* a_type = kit.gvn().type(a);
  const Type* b_type = kit.gvn().type(b);
  if (a_type->higher_equal(TypePtr::NULL_PTR) || b_type->higher_equal(TypePtr::NULL_PTR)) {
    // We know one arg is gonna be null. No need for barriers.
    return;
  }

  const TypePtr* a_adr_type = ShenandoahBarrierNode::brooks_pointer_type(a_type);
  const TypePtr* b_adr_type = ShenandoahBarrierNode::brooks_pointer_type(b_type);
  if ((! ShenandoahBarrierNode::needs_barrier(&kit.gvn(), NULL, a, kit.memory(a_adr_type), false)) &&
      (! ShenandoahBarrierNode::needs_barrier(&kit.gvn(), NULL, b, kit.memory(b_adr_type), false))) {
    // We know both args are in to-space already. No acmp barrier needed.
    return;
  }

  Node* equal_path = iff->proj_out(true);
  Node* not_equal_path = iff->proj_out(false);

  if (bol->_test._test == BoolTest::ne) {
    Node* tmp = equal_path;
    equal_path = not_equal_path;
    not_equal_path = tmp;
  }

  Node* init_equal_path = equal_path;
  Node* init_not_equal_path = not_equal_path;

  uint alias_a = kit.C->get_alias_index(a_adr_type);
  uint alias_b = kit.C->get_alias_index(b_adr_type);

  Node* equal_memory = NULL;
  Node* not_equal_memory = NULL;

  RegionNode* region = new (kit.C) RegionNode(3);
  region->init_req(1, equal_path);
  PhiNode* mem_phi = NULL;
  if (alias_a == alias_b) {
    mem_phi = PhiNode::make(region, kit.memory(alias_a), Type::MEMORY, kit.C->get_adr_type(alias_a));
  } else {
    Node* mem = kit.reset_memory();
    mem_phi = PhiNode::make(region, mem, Type::MEMORY, TypePtr::BOTTOM);
    kit.set_all_memory(mem);
  }

  kit.set_control(not_equal_path);

  Node* mb = NULL;
  if (alias_a == alias_b) {
    Node* mem = kit.reset_memory();
    mb = MemBarNode::make(kit.C, Op_MemBarAcquire, alias_a);
    mb->init_req(TypeFunc::Control, kit.control());
    mb->init_req(TypeFunc::Memory, mem);
    Node* membar = kit.gvn().transform(mb);
    kit.set_control(kit.gvn().transform(new (kit.C) ProjNode(membar, TypeFunc::Control)));
    Node* newmem = kit.gvn().transform(new (kit.C) ProjNode(membar, TypeFunc::Memory));
    kit.set_all_memory(mem);
    kit.set_memory(newmem, alias_a);
  } else {
    mb = kit.insert_mem_bar(Op_MemBarAcquire);
  }

  a = kit.shenandoah_read_barrier_acmp(a);
  b = kit.shenandoah_read_barrier_acmp(b);

  Node* cmp2 = kit.gvn().transform(new (kit.C) CmpPNode(a, b));
  Node* bol2 = bol->clone();
  bol2->set_req(1, cmp2);
  bol2 = kit.gvn().transform(bol2);
  Node* iff2 = iff->clone();
  iff2->set_req(0, kit.control());
  iff2->set_req(1, bol2);
  kit.gvn().set_type(iff2, kit.gvn().type(iff));
  Node* equal_path2 = equal_path->clone();
  equal_path2->set_req(0, iff2);
  equal_path2 = kit.gvn().transform(equal_path2);
  Node* not_equal_path2 = not_equal_path->clone();
  not_equal_path2->set_req(0, iff2);
  not_equal_path2 = kit.gvn().transform(not_equal_path2);

  region->init_req(2, equal_path2);
  not_equal_memory = kit.reset_memory();
  not_equal_path = not_equal_path2;

  kit.set_all_memory(not_equal_memory);

  if (alias_a == alias_b) {
    mem_phi->init_req(2, kit.memory(alias_a));
    kit.set_memory(mem_phi, alias_a);
  } else {
    mem_phi->init_req(2, kit.reset_memory());
  }

  kit.record_for_igvn(mem_phi);
  kit.gvn().set_type(mem_phi, Type::MEMORY);

  if (alias_a == alias_b) {
    equal_memory = kit.reset_memory();
  } else {
    equal_memory = mem_phi;
  }

  assert(kit.map()->memory() == NULL, "no live memory state");
  equal_path = kit.gvn().transform(region);

  if (taken_branch == init_equal_path) {
    assert(untaken_branch == init_not_equal_path, "inconsistent");
    taken_branch = equal_path;
    untaken_branch = not_equal_path;
    taken_memory = equal_memory;
    untaken_memory = not_equal_memory;
  } else {
    assert(taken_branch == init_not_equal_path, "inconsistent");
    assert(untaken_branch == init_equal_path, "inconsistent");
    taken_branch = not_equal_path;
    untaken_branch = equal_path;
    taken_memory = not_equal_memory;
    untaken_memory = equal_memory;
  }
}

bool ShenandoahReadBarrierNode::dominates_memory_rb_impl(PhaseTransform* phase,
                                                         Node* b1,
                                                         Node* b2,
                                                         Node* current,
                                                         bool linear) {
  ResourceMark rm;
  VectorSet visited(Thread::current()->resource_area());
  Node_Stack phis(0);

  for(int i = 0; i < 10; i++) {
    if (current == NULL) {
      return false;
    } else if (visited.test_set(current->_idx) || current->is_top() || current == b1) {
      current = NULL;
      while (phis.is_nonempty() && current == NULL) {
        uint idx = phis.index();
        Node* phi = phis.node();
        if (idx >= phi->req()) {
          phis.pop();
        } else {
          current = phi->in(idx);
          phis.set_index(idx+1);
        }
      }
      if (current == NULL) {
        return true;
      }
    } else if (current == phase->C->immutable_memory()) {
      return false;
    } else if (current->isa_Phi()) {
      if (!linear) {
        return false;
      }
      phis.push(current, 2);
      current = current->in(1);
    } else if (current->Opcode() == Op_ShenandoahWriteBarrier) {
      const Type* in_type = current->bottom_type();
      const Type* this_type = b2->bottom_type();
      if (is_independent(in_type, this_type)) {
        current = current->in(Memory);
      } else {
        return false;
      }
    } else if (current->Opcode() == Op_ShenandoahWBMemProj) {
      current = current->in(0);
    } else if (current->is_Proj()) {
      current = current->in(0);
    } else if (current->is_Call()) {
      return false; // TODO: Maybe improve by looking at the call's memory effects?
    } else if (current->is_MemBar()) {
      return false; // TODO: Do we need to stop at *any* membar?
    } else if (current->is_MergeMem()) {
      // if (true) return false;
      // tty->print_cr("current == mergemem: "); current->dump();
      const TypePtr* adr_type = brooks_pointer_type(phase->type(b2));
      uint alias_idx = phase->C->get_alias_index(adr_type);
      current = current->as_MergeMem()->memory_at(alias_idx);
    } else {
      // tty->print_cr("what else can we see here:");
#ifdef ASSERT
      current->dump();
#endif
      ShouldNotReachHere();
      return false;
    }
  }
  return false;
}

bool ShenandoahReadBarrierNode::is_independent(Node* mem) {
  if (mem->is_Phi() || mem->is_Proj() || mem->is_MergeMem()) {
    return true;
  } else if (mem->Opcode() == Op_ShenandoahWriteBarrier) {
    const Type* mem_type = mem->bottom_type();
    const Type* this_type = bottom_type();
    if (is_independent(mem_type, this_type)) {
      return true;
    } else {
      return false;
    }
  } else if (mem->is_Call() || mem->is_MemBar()) {
    return false;
  }
#ifdef ASSERT
  mem->dump();
#endif
  ShouldNotReachHere();
  return true;
}


bool ShenandoahReadBarrierNode::dominates_memory_rb(PhaseTransform* phase, Node* b1, Node* b2, bool linear) {
  return dominates_memory_rb_impl(phase, b1->in(Memory), b2, b2->in(Memory), linear);
}

bool ShenandoahReadBarrierNode::is_independent(const Type* in_type, const Type* this_type) {
  assert(in_type->isa_oopptr(), "expect oop ptr");
  assert(this_type->isa_oopptr(), "expect oop ptr");
  /*
  if ((! in_type->isa_oopptr()) || (! this_type->isa_oopptr())) {
#ifdef ASSERT
    tty->print_cr("not oopptr");
    tty->print("in:   "); in_type->dump(); tty->print_cr(" ");
    tty->print("this: "); this_type->dump(); tty->print_cr(" ");
#endif
    return false;
  }
  */

  ciKlass* in_kls = in_type->is_oopptr()->klass();
  ciKlass* this_kls = this_type->is_oopptr()->klass();
  if (in_kls != NULL && this_kls != NULL &&
      in_kls->is_loaded() && this_kls->is_loaded() &&
      (!in_kls->is_subclass_of(this_kls)) &&
      (!this_kls->is_subclass_of(in_kls))) {
#ifdef ASSERT
    // tty->print_cr("independent: ");
    // tty->print("in:   "); in_kls->print(); tty->print_cr(" ");
    // tty->print("this: "); this_kls->print(); tty->print_cr(" ");
#endif
    return true;
  }
#ifdef ASSERT
  // tty->print_cr("possibly dependend?");
  // tty->print("in:   "); in_type->dump(); tty->print_cr(" ");
  // tty->print("this: "); this_type->dump(); tty->print_cr(" ");
#endif
  return false;
}

Node* ShenandoahReadBarrierNode::Ideal(PhaseGVN *phase, bool can_reshape) {

  if (! can_reshape) {
    return NULL;
  }

  if (in(Memory) == phase->C->immutable_memory()) return NULL;

  // If memory input is a MergeMem, take the appropriate slice out of it.
  Node* mem_in = in(Memory);
  if (mem_in->isa_MergeMem()) {
    const TypePtr* adr_type = brooks_pointer_type(bottom_type());
    uint alias_idx = phase->C->get_alias_index(adr_type);
    mem_in = mem_in->as_MergeMem()->memory_at(alias_idx);
    set_req(Memory, mem_in);
    return this;
  }

  Node* input = in(Memory);
  if (input->Opcode() == Op_ShenandoahWBMemProj) {
    Node* wb = input->in(0);
    const Type* in_type = phase->type(wb);
    // is_top() test not sufficient here: we can come here after CCP
    // in a dead branch of the graph that has not yet been removed.
    if (in_type == Type::TOP) return NULL; // Dead path.
    assert(wb->Opcode() == Op_ShenandoahWriteBarrier, "expect write barrier");
    if (is_independent(in_type, _type)) {
      if (phase->is_IterGVN()) {
        phase->is_IterGVN()->rehash_node_delayed(wb);
      }
      set_req(Memory, wb->in(Memory));
      if (can_reshape && input->outcnt() == 0) {
        phase->is_IterGVN()->_worklist.push(input);
      }
      return this;
    }
  }
  return NULL;
}

Node* ShenandoahWriteBarrierNode::Identity(PhaseTransform* phase) {
  assert(in(0) != NULL, "should have control");
  PhaseIterGVN* igvn = phase->is_IterGVN();
  Node* mem_in = in(Memory);
  Node* mem_proj = NULL;

  if (igvn != NULL) {
    mem_proj = find_out_with(Op_ShenandoahWBMemProj);
    if (mem_proj == NULL || mem_in == mem_proj) {
      return this;
    }
  }

  Node* replacement = Identity_impl(phase);
  if (igvn != NULL) {
    if (replacement != NULL && replacement != this) {
      igvn->replace_node(mem_proj, mem_in);
    }
  }
  return replacement;
}


Node* ShenandoahWriteBarrierNode::Ideal(PhaseGVN *phase, bool can_reshape) {
  assert(in(0) != NULL, "should have control");
  if (!can_reshape) {
    return NULL;
  }

  PhaseIterGVN* igvn = phase->is_IterGVN();
  Node* mem_proj = find_out_with(Op_ShenandoahWBMemProj);
  Node* mem_in = in(Memory);

  if (mem_in == phase->C->immutable_memory()) return NULL;

  if (mem_in->isa_MergeMem()) {
    const TypePtr* adr_type = brooks_pointer_type(bottom_type());
    uint alias_idx = phase->C->get_alias_index(adr_type);
    mem_in = mem_in->as_MergeMem()->memory_at(alias_idx);
    set_req(Memory, mem_in);
    return this;
  }

  return NULL;
}

bool ShenandoahBarrierNode::dominates_memory_impl(PhaseTransform* phase,
                                                  Node* b1,
                                                  Node* b2,
                                                  Node* current,
                                                  bool linear) {
  ResourceMark rm;
  VectorSet visited(Thread::current()->resource_area());
  Node_Stack phis(0);


  for(int i = 0; i < 10; i++) {
    if (current == NULL) {
      return false;
    } else if (visited.test_set(current->_idx) || current->is_top() || current == b1) {
      current = NULL;
      while (phis.is_nonempty() && current == NULL) {
        uint idx = phis.index();
        Node* phi = phis.node();
        if (idx >= phi->req()) {
          phis.pop();
        } else {
          current = phi->in(idx);
          phis.set_index(idx+1);
        }
      }
      if (current == NULL) {
        return true;
      }
    } else if (current == b2) {
      return false;
    } else if (current == phase->C->immutable_memory()) {
      return false;
    } else if (current->isa_Phi()) {
      if (!linear) {
        return false;
      }
      phis.push(current, 2);
      current = current->in(1);
    } else if (current->Opcode() == Op_ShenandoahWriteBarrier) {
      current = current->in(Memory);
    } else if (current->Opcode() == Op_ShenandoahWBMemProj) {
      current = current->in(0);
    } else if (current->is_Proj()) {
      current = current->in(0);
    } else if (current->is_Call()) {
      current = current->in(TypeFunc::Memory);
    } else if (current->is_MemBar()) {
      current = current->in(TypeFunc::Memory);
    } else if (current->is_MergeMem()) {
      const TypePtr* adr_type = brooks_pointer_type(phase->type(b2));
      uint alias_idx = phase->C->get_alias_index(adr_type);
      current = current->as_MergeMem()->memory_at(alias_idx);
    } else {
#ifdef ASSERT
      current->dump();
#endif
      ShouldNotReachHere();
      return false;
    }
  }
  return false;
}

/**
 * Determines if b1 dominates b2 through memory inputs. It returns true if:
 * - b1 can be reached by following each branch in b2's memory input (through phis, etc)
 * - or we get back to b2 (i.e. through a loop) without seeing b1
 * In all other cases, (in particular, if we reach immutable_memory without having seen b1)
 * we return false.
 */
bool ShenandoahBarrierNode::dominates_memory(PhaseTransform* phase, Node* b1, Node* b2, bool linear) {
  return dominates_memory_impl(phase, b1, b2, b2->in(Memory), linear);
}

Node* ShenandoahBarrierNode::Identity_impl(PhaseTransform* phase) {
  Node* n = in(ValueIn);

  Node* rb_mem = Opcode() == Op_ShenandoahReadBarrier ? in(Memory) : NULL;
  if (! needs_barrier(phase, this, n, rb_mem, _allow_fromspace)) {
    return n;
  }

  // tty->print_cr("find sibling for: "); dump(2);
  // Try to find a write barrier sibling with identical inputs that we can fold into.
  for (DUIterator i = n->outs(); n->has_out(i); i++) {
    Node* sibling = n->out(i);
    if (sibling == this) {
      continue;
    }
    /*
    assert(sibling->Opcode() != Op_ShenandoahWriteBarrier ||
           Opcode() != Op_ShenandoahWriteBarrier || hash() == sibling->hash(),
           "if this is a write barrier, then sibling can't be write barrier too");
    */
    if (sibling->Opcode() != Op_ShenandoahWriteBarrier) {
      continue;
    }
    /*
    if (sibling->outcnt() == 0) {
      // Some dead node.
      continue;
    }
    */
    assert(sibling->in(ValueIn) == in(ValueIn), "sanity");
    assert(sibling->Opcode() == Op_ShenandoahWriteBarrier, "sanity");
    // tty->print_cr("candidate: "); sibling->dump();

    if (dominates_memory(phase, sibling, this, phase->is_IterGVN() == NULL)) {

      /*
      tty->print_cr("matched barrier:");
      sibling->dump();
      tty->print_cr("for: ");
      dump();
      */
      return sibling;
    }

    /*
    tty->print_cr("couldn't match candidate:");
    sibling->dump(2);
    */
  }
  /*
  tty->print_cr("couldn't match barrier to any:");
  dump();
  */
  return this;
}

#ifndef PRODUCT
void ShenandoahBarrierNode::dump_spec(outputStream *st) const {
  const TypePtr* adr = adr_type();
  if (adr == NULL) {
    return;
  }
  st->print(" @");
  adr->dump_on(st);
  st->print(" (");
  Compile::current()->alias_type(adr)->adr_type()->dump_on(st);
  st->print(") ");
}
#endif

Node* ShenandoahReadBarrierNode::Identity(PhaseTransform* phase) {

  // if (true) return this;

  // tty->print("optimizing rb: "); dump();
  Node* id = Identity_impl(phase);

  if (id == this && phase->is_IterGVN()) {
    Node* n = in(ValueIn);
    // No success in super call. Try to combine identical read barriers.
    for (DUIterator i = n->outs(); n->has_out(i); i++) {
      Node* sibling = n->out(i);
      if (sibling == this || sibling->Opcode() != Op_ShenandoahReadBarrier) {
        continue;
      }
      assert(sibling->in(ValueIn)  == in(ValueIn), "sanity");
      if (phase->is_IterGVN()->hash_find(sibling) &&
          sibling->bottom_type() == bottom_type() &&
          sibling->in(Control) == in(Control) &&
          dominates_memory_rb(phase, sibling, this, phase->is_IterGVN() == NULL)) {
        /*
        if (in(Memory) != sibling->in(Memory)) {
          tty->print_cr("interesting rb-fold");
          dump();
          sibling->dump();
        }
        */
        return sibling;
      }
    }
  }
  return id;
}

const Type* ShenandoahBarrierNode::Value(PhaseTransform* phase) const {
  // Either input is TOP ==> the result is TOP
  const Type *t1 = phase->type(in(Memory));
  if (t1 == Type::TOP) return Type::TOP;
  const Type *t2 = phase->type(in(ValueIn));
  if( t2 == Type::TOP ) return Type::TOP;

  Node* input = in(ValueIn);
  const Type* type = phase->type(input)->is_oopptr()->cast_to_nonconst();
  return type->filter_speculative(_type);
}

uint ShenandoahBarrierNode::hash() const {
  return TypeNode::hash() + _allow_fromspace;
}

uint ShenandoahBarrierNode::cmp(const Node& n) const {
  return _allow_fromspace == ((ShenandoahBarrierNode&) n)._allow_fromspace
    && TypeNode::cmp(n);
}

uint ShenandoahBarrierNode::size_of() const {
  return sizeof(*this);
}

Node* ShenandoahWBMemProjNode::Identity(PhaseTransform* phase) {

  Node* wb = in(0);
  if (wb->is_top()) return phase->C->top(); // Dead path.

  assert(wb->Opcode() == Op_ShenandoahWriteBarrier, "expect write barrier");
  PhaseIterGVN* igvn = phase->is_IterGVN();
  // We can't do the below unless the graph is fully constructed.
  if (igvn == NULL) {
    return this;
  }

  // If the mem projection has no barrier users, it's not needed anymore.
  Unique_Node_List visited;
  if (wb->outcnt() == 1) {
    return wb->in(ShenandoahBarrierNode::Memory);
  }

  return this;
}

#ifdef ASSERT
bool ShenandoahBarrierNode::verify_helper(Node* in, Node_Stack& phis, VectorSet& visited, verify_type t, bool trace, Unique_Node_List& barriers_used) {
  assert(phis.size() == 0, "");

  while (true) {
    if (!in->bottom_type()->make_ptr()->isa_oopptr()) {
      if (trace) {tty->print_cr("Non oop");}
    } else if (t == ShenandoahLoad && ShenandoahOptimizeStableFinals &&
               in->bottom_type()->make_ptr()->isa_aryptr() &&
               in->bottom_type()->make_ptr()->is_aryptr()->is_stable()) {
      if (trace) {tty->print_cr("Stable array load");}
    } else {
      if (in->Opcode() == Op_CastPP || in->Opcode() == Op_CheckCastPP) {
        in = in->in(1);
        continue;
      } else if (in->is_AddP()) {
        assert(!in->in(AddPNode::Address)->is_top(), "no raw memory access");
        in = in->in(AddPNode::Address);
        continue;
      } else if (in->is_Con()) {
        if (trace) {tty->print("Found constant"); in->dump();}
      } else if (in->is_ShenandoahBarrier()) {
        if (t == ShenandoahStore && in->Opcode() != Op_ShenandoahWriteBarrier) {
          return false;
        }
        barriers_used.push(in);
        if (trace) {tty->print("Found barrier"); in->dump();}
      } else if (in->is_Proj() && in->in(0)->is_Allocate()) {
        if (trace) {tty->print("Found alloc"); in->in(0)->dump();}
      } else if (in->is_Phi()) {
        if (!visited.test_set(in->_idx)) {
          if (trace) {tty->print("Pushed phi:"); in->dump();}
          phis.push(in, 2);
          in = in->in(1);
          continue;
        }
        if (trace) {tty->print("Already seen phi:"); in->dump();}
      } else if (in->Opcode() == Op_CMoveP || in->Opcode() == Op_CMoveN) {
        if (!visited.test_set(in->_idx)) {
          if (trace) {tty->print("Pushed cmovep:"); in->dump();}
          phis.push(in, CMoveNode::IfTrue);
          in = in->in(CMoveNode::IfFalse);
          continue;
        }
        if (trace) {tty->print("Already seen cmovep:"); in->dump();}
      } else if (in->Opcode() == Op_EncodeP || in->Opcode() == Op_DecodeN) {
        in = in->in(1);
        continue;
      } else {
        return false;
      }
    }
    bool cont = false;
    while (phis.is_nonempty()) {
      uint idx = phis.index();
      Node* phi = phis.node();
      if (idx >= phi->req()) {
        if (trace) {tty->print("Popped phi:"); phi->dump();}
        phis.pop();
        continue;
      }
      if (trace) {tty->print("Next entry(%d) for phi:", idx); phi->dump();}
      in = phi->in(idx);
      phis.set_index(idx+1);
      cont = true;
      break;
    }
    if (!cont) {
      break;
    }
  }
  return true;
}

void ShenandoahBarrierNode::report_verify_failure(const char *msg, Node *n1, Node *n2) {
  if (n1 != NULL) {
    n1->dump(+10);
  }
  if (n2 != NULL) {
    n2->dump(+10);
  }
  fatal(msg);
}

void ShenandoahBarrierNode::verify(RootNode* root) {
  ResourceMark rm;
  Unique_Node_List wq;
  GrowableArray<Node*> barriers;
  Unique_Node_List barriers_used;
  Node_Stack phis(0);
  VectorSet visited(Thread::current()->resource_area());
  const bool trace = false;
  const bool verify_no_useless_barrier = false;

  wq.push(root);
  for (uint next = 0; next < wq.size(); next++) {
    Node *n = wq.at(next);
    if (n->is_Load()) {
      const bool trace = false;
      if (trace) {tty->print("Verifying"); n->dump();}
      if (n->Opcode() == Op_LoadRange || n->Opcode() == Op_LoadKlass || n->Opcode() == Op_LoadNKlass) {
        if (trace) {tty->print_cr("Load range/klass");}
      } else {
        const TypePtr* adr_type = n->as_Load()->adr_type();

        if (adr_type->isa_oopptr() && adr_type->is_oopptr()->offset() == oopDesc::mark_offset_in_bytes()) {
          if (trace) {tty->print_cr("Mark load");}
        } else if (adr_type->isa_instptr() &&
                   adr_type->is_instptr()->klass()->is_subtype_of(Compile::current()->env()->Reference_klass()) &&
                   adr_type->is_instptr()->offset() == java_lang_ref_Reference::referent_offset) {
          if (trace) {tty->print_cr("Reference.get()");}
        } else {
          bool verify = true;
          if (adr_type->isa_instptr()) {
            ciKlass* k = adr_type->is_instptr()->klass();
            assert(k->is_instance_klass(), "");
            ciInstanceKlass* ik = (ciInstanceKlass*)k;
            int offset = adr_type->offset();

            if ((ik->debug_final_field_at(offset) && ShenandoahOptimizeInstanceFinals) ||
                (ik->debug_stable_field_at(offset) && ShenandoahOptimizeStableFinals)) {
              if (trace) {tty->print_cr("Final/stable");}
              verify = false;
            }
          }

          if (verify && !ShenandoahBarrierNode::verify_helper(n->in(MemNode::Address), phis, visited, ShenandoahLoad, trace, barriers_used)) {
            report_verify_failure("Shenandoah verification: Load should have barriers", n);
          }
        }
      }
    } else if (n->is_Store()) {
      const bool trace = false;

      if (trace) {tty->print("Verifying"); n->dump();}
      if (n->in(MemNode::ValueIn)->bottom_type()->isa_oopptr()) {
        Node* adr = n->in(MemNode::Address);
        bool verify = true;

        if (adr->is_AddP() && adr->in(AddPNode::Base)->is_top()) {
          adr = adr->in(AddPNode::Address);
          if (adr->is_AddP()) {
            assert(adr->in(AddPNode::Base)->is_top(), "");
            adr = adr->in(AddPNode::Address);
            if (adr->Opcode() == Op_LoadP &&
                adr->in(MemNode::Address)->in(AddPNode::Base)->is_top() &&
                adr->in(MemNode::Address)->in(AddPNode::Address)->Opcode() == Op_ThreadLocal &&
                adr->in(MemNode::Address)->in(AddPNode::Offset)->find_intptr_t_con(-1) == in_bytes(JavaThread::satb_mark_queue_offset() +
                                                                                              PtrQueue::byte_offset_of_buf())) {
              if (trace) {tty->print_cr("G1 prebarrier");}
              verify = false;
            }
          }
        }

        if (verify && !ShenandoahBarrierNode::verify_helper(n->in(MemNode::ValueIn), phis, visited, ShenandoahValue, trace, barriers_used)) {
          report_verify_failure("Shenandoah verification: Store should have barriers", n);
        }
      }
      if (!ShenandoahBarrierNode::verify_helper(n->in(MemNode::Address), phis, visited, ShenandoahStore, trace, barriers_used)) {
        report_verify_failure("Shenandoah verification: Store (address) should have barriers", n);
      }
    } else if (n->is_ClearArray()) {
      if (!ShenandoahBarrierNode::verify_helper(n->in(3), phis, visited, ShenandoahStore, trace, barriers_used)) {
        report_verify_failure("Shenandoah verification: ClearArray should have barriers", n);
      }
    } else if (n->Opcode() == Op_CmpP) {
      const bool trace = false;

      Node* in1 = n->in(1);
      Node* in2 = n->in(2);
      if (in1->bottom_type()->isa_oopptr()) {
        if (trace) {tty->print("Verifying"); n->dump();}

        bool mark_inputs = false;
        if (in1->is_Con() || in2->is_Con()) {
          if (trace) {tty->print_cr("Comparison against a constant");}
          mark_inputs = true;
        } else if ((in1->is_CheckCastPP() && in1->in(1)->is_Proj() && in1->in(1)->in(0)->is_Allocate()) ||
                   (in2->is_CheckCastPP() && in2->in(1)->is_Proj() && in2->in(1)->in(0)->is_Allocate())) {
          if (trace) {tty->print_cr("Comparison with newly alloc'ed object");}
          mark_inputs = true;
        } else {
          assert(in2->bottom_type()->isa_oopptr(), "");

          if (!ShenandoahBarrierNode::verify_helper(in1, phis, visited, ShenandoahStore, trace, barriers_used) ||
              !ShenandoahBarrierNode::verify_helper(in2, phis, visited, ShenandoahStore, trace, barriers_used)) {
            report_verify_failure("Shenandoah verification: Cmp should have barriers", n);
          }
        }
        if (verify_no_useless_barrier &&
            mark_inputs &&
            (!ShenandoahBarrierNode::verify_helper(in1, phis, visited, ShenandoahValue, trace, barriers_used) ||
             !ShenandoahBarrierNode::verify_helper(in2, phis, visited, ShenandoahValue, trace, barriers_used))) {
          phis.clear();
          visited.Reset();
        }
      }
    } else if (n->is_LoadStore()) {
      if (n->in(MemNode::ValueIn)->bottom_type()->isa_ptr() &&
          !ShenandoahBarrierNode::verify_helper(n->in(MemNode::ValueIn), phis, visited, ShenandoahLoad, trace, barriers_used)) {
        report_verify_failure("Shenandoah verification: LoadStore (value) should have barriers", n);
      }

      if (n->in(MemNode::Address)->bottom_type()->isa_oopptr() && !ShenandoahBarrierNode::verify_helper(n->in(MemNode::Address), phis, visited, ShenandoahStore, trace, barriers_used)) {
        report_verify_failure("Shenandoah verification: LoadStore (address) should have barriers", n);
      }
    } else if (n->Opcode() == Op_CallLeafNoFP || n->Opcode() == Op_CallLeaf) {
      CallRuntimeNode* call = n->as_CallRuntime();

      static struct {
        const char* name;
        struct {
          int pos;
          verify_type t;
        } args[6];
      } calls[] = {
        "aescrypt_encryptBlock",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "aescrypt_decryptBlock",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "multiplyToLen",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+2, ShenandoahLoad },   { TypeFunc::Parms+4, ShenandoahStore },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "squareToLen",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+2, ShenandoahLoad },   { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "montgomery_multiply",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahLoad },   { TypeFunc::Parms+2, ShenandoahLoad },
          { TypeFunc::Parms+6, ShenandoahStore }, { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "montgomery_square",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahLoad },   { TypeFunc::Parms+5, ShenandoahStore },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "mulAdd",
        { { TypeFunc::Parms, ShenandoahStore },  { TypeFunc::Parms+1, ShenandoahLoad },   { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "vectorizedMismatch",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahLoad },   { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "updateBytesCRC32",
        { { TypeFunc::Parms+1, ShenandoahLoad }, { -1,  ShenandoahNone},                  { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "updateBytesAdler32",
        { { TypeFunc::Parms+1, ShenandoahLoad }, { -1,  ShenandoahNone},                  { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "updateBytesCRC32C",
        { { TypeFunc::Parms+1, ShenandoahLoad }, { TypeFunc::Parms+3, ShenandoahLoad},    { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "counterMode_AESCrypt",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { TypeFunc::Parms+3, ShenandoahStore }, { TypeFunc::Parms+5, ShenandoahStore }, { TypeFunc::Parms+6, ShenandoahStore } },
        "cipherBlockChaining_encryptAESCrypt",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { TypeFunc::Parms+3, ShenandoahLoad },  { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "cipherBlockChaining_decryptAESCrypt",
        { { TypeFunc::Parms, ShenandoahLoad },   { TypeFunc::Parms+1, ShenandoahStore },  { TypeFunc::Parms+2, ShenandoahLoad },
          { TypeFunc::Parms+3, ShenandoahLoad },  { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "shenandoah_clone_barrier",
        { { TypeFunc::Parms, ShenandoahLoad },   { -1,  ShenandoahNone},                  { -1,  ShenandoahNone},
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "ghash_processBlocks",
        { { TypeFunc::Parms, ShenandoahStore },  { TypeFunc::Parms+1, ShenandoahLoad },   { TypeFunc::Parms+2, ShenandoahLoad },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha1_implCompress",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha256_implCompress",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha512_implCompress",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha1_implCompressMB",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha256_implCompressMB",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
        "sha512_implCompressMB",
        { { TypeFunc::Parms, ShenandoahLoad },  { TypeFunc::Parms+1, ShenandoahStore },   { -1, ShenandoahNone },
          { -1,  ShenandoahNone},                 { -1,  ShenandoahNone},                 { -1,  ShenandoahNone} },
      };

      if (call->is_call_to_arraycopystub()) {
        Node* dest = NULL;
        const TypeTuple* args = n->as_Call()->_tf->domain();
        for (uint i = TypeFunc::Parms, j = 0; i < args->cnt(); i++) {
          if (args->field_at(i)->isa_ptr()) {
            j++;
            if (j == 2) {
              dest = n->in(i);
              break;
            }
          }
        }
        if (!ShenandoahBarrierNode::verify_helper(n->in(TypeFunc::Parms), phis, visited, ShenandoahLoad, trace, barriers_used) ||
            !ShenandoahBarrierNode::verify_helper(dest, phis, visited, ShenandoahStore, trace, barriers_used)) {
          report_verify_failure("Shenandoah verification: ArrayCopy should have barriers", n);
        }
      } else if (strlen(call->_name) > 5 &&
                 !strcmp(call->_name + strlen(call->_name) - 5, "_fill")) {
        if (!ShenandoahBarrierNode::verify_helper(n->in(TypeFunc::Parms), phis, visited, ShenandoahStore, trace, barriers_used)) {
          report_verify_failure("Shenandoah verification: _fill should have barriers", n);
        }
      } else if (!strcmp(call->_name, "g1_wb_pre")) {
        // skip
      } else {
        const int calls_len = sizeof(calls) / sizeof(calls[0]);
        int i = 0;
        for (; i < calls_len; i++) {
          if (!strcmp(calls[i].name, call->_name)) {
            break;
          }
        }
        if (i != calls_len) {
          const uint args_len = sizeof(calls[0].args) / sizeof(calls[0].args[0]);
          for (uint j = 0; j < args_len; j++) {
            int pos = calls[i].args[j].pos;
            if (pos == -1) {
              break;
            }
            if (!ShenandoahBarrierNode::verify_helper(call->in(pos), phis, visited, calls[i].args[j].t, trace, barriers_used)) {
              report_verify_failure("Shenandoah verification: intrinsic calls should have barriers", n);
            }
          }
          for (uint j = TypeFunc::Parms; j < call->req(); j++) {
            if (call->in(j)->bottom_type()->make_ptr() &&
                call->in(j)->bottom_type()->make_ptr()->isa_oopptr()) {
              uint k = 0;
              for (; k < args_len && calls[i].args[k].pos != (int)j; k++);
              if (k == args_len) {
                fatal(err_msg("arg %d for call %s not covered", j, call->_name));
              }
            }
          }
        } else {
          for (uint j = TypeFunc::Parms; j < call->req(); j++) {
            if (call->in(j)->bottom_type()->make_ptr() &&
                call->in(j)->bottom_type()->make_ptr()->isa_oopptr()) {
              fatal(err_msg("%s not covered", call->_name));
            }
          }
        }
      }
    } else if (n->is_ShenandoahBarrier()) {
      assert(!barriers.contains(n), "");
      assert(n->Opcode() != Op_ShenandoahWriteBarrier || n->find_out_with(Op_ShenandoahWBMemProj) != NULL, "bad shenandoah write barrier");
      assert(n->Opcode() != Op_ShenandoahWriteBarrier || n->outcnt() > 1, "bad shenandoah write barrier");
      barriers.push(n);
    } else if (n->is_AddP()
               || n->is_Phi()
               || n->Opcode() == Op_CastPP
               || n->Opcode() == Op_CheckCastPP
               || n->Opcode() == Op_Return
               || n->Opcode() == Op_CMoveP
               || n->Opcode() == Op_CMoveN
               || n->Opcode() == Op_Rethrow
               || n->is_MemBar()
               || n->Opcode() == Op_Conv2B
               || n->Opcode() == Op_SafePoint
               || n->is_CallJava()
               || n->Opcode() == Op_Unlock
               || n->Opcode() == Op_EncodeP
               || n->Opcode() == Op_DecodeN) {
      // nothing to do
    } else {
      static struct {
        int opcode;
        struct {
          int pos;
          verify_type t;
        } inputs[2];
      } others[] = {
        Op_FastLock,
        { { 1, ShenandoahLoad },                  { -1, ShenandoahNone} },
        Op_Lock,
        { { TypeFunc::Parms, ShenandoahLoad },    { -1, ShenandoahNone} },
        Op_AryEq,
        { { 2, ShenandoahLoad },                  { 3, ShenandoahLoad } },
        Op_StrIndexOf,
        { { 2, ShenandoahLoad },                  { 4, ShenandoahLoad } },
        Op_StrComp,
        { { 2, ShenandoahLoad },                  { 4, ShenandoahLoad } },
        Op_StrEquals,
        { { 2, ShenandoahLoad },                  { 3, ShenandoahLoad } },
        Op_EncodeISOArray,
        { { 2, ShenandoahLoad },                  { 3, ShenandoahStore } },
        Op_CastP2X,
        { { 1, ShenandoahLoad },                  { -1, ShenandoahNone} },
      };

      const int others_len = sizeof(others) / sizeof(others[0]);
      int i = 0;
      for (; i < others_len; i++) {
        if (others[i].opcode == n->Opcode()) {
          break;
        }
      }
      uint stop = n->is_Call() ? n->as_Call()->tf()->domain()->cnt() : n->req();
      if (i != others_len) {
        const uint inputs_len = sizeof(others[0].inputs) / sizeof(others[0].inputs[0]);
        for (uint j = 0; j < inputs_len; j++) {
          int pos = others[i].inputs[j].pos;
          if (pos == -1) {
            break;
          }
          if (!ShenandoahBarrierNode::verify_helper(n->in(pos), phis, visited, others[i].inputs[j].t, trace, barriers_used)) {
            report_verify_failure("Shenandoah verification: intrinsic calls should have barriers", n);
          }
        }
        for (uint j = 1; j < stop; j++) {
          if (n->in(j) != NULL && n->in(j)->bottom_type()->make_ptr() &&
              n->in(j)->bottom_type()->make_ptr()->isa_oopptr()) {
            uint k = 0;
            for (; k < inputs_len && others[i].inputs[k].pos != (int)j; k++);
            if (k == inputs_len) {
              fatal(err_msg("arg %d for node %s not covered", j, n->Name()));
            }
          }
        }
      } else {
        for (uint j = 1; j < stop; j++) {
          if (n->in(j) != NULL && n->in(j)->bottom_type()->make_ptr() &&
              n->in(j)->bottom_type()->make_ptr()->isa_oopptr()) {
            fatal(err_msg("%s not covered", n->Name()));
          }
        }
      }
    }

    if (n->is_SafePoint()) {
      SafePointNode* sfpt = n->as_SafePoint();
      if (verify_no_useless_barrier && sfpt->jvms() != NULL) {
        for (uint i = sfpt->jvms()->scloff(); i < sfpt->jvms()->endoff(); i++) {
          if (!ShenandoahBarrierNode::verify_helper(sfpt->in(i), phis, visited, ShenandoahLoad, trace, barriers_used)) {
            phis.clear();
            visited.Reset();
          }
        }
      }
    }
    for( uint i = 0; i < n->len(); ++i ) {
      Node *m = n->in(i);
      if (m == NULL) continue;

      // In most cases, inputs should be known to be non null. If it's
      // not the case, it could be a missing cast_not_null() in an
      // intrinsic or support might be needed in AddPNode::Ideal() to
      // avoid a NULL+offset input.
      if (!(n->is_Phi() ||
            (n->is_SafePoint() && (!n->is_CallRuntime() || !strcmp(n->as_CallRuntime()->_name, "g1_wb_pre") || !strcmp(n->as_CallRuntime()->_name, "unsafe_arraycopy"))) ||
            n->Opcode() == Op_CmpP ||
            n->Opcode() == Op_CmpN ||
            (n->Opcode() == Op_StoreP && i == StoreNode::ValueIn) ||
            (n->Opcode() == Op_StoreN && i == StoreNode::ValueIn) ||
            n->Opcode() == Op_CheckCastPP ||
            n->Opcode() == Op_CastPP ||
            n->Opcode() == Op_Return ||
            n->Opcode() == Op_Conv2B ||
            n->is_AddP() ||
            n->Opcode() == Op_CMoveP ||
            n->Opcode() == Op_CMoveN ||
            n->Opcode() == Op_Rethrow ||
            n->is_MemBar() ||
            n->is_Mem() ||
            n->Opcode() == Op_AryEq ||
            n->Opcode() == Op_SCMemProj ||
            n->Opcode() == Op_EncodeP ||
            n->Opcode() == Op_DecodeN ||
            (n->is_CallRuntime() && !strcmp(n->as_CallRuntime()->_name, "generic_arraycopy")))) {
        if (m->bottom_type()->isa_oopptr() && m->bottom_type()->meet(TypePtr::NULL_PTR) == m->bottom_type()) {
          report_verify_failure("Shenandoah verification: null input", n, m);
        }
      }

      wq.push(m);
    }
  }

  if (verify_no_useless_barrier) {
    for (int i = 0; i < barriers.length(); i++) {
      Node* n = barriers.at(i);
      if (!barriers_used.member(n)) {
        tty->print("XXX useless barrier"); n->dump(-2);
        ShouldNotReachHere();
      }
    }
  }
}
#endif

MergeMemNode* ShenandoahWriteBarrierNode::allocate_merge_mem(Node* mem, int alias, Node* rep_proj, Node* rep_ctrl, PhaseIdealLoop* phase) {
  MergeMemNode* mm = MergeMemNode::make(phase->C, mem);
  mm->set_memory_at(alias, rep_proj);
  phase->register_new_node(mm, rep_ctrl);
  return mm;
}

MergeMemNode* ShenandoahWriteBarrierNode::clone_merge_mem(Node* u, Node* mem, int alias, Node* rep_proj, Node* rep_ctrl, DUIterator& i, PhaseIdealLoop* phase) {
  MergeMemNode* newmm = NULL;
  MergeMemNode* u_mm = u->as_MergeMem();
  Node* c = phase->get_ctrl(u);
  if (phase->is_dominator(c, rep_ctrl)) {
    c = rep_ctrl;
  } else {
    assert(phase->is_dominator(rep_ctrl, c), "one must dominate the other");
  }
  if (u->outcnt() == 1) {
    if (u->req() > (uint)alias && u->in(alias) == mem) {
      phase->igvn().replace_input_of(u, alias, rep_proj);
      --i;
    } else {
      phase->igvn().rehash_node_delayed(u);
      u_mm->set_memory_at(alias, rep_proj);
    }
    newmm = u_mm;
    phase->set_ctrl_and_loop(u, c);
  } else {
    // can't simply clone u and then change one of its input because
    // it adds and then removes an edge which messes with the
    // DUIterator
    newmm = MergeMemNode::make(phase->C, u_mm->base_memory());
    for (uint j = 0; j < u->req(); j++) {
      if (j < newmm->req()) {
        if (j == (uint)alias) {
          newmm->set_req(j, rep_proj);
        } else if (newmm->in(j) != u->in(j)) {
          newmm->set_req(j, u->in(j));
        }
      } else if (j == (uint)alias) {
        newmm->add_req(rep_proj);
      } else {
        newmm->add_req(u->in(j));
      }
    }
    if ((uint)alias >= u->req()) {
      newmm->set_memory_at(alias, rep_proj);
    }
    phase->register_new_node(newmm, c);
  }
  return newmm;
}

bool ShenandoahWriteBarrierNode::should_process_phi(Node* phi, int alias, Compile* C) {
  if (phi->adr_type() == TypePtr::BOTTOM) {
    Node* region = phi->in(0);
    for (DUIterator_Fast jmax, j = region->fast_outs(jmax); j < jmax; j++) {
      Node* uu = region->fast_out(j);
      if (uu->is_Phi() && uu != phi && uu->bottom_type() == Type::MEMORY && C->get_alias_index(uu->adr_type()) == alias) {
        return false;
      }
    }
    return true;
  }
  return C->get_alias_index(phi->adr_type()) == alias;
}

bool ShenandoahBarrierNode::is_dominator_same_ctrl(Node*c, Node* d, Node* n, PhaseIdealLoop* phase) {
  // That both nodes have the same control is not sufficient to prove
  // domination, verify that there's no path from d to n
  ResourceMark rm;
  Unique_Node_List wq;
  wq.push(d);
  for (uint next = 0; next < wq.size(); next++) {
    Node *m = wq.at(next);
    if (m == n) {
      return false;
    }
    if (m->is_Phi() && m->in(0)->is_Loop()) {
      assert(phase->ctrl_or_self(m->in(LoopNode::EntryControl)) != c, "following loop entry should lead to new control");
    } else {
      for (uint i = 0; i < m->req(); i++) {
        if (m->in(i) != NULL && phase->ctrl_or_self(m->in(i)) == c) {
          wq.push(m->in(i));
        }
      }
    }
  }
  return true;
}

bool ShenandoahBarrierNode::is_dominator(Node *d_c, Node *n_c, Node* d, Node* n, PhaseIdealLoop* phase) {
  if (d_c != n_c) {
    return phase->is_dominator(d_c, n_c);
  }
  return is_dominator_same_ctrl(d_c, d, n, phase);
}

Node* next_mem(Node* mem, int alias) {
  Node* res = NULL;
  if (mem->is_Proj()) {
    res = mem->in(0);
  } else if (mem->is_SafePoint() || mem->is_MemBar()) {
    res = mem->in(TypeFunc::Memory);
  } else if (mem->is_Phi()) {
    res = mem->in(1);
  } else if (mem->is_ShenandoahBarrier()) {
    res = mem->in(ShenandoahBarrierNode::Memory);
  } else if (mem->is_MergeMem()) {
    res = mem->as_MergeMem()->memory_at(alias);
  } else if (mem->is_Store() || mem->is_LoadStore() || mem->is_ClearArray()) {
    assert(alias = Compile::AliasIdxRaw, "following raw memory can't lead to a barrier");
    res = mem->in(MemNode::Memory);
  } else {
#ifdef ASSERT
    mem->dump();
#endif
    ShouldNotReachHere();
  }
  return res;
}

bool shenandoah_suitable_mem(Node* mem, Node* old_mem, Node* rep_proj) {
  for (DUIterator_Fast imax, i = mem->fast_outs(imax); i < imax; i++) {
    Node* u = mem->fast_out(i);
    if (u->is_MergeMem()) {
      if (u->has_out_with(Op_MergeMem)) {
        // too complicated for now
        return false;
      }
      if (old_mem == u && rep_proj->has_out_with(Op_MergeMem)) {
        return false;
      }
    }
    if (u->Opcode() == Op_Unlock && mem->is_Proj() && mem->in(0)->Opcode() == Op_MemBarReleaseLock) {
      // would require a merge mem between unlock and the
      // preceding membar. Would confuse logic that eliminates
      // lock/unlock nodes.
      return false;
    }
  }
  return true;
}

void ShenandoahWriteBarrierNode::fix_memory_uses(Node* mem, Node* replacement, Node* rep_proj, Node* rep_ctrl, int alias, PhaseIdealLoop* phase) {
  uint last =phase-> C->unique();
  MergeMemNode* mm = NULL;
  assert(mem->bottom_type() == Type::MEMORY, "");
  for (DUIterator i = mem->outs(); mem->has_out(i); i++) {
    Node* u = mem->out(i);
    if (u != replacement && u->_idx < last) {
      if (u->is_ShenandoahBarrier() && alias != Compile::AliasIdxRaw) {
        if (phase->C->get_alias_index(u->adr_type()) == alias && is_dominator(rep_ctrl, phase->ctrl_or_self(u), replacement, u, phase)) {
          phase->igvn().replace_input_of(u, u->find_edge(mem), rep_proj);
          assert(u->find_edge(mem) == -1, "only one edge");
          --i;
        }
      } else if (u->is_Mem()) {
        if (phase->C->get_alias_index(u->adr_type()) == alias && is_dominator(rep_ctrl, phase->ctrl_or_self(u), replacement, u, phase)) {
          assert(alias == Compile::AliasIdxRaw , "only raw memory can lead to a memory operation");
          phase->igvn().replace_input_of(u, u->find_edge(mem), rep_proj);
          assert(u->find_edge(mem) == -1, "only one edge");
          --i;
        }
      } else if (u->is_MergeMem()) {
        MergeMemNode* u_mm = u->as_MergeMem();
        if (u_mm->memory_at(alias) == mem) {
          MergeMemNode* newmm = NULL;
          for (DUIterator_Fast jmax, j = u->fast_outs(jmax); j < jmax; j++) {
            Node* uu = u->fast_out(j);
            assert(!uu->is_MergeMem(), "chain of MergeMems?");
            if (uu->is_Phi()) {
              if (should_process_phi(uu, alias, phase->C)) {
                Node* region = uu->in(0);
                int nb = 0;
                for (uint k = 1; k < uu->req(); k++) {
                  if (uu->in(k) == u && phase->is_dominator(rep_ctrl, region->in(k))) {
                    if (newmm == NULL) {
                      newmm = clone_merge_mem(u, mem, alias, rep_proj, rep_ctrl, i, phase);
                    }
                    if (newmm != u) {
                      phase->igvn().replace_input_of(uu, k, newmm);
                      nb++;
                      --jmax;
                    }
                  }
                }
                if (nb > 0) {
                  --j;
                }
              }
            } else {
              if (rep_ctrl != uu && is_dominator(rep_ctrl, phase->ctrl_or_self(uu), replacement, uu, phase)) {
                if (newmm == NULL) {
                  newmm = clone_merge_mem(u, mem, alias, rep_proj, rep_ctrl, i, phase);
                }
                if (newmm != u) {
                  phase->igvn().replace_input_of(uu, uu->find_edge(u), newmm);
                  --j, --jmax;
                }
              }
            }
          }
        }
      } else if (u->is_Phi()) {
        assert(u->bottom_type() == Type::MEMORY, "what else?");
        Node* region = u->in(0);
        if (should_process_phi(u, alias, phase->C)) {
          bool replaced = false;
          for (uint j = 1; j < u->req(); j++) {
            if (u->in(j) == mem && phase->is_dominator(rep_ctrl, region->in(j))) {
              Node* nnew = rep_proj;
              if (u->adr_type() == TypePtr::BOTTOM) {
                if (mm == NULL) {
                  mm = allocate_merge_mem(mem, alias, rep_proj, rep_ctrl, phase);
                }
                nnew = mm;
              }
              phase->igvn().replace_input_of(u, j, nnew);
              replaced = true;
            }
          }
          if (replaced) {
            --i;
          }

        }
      } else if (u->adr_type() == TypePtr::BOTTOM || u->adr_type() == NULL) {
        assert(u->adr_type() != NULL ||
               u->Opcode() == Op_Rethrow ||
               u->Opcode() == Op_Return ||
               u->Opcode() == Op_SafePoint ||
               (u->is_CallStaticJava() && u->as_CallStaticJava()->uncommon_trap_request() != 0) ||
               (u->is_CallStaticJava() && u->as_CallStaticJava()->_entry_point == OptoRuntime::rethrow_stub()) ||
               u->Opcode() == Op_CallLeaf, "");
        if (is_dominator(rep_ctrl, phase->ctrl_or_self(u), replacement, u, phase)) {
          if (mm == NULL) {
            mm = allocate_merge_mem(mem, alias, rep_proj, rep_ctrl, phase);
          }
          phase->igvn().replace_input_of(u, u->find_edge(mem), mm);
          --i;
        }
      } else if (phase->C->get_alias_index(u->adr_type()) == alias) {
        if (is_dominator(rep_ctrl, phase->ctrl_or_self(u), replacement, u, phase)) {
          phase->igvn().replace_input_of(u, u->find_edge(mem), rep_proj);
          --i;
        }
      }
    }
  }
}

Node* PhaseIdealLoop::shenandoah_no_branches(Node* c, Node* dom, bool allow_one_proj) {
  Node* iffproj = NULL;
  while (c != dom) {
    Node* next = idom(c);
    assert(next->unique_ctrl_out() == c || c->is_Proj() || c->is_Region(), "multiple control flow out but no proj or region?");
    if (c->is_Region()) {
      ResourceMark rm;
      Unique_Node_List wq;
      wq.push(c);
      for (uint i = 0; i < wq.size(); i++) {
        Node *n = wq.at(i);
        if (n->is_Region()) {
          for (uint j = 1; j < n->req(); j++) {
            if (n->in(j) != next) {
              wq.push(n->in(j));
            }
          }
        } else {
          if (n->in(0) != next) {
            wq.push(n->in(0));
          }
        }
      }
      for (DUIterator_Fast imax, i = next->fast_outs(imax); i < imax; i++) {
        Node* u = next->fast_out(i);
        if (u->is_CFG()) {
          if (!wq.member(u)) {
            return NodeSentinel;
          }
        }
      }

    } else  if (c->is_Proj()) {
      if (c->is_IfProj()) {
        if (c->as_Proj()->is_uncommon_trap_if_pattern(Deoptimization::Reason_none) != NULL) {
          // continue;
        } else {
          if (!allow_one_proj) {
            return NodeSentinel;
          }
          if (iffproj == NULL) {
            iffproj = c;
          } else {
            return NodeSentinel;
          }
        }
      } else if (c->Opcode() == Op_JumpProj) {
        return NodeSentinel; // unsupported
      } else if (c->Opcode() == Op_CatchProj) {
        return NodeSentinel; // unsupported
      } else if (c->Opcode() == Op_CProj && next->Opcode() == Op_NeverBranch) {
        return NodeSentinel; // unsupported
      } else {
        assert(next->unique_ctrl_out() == c, "unsupported branch pattern");
      }
    }
    c = next;
  }
  return iffproj;
}

#ifdef ASSERT
void PhaseIdealLoop::shenandoah_memory_dominates_all_paths_helper(Node* c, Node* rep_ctrl, Unique_Node_List& controls) {
  const bool trace = false;
  if (trace) { tty->print("X control is"); c->dump(); }

  uint start = controls.size();
  controls.push(c);
  for (uint i = start; i < controls.size(); i++) {
    Node *n = controls.at(i);

    if (trace) { tty->print("X from"); n->dump(); }

    if (n == rep_ctrl) {
      continue;
    }

    if (n->is_Proj()) {
      Node* n_dom = n->in(0);
      IdealLoopTree* n_dom_loop = get_loop(n_dom);
      if (n->is_IfProj() && n_dom->outcnt() == 2) {
        n_dom_loop = get_loop(n_dom->as_If()->proj_out(n->as_Proj()->_con == 0 ? 1 : 0));
      }
      if (n_dom_loop != _ltree_root) {
        Node* tail = n_dom_loop->tail();
        if (tail->is_Region()) {
          for (uint j = 1; j < tail->req(); j++) {
            if (is_dominator(n_dom, tail->in(j)) && !is_dominator(n, tail->in(j))) {
              assert(is_dominator(rep_ctrl, tail->in(j)), "why are we here?");
              // entering loop from below, mark backedge
              if (trace) { tty->print("X pushing backedge"); tail->in(j)->dump(); }
              controls.push(tail->in(j));
              //assert(n->in(0) == n_dom, "strange flow control");
            }
          }
        } else if (get_loop(n) != n_dom_loop && is_dominator(n_dom, tail)) {
          // entering loop from below, mark backedge
          if (trace) { tty->print("X pushing backedge"); tail->dump(); }
          controls.push(tail);
          //assert(n->in(0) == n_dom, "strange flow control");
        }
      }
    }

    if (n->is_Loop()) {
      Node* c = n->in(LoopNode::EntryControl);
      if (trace) { tty->print("X pushing"); c->dump(); }
      controls.push(c);
    } else if (n->is_Region()) {
      for (uint i = 1; i < n->req(); i++) {
        Node* c = n->in(i);
        if (trace) { tty->print("X pushing"); c->dump(); }
        controls.push(c);
      }
    } else {
      Node* c = n->in(0);
      if (trace) { tty->print("X pushing"); c->dump(); }
      controls.push(c);
    }
  }
}

bool PhaseIdealLoop::shenandoah_memory_dominates_all_paths(Node* mem, Node* rep_ctrl, int alias) {
  const bool trace = false;
  if (trace) {
    tty->print("XXX mem is"); mem->dump();
    tty->print("XXX rep ctrl is"); rep_ctrl->dump();
    tty->print_cr("XXX alias is %d", alias);
  }
  ResourceMark rm;
  Unique_Node_List wq;
  Unique_Node_List controls;
  wq.push(mem);
  for (uint next = 0; next < wq.size(); next++) {
    Node *nn = wq.at(next);
    if (trace) { tty->print("XX from mem"); nn->dump(); }
    assert(nn->bottom_type() == Type::MEMORY, "memory only");

    if (nn->is_Phi()) {
      Node* r = nn->in(0);
      for (DUIterator_Fast jmax, j = r->fast_outs(jmax); j < jmax; j++) {
        Node* u = r->fast_out(j);
        if (u->is_Phi() && u->bottom_type() == Type::MEMORY && u != nn &&
            (u->adr_type() == TypePtr::BOTTOM || C->get_alias_index(u->adr_type()) == alias)) {
          if (trace) { tty->print("XX Next mem (other phi)"); u->dump(); }
          wq.push(u);
        }
      }
    }

    for (DUIterator_Fast imax, i = nn->fast_outs(imax); i < imax; i++) {
      Node* use = nn->fast_out(i);

      if (trace) { tty->print("XX use %p", use->adr_type()); use->dump(); }
      if (use->is_CFG()) {
        assert(use->in(TypeFunc::Memory) == nn, "bad cfg node");
        Node* c = use->in(0);
        if (is_dominator(rep_ctrl, c)) {
          shenandoah_memory_dominates_all_paths_helper(c, rep_ctrl, controls);
        } else if (use->is_CallStaticJava() && use->as_CallStaticJava()->uncommon_trap_request() != 0 && c->is_Region()) {
          Node* region = c;
          if (trace) { tty->print("XX unc region"); region->dump(); }
          for (uint j = 1; j < region->req(); j++) {
            if (is_dominator(rep_ctrl, region->in(j))) {
              if (trace) { tty->print("XX unc follows"); region->in(j)->dump(); }
              shenandoah_memory_dominates_all_paths_helper(region->in(j), rep_ctrl, controls);
            }
          }
        }
        //continue;
      } else if (use->is_Phi()) {
        assert(use->bottom_type() == Type::MEMORY, "bad phi");
        if ((use->adr_type() == TypePtr::BOTTOM /*&& !shenandoah_has_alias_phi(C, use, alias)*/) ||
            C->get_alias_index(use->adr_type()) == alias) {
          for (uint j = 1; j < use->req(); j++) {
            if (use->in(j) == nn) {
              Node* c = use->in(0)->in(j);
              if (is_dominator(rep_ctrl, c)) {
                shenandoah_memory_dominates_all_paths_helper(c, rep_ctrl, controls);
              }
            }
          }
        }
        //        continue;
      }

      if (use->is_MergeMem()) {
        if (use->as_MergeMem()->memory_at(alias) == nn) {
          if (trace) { tty->print("XX Next mem"); use->dump(); }
          // follow the memory edges
          wq.push(use);
        }
      } else if (use->is_Phi()) {
        assert(use->bottom_type() == Type::MEMORY, "bad phi");
        if ((use->adr_type() == TypePtr::BOTTOM /*&& !shenandoah_has_alias_phi(C, use, alias)*/) ||
            C->get_alias_index(use->adr_type()) == alias) {
          if (trace) { tty->print("XX Next mem"); use->dump(); }
          // follow the memory edges
          wq.push(use);
        }
      } else if (use->bottom_type() == Type::MEMORY &&
                 (use->adr_type() == TypePtr::BOTTOM || C->get_alias_index(use->adr_type()) == alias)) {
        if (trace) { tty->print("XX Next mem"); use->dump(); }
        // follow the memory edges
        wq.push(use);
      } else if ((use->is_SafePoint() || use->is_MemBar()) &&
                 (use->adr_type() == TypePtr::BOTTOM || C->get_alias_index(use->adr_type()) == alias)) {
        for (DUIterator_Fast jmax, j = use->fast_outs(jmax); j < jmax; j++) {
          Node* u = use->fast_out(j);
          if (u->bottom_type() == Type::MEMORY) {
            if (trace) { tty->print("XX Next mem"); u->dump(); }
            // follow the memory edges
            wq.push(u);
          }
        }
      } else if (use->Opcode() == Op_ShenandoahWriteBarrier && C->get_alias_index(use->adr_type()) == alias) {
        Node* m = use->find_out_with(Op_ShenandoahWBMemProj);
        if (m != NULL) {
          if (trace) { tty->print("XX Next mem"); m->dump(); }
          // follow the memory edges
          wq.push(m);
        }
      }
    }
  }

  if (controls.size() == 0) {
    return false;
  }

  for (uint i = 0; i < controls.size(); i++) {
    Node *n = controls.at(i);

    if (trace) { tty->print("X checking"); n->dump(); }

    if (n->unique_ctrl_out() != NULL) {
      continue;
    }

    if (n->Opcode() == Op_NeverBranch) {
      Node* taken = n->as_Multi()->proj_out(0);
      if (!controls.member(taken)) {
        if (trace) { tty->print("X not seen"); taken->dump(); }
        return false;
      }
      continue;
    }

    for (DUIterator_Fast jmax, j = n->fast_outs(jmax); j < jmax; j++) {
      Node* u = n->fast_out(j);

      if (u->is_CFG()) {
        if (!controls.member(u)) {
          if (u->is_Proj() && u->as_Proj()->is_uncommon_trap_proj(Deoptimization::Reason_none)) {
            if (trace) { tty->print("X not seen but unc"); u->dump(); }
          } else if (u->unique_ctrl_out() != NULL && u->unique_ctrl_out()->Opcode() == Op_Halt) {
            if (trace) { tty->print("X not seen but halt"); u->dump(); }
          } else {
            Node* c = u;
            do {
              c = c->unique_ctrl_out();
            } while (c != NULL && c->is_Region());
            if (c != NULL && c->Opcode() == Op_Halt) {
              if (trace) { tty->print("X not seen but halt"); c->dump(); }
            } else {
              if (trace) { tty->print("X not seen"); u->dump(); }
              return false;
            }
          }
        } else {
          if (trace) { tty->print("X seen"); u->dump(); }
        }
      }
    }
  }
  return true;
}
#endif

static bool has_mem_phi(Compile* C, Node* region, int alias) {
  for (DUIterator_Fast imax, i = region->fast_outs(imax); i < imax; i++) {
    Node* use = region->fast_out(i);
    if (use->is_Phi() && use->bottom_type() == Type::MEMORY &&
        (C->get_alias_index(use->adr_type()) == alias)) {
      return true;
    }
  }
  return false;
}

bool PhaseIdealLoop::shenandoah_fix_mem_phis_helper(Node* c, Node* mem, Node* mem_ctrl, Node* rep_ctrl, int alias, VectorSet& controls, GrowableArray<Node*>& regions) {
  const bool trace = false;
  Node_List wq;
  wq.push(c);

#ifdef ASSERT
  if (trace) { tty->print("YYY from"); c->dump(); }
  if (trace) { tty->print("YYY with mem"); mem->dump(); }
#endif

  while(wq.size() > 0) {
    c = wq.pop();

    while (!c->is_Region() || c->is_Loop()) {
#ifdef ASSERT
      if (trace) { tty->print("YYY"); c->dump(); }
#endif
      assert(c->is_CFG(), "node should be control node");
      if (c == mem_ctrl || is_dominator(c, rep_ctrl)) {
        c = NULL;
        break;
      } else if (c->is_Loop()) {
        c = c->in(LoopNode::EntryControl);
      } else {
        c = c->in(0);
      }
    }
    if (c == NULL) {
      continue;
    }

#ifdef ASSERT
    if (trace) { tty->print("YYY new region"); c->dump(); }
#endif

    bool has_phi = has_mem_phi(C, c, alias);
    if (!has_phi) {

      Node* m = mem;
      Node* m_ctrl = ctrl_or_self(m);
      {
        ResourceMark rm;
        VectorSet wq(Thread::current()->resource_area());
        wq.set(m->_idx);
        while (!is_dominator(m_ctrl, c) || m_ctrl == c) {
          m = next_mem(m, alias);
          if (wq.test_set(m->_idx)) {
            return false;
          }
          m_ctrl = ctrl_or_self(m);
        }
      }

      assert(m->bottom_type() == Type::MEMORY, "");

      if (m->is_MergeMem()) {
        m = m->as_MergeMem()->memory_at(alias);
        m_ctrl = ctrl_or_self(m);
      }

#ifdef ASSERT
      if (trace) { tty->print("YYY mem "); m->dump(); }
#endif

      if (controls.test(c->_idx)) {
        int i;
        for (i = 0; i < regions.length() && regions.at(i) != c; i+=2) {
          // deliberately empty, rolling over the regions
        }
        assert(i < regions.length(), "missing region");
        Node* prev_m = regions.at(i+1);
        if (prev_m == m) {
          continue;
        }
#ifdef ASSERT
        if (trace) { tty->print("YYY prev mem "); prev_m->dump(); }
#endif
        Node* prev_m_ctrl = ctrl_or_self(prev_m);
        assert(ShenandoahBarrierNode::is_dominator(m_ctrl, prev_m_ctrl, m, prev_m, this) ||
               ShenandoahBarrierNode::is_dominator(prev_m_ctrl, m_ctrl, prev_m, m, this), "one should dominate the other");
        if (ShenandoahBarrierNode::is_dominator(m_ctrl, prev_m_ctrl, m, prev_m, this)) {
          continue;
        }
#ifdef ASSERT
        if (trace) { tty->print("YYY Fixing "); c->dump(); }
#endif
        regions.at_put(i+1, m);
      } else {
#ifdef ASSERT
        if (trace) { tty->print("YYY Pushing "); c->dump(); }
#endif
        regions.push(c);
        regions.push(m);
      }
    } else {
      continue;
    }

    controls.set(c->_idx);

    for (uint i = 1; i < c->req(); i++) {
      wq.push(c->in(i));
    }
  }
  return true;
}


bool PhaseIdealLoop::shenandoah_fix_mem_phis(Node* mem, Node* mem_ctrl, Node* rep_ctrl, int alias) {
  GrowableArray<Node*> regions;
  VectorSet controls(Thread::current()->resource_area());
  const bool trace = false;

#ifdef ASSERT
  if (trace) { tty->print("YYY mem is "); mem->dump(); }
  if (trace) { tty->print("YYY mem ctrl is "); mem_ctrl->dump(); }
  if (trace) { tty->print("YYY rep ctrl is "); rep_ctrl->dump(); }
  if (trace) { tty->print_cr("YYY alias is %d", alias); }
#endif

  // Walk memory edges from mem until we hit a memory point where
  // control is known then follow the control up looking for regions
  // with no memory Phi for alias
  Unique_Node_List wq;
  wq.push(mem);

  for (uint next = 0; next < wq.size(); next++) {
    Node *n = wq.at(next);
#ifdef ASSERT
    if (trace) { tty->print("YYY from (2) "); n->dump(); }
#endif
    for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
      Node* u = n->fast_out(i);
#ifdef ASSERT
      if (trace) { tty->print("YYY processing "); u->dump(); }
#endif
      if (u->is_Phi()) {
        assert(u->bottom_type() == Type::MEMORY, "strange memory graph");
        if (ShenandoahWriteBarrierNode::should_process_phi(u, alias, C)) {
          for (uint j = 1; j < u->req(); j++) {
            if (u->in(j) == n) {
              Node *c = u->in(0)->in(j);
              if (!shenandoah_fix_mem_phis_helper(c, n, mem_ctrl, rep_ctrl, alias, controls, regions)) {
                return false;
              }
            }
          }
        }
#ifdef ASSERT
      } else if (u->is_CallStaticJava() && u->as_CallStaticJava()->uncommon_trap_request() != 0) {
        if (!shenandoah_fix_mem_phis_helper(u->in(0), n, mem_ctrl, rep_ctrl, alias, controls, regions)) {
          return false;
        }
#endif
      } else if ((u->is_CFG() && u->adr_type() == TypePtr::BOTTOM) || u->Opcode() == Op_Rethrow || u->Opcode() == Op_Return) {
        if (!shenandoah_fix_mem_phis_helper(u->in(0), n, mem_ctrl, rep_ctrl, alias, controls, regions)) {
          return false;
        }
      } else if (u->is_MergeMem() && u->as_MergeMem()->memory_at(alias) == n) {
        wq.push(u);
      } else if (u->Opcode() == Op_ShenandoahWriteBarrier && C->get_alias_index(u->adr_type()) == alias) {
        Node* m = u->find_out_with(Op_ShenandoahWBMemProj);
        if (m != NULL) {
          wq.push(m);
        }
      }
    }
  }
#ifdef ASSERT
  if (trace) {
    tty->print_cr("XXXXXXXXXXXXXXXXXXXX");
    for (int i = 0; i < regions.length(); i++) {
      Node* r = regions.at(i);
      tty->print("%d", i); r->dump();
    }
    tty->print_cr("XXXXXXXXXXXXXXXXXXXX");
  }
#endif

  if (regions.length() == 0) {
    return true;
  }

  {
    int i = 0;
    for (; i < regions.length(); i+=2) {
      Node* region = regions.at(i);
      bool has_phi = false;
      for (DUIterator_Fast jmax, j = region->fast_outs(jmax); j < jmax && !has_phi; j++) {
        Node* u = region->fast_out(j);
        if (u->is_Phi() && u->bottom_type() == Type::MEMORY &&
            (u->adr_type() == TypePtr::BOTTOM || C->get_alias_index(u->adr_type()) == alias)) {
          has_phi = true;
        }
      }
      if (!has_phi) {
        break;
      }
    }
    if (i == regions.length()) {
      return true;
    }
  }

  // Try to restrict the update to path that post dominates rep_ctrl
  int k = 0;
  int start = 0;
  int end = 0;
  do {
    start = end;
    end = k;
    for (int i = end; i < regions.length(); i+=2) {
      Node* r = regions.at(i);
      int prev = k;
      for (uint j = 1; j < r->req() && prev == k; j++) {
        if (end == 0) {
          if (is_dominator(rep_ctrl, r->in(j))) {
            Node* mem = regions.at(i+1);
            regions.at_put(i, regions.at(k));
            regions.at_put(i+1, regions.at(k+1));
            regions.at_put(k, r);
            regions.at_put(k+1, mem);
            k+=2;
          }
        } else {
          for (int l = start; l < end && prev == k; l+=2) {
            Node* r2 = regions.at(l);
            if (is_dominator(r2, r->in(j))) {
              Node* mem = regions.at(i+1);
              regions.at_put(i, regions.at(k));
              regions.at_put(i+1, regions.at(k+1));
              regions.at_put(k, r);
              regions.at_put(k+1, mem);
              k+=2;
            }
          }
        }
      }
    }
#ifdef ASSERT
    if (trace) { tty->print_cr("k = %d start = %d end = %d", k, start, end); }
#endif
  } while(k != end);

#ifdef ASSERT
  if (end != regions.length()) {
    if (trace) { tty->print_cr("Compacting %d -> %d", regions.length(), end); }
  }
#endif
  regions.trunc_to(end);

#ifdef ASSERT
  if (trace) {
    tty->print_cr("XXXXXXXXXXXXXXXXXXXX");
    for (int i = 0; i < regions.length(); i++) {
      Node* r = regions.at(i);
      tty->print("%d", i); r->dump();
    }
    tty->print_cr("XXXXXXXXXXXXXXXXXXXX");
  }
#endif

  // Creating new phis must be done in post order
  while (regions.length() > 0) {
    int i = 0;
    for (; i < regions.length(); i+=2) {
      Node* r1 = regions.at(i);
      bool is_dom = false;
      for (int j = 0; j < regions.length() && !is_dom; j+=2) {
        if (i != j) {
          Node* r2 = regions.at(j);
          for (uint k = 1; k < r2->req() && !is_dom; k++) {
            if (is_dominator(r1, r2->in(k))) {
              is_dom = true;
            }
          }
        }
      }
      if (!is_dom) {
        break;
      }
    }
    assert(i < regions.length(), "need one");
    Node* r = regions.at(i);
    Node* m = regions.at(i+1);
    regions.delete_at(i+1);
    regions.delete_at(i);

    if (!shenandoah_suitable_mem(m, NULL, NULL)) {
      return false;
    }
    Node* phi = PhiNode::make(r, m, Type::MEMORY, C->get_adr_type(alias));
#ifdef ASSERT
    if (trace) { tty->print("YYY Adding new mem phi "); phi->dump(); }
#endif
    register_new_node(phi, r);

    ShenandoahWriteBarrierNode::fix_memory_uses(m, phi, phi, r, C->get_alias_index(phi->adr_type()), this);
    assert(phi->outcnt() != 0, "new proj should have uses");
    if (phi->outcnt() == 0) {
      _igvn.remove_dead_node(phi);
    }
  }

  return true;
}

Node* PhaseIdealLoop::shenandoah_dom_mem(Node* mem, Node*& mem_ctrl, Node* n, Node* rep_ctrl, int alias) {
  ResourceMark rm;
  VectorSet wq(Thread::current()->resource_area());
  wq.set(mem->_idx);
  mem_ctrl = get_ctrl(mem);
  while (!ShenandoahBarrierNode::is_dominator(mem_ctrl, rep_ctrl, mem, n, this)) {
    mem = next_mem(mem, alias);
    if (wq.test_set(mem->_idx)) {
      return NULL; // hit an unexpected loop
    }
    mem_ctrl = ctrl_or_self(mem);
  }
  if (mem->is_MergeMem()) {
    mem = mem->as_MergeMem()->memory_at(alias);
    mem_ctrl = ctrl_or_self(mem);
  }
  return mem;
}

Node* PhaseIdealLoop::try_common_shenandoah_barriers(Node* n, Node *n_ctrl) {
  if (n->is_ShenandoahBarrier() && !C->has_irreducible_loop()) {
    // We look for a write barrier whose memory edge dominates n
    // Either the replacement write barrier dominates n or we have,
    // for instance:
    // if ( ) {
    //   read barrier n
    // } else {
    //   write barrier
    // }
    // in which case replacing n by the write barrier causes the write
    // barrier to move above the if() and the memory Phi that merges
    // the memory state for both branches must be updated so both
    // inputs become the write barrier's memory projection (and the
    // Phi is optimized out) otherwise we risk loosing a memory
    // dependency.
    // Once we find a replacement write barrier, the code below fixes
    // the memory graph in cases like the one above.
    Node* val = n->in(ShenandoahBarrierNode::ValueIn);
    Node* val_ctrl = get_ctrl(val);
    Node* n_proj = n->find_out_with(Op_ShenandoahWBMemProj);
    Node* replacement = NULL;
    int alias = C->get_alias_index(n->adr_type());
    for (DUIterator_Fast imax, i = val->fast_outs(imax); i < imax && replacement == NULL; i++) {
      Node* u = val->fast_out(i);
      if (u != n && u->Opcode() == Op_ShenandoahWriteBarrier) {
        Node* u_mem = u->in(ShenandoahBarrierNode::Memory);
        Node* u_proj = u->find_out_with(Op_ShenandoahWBMemProj);
        Node* u_ctrl = get_ctrl(u);
        Node* u_mem_ctrl = get_ctrl(u_mem);
        IdealLoopTree* n_loop = get_loop(n_ctrl);
        IdealLoopTree* u_loop = get_loop(u_ctrl);

        Node* ctrl = dom_lca(u_ctrl, n_ctrl);

        if (ctrl->is_Proj() &&
            ctrl->in(0)->is_Call() &&
            ctrl->unique_ctrl_out() != NULL &&
            ctrl->unique_ctrl_out()->Opcode() == Op_Catch &&
            !is_dominator(val_ctrl, ctrl->in(0)->in(0))) {
          continue;
        }

        if (n->Opcode() == Op_ShenandoahWriteBarrier && u_proj == NULL && n_proj != NULL) {
          continue;
        }

        IdealLoopTree* loop = get_loop(ctrl);

        // we don't want to move a write barrier in a loop
        if (loop->is_member(u_loop) || (n->Opcode() == Op_ShenandoahWriteBarrier && loop->is_member(n_loop))) {
          if (ShenandoahDontIncreaseWBFreq) {
            Node* u_iffproj = shenandoah_no_branches(u_ctrl, ctrl, true);
            if (n->Opcode() == Op_ShenandoahWriteBarrier) {
              Node* n_iffproj = shenandoah_no_branches(n_ctrl, ctrl, true);
              if (u_iffproj == NULL || n_iffproj == NULL) {
                replacement = u;
              } else if (u_iffproj != NodeSentinel && n_iffproj != NodeSentinel && u_iffproj->in(0) == n_iffproj->in(0)) {
                replacement = u;
              }
            } else if (u_iffproj == NULL) {
              replacement = u;
            }
          } else {
            replacement = u;
          }
        }
      }
    }
    if (replacement != NULL) {
      Node* old_ctrl = get_ctrl(replacement);
      Node* rep_ctrl = dom_lca(n_ctrl, old_ctrl);
      if (rep_ctrl->is_Proj() &&
          rep_ctrl->in(0)->is_Call() &&
          rep_ctrl->unique_ctrl_out() != NULL &&
          rep_ctrl->unique_ctrl_out()->Opcode() == Op_Catch) {
        rep_ctrl = rep_ctrl->in(0)->in(0);
        assert(is_dominator(val_ctrl, rep_ctrl), "bad control");
      } else {
        Node* c = try_move_shenandoah_barrier_before_pre_loop(rep_ctrl, val_ctrl);
        if (c != NULL) {
          rep_ctrl = shenandoah_move_above_predicates(c, val_ctrl);
        } else {
          while (rep_ctrl->is_IfProj()) {
            CallStaticJavaNode* unc = rep_ctrl->as_Proj()->is_uncommon_trap_if_pattern(Deoptimization::Reason_none);
            if (unc != NULL) {
              int req = unc->uncommon_trap_request();
              Deoptimization::DeoptReason trap_reason = Deoptimization::trap_request_reason(req);
              if ((trap_reason == Deoptimization::Reason_loop_limit_check ||
                   trap_reason == Deoptimization::Reason_predicate) && is_dominator(val_ctrl, rep_ctrl->in(0)->in(0))) {
                rep_ctrl = rep_ctrl->in(0)->in(0);
                continue;
              }
            }
            break;
          }
        }
      }

      Node* mem = replacement->in(ShenandoahBarrierNode::Memory);
      Node* old_mem = mem;
      Node* rep_proj = replacement->find_out_with(Op_ShenandoahWBMemProj);
      {
        Node* mem_ctrl = NULL;

        mem = shenandoah_dom_mem(mem, mem_ctrl, n, rep_ctrl, alias);
        if (mem == NULL) {
          return NULL;
        }

        // Add a memory Phi for the slice of the write barrier to any
        // region that post dominates rep_ctrl and doesn't have one
        // already.
        if (rep_proj != NULL && !shenandoah_fix_mem_phis(mem, mem_ctrl, rep_ctrl, alias)) {
          return NULL;
        }

        assert(shenandoah_memory_dominates_all_paths(mem, rep_ctrl, alias), "can't fix the memory graph");
      }
      assert(_igvn.type(mem) == Type::MEMORY, "not memory");

      if (rep_proj != NULL) {
        Node* old_mem = replacement->in(ShenandoahBarrierNode::Memory);
        if (!shenandoah_suitable_mem(mem, old_mem, rep_proj)) {
          return NULL;
        }

        if (replacement->in(ShenandoahBarrierNode::Memory) != mem) {
          // tty->print("XXX setting memory of"); replacement->dump();
          // tty->print("XXX to"); mem->dump();
          for (DUIterator_Last imin, i = rep_proj->last_outs(imin); i >= imin; ) {
            Node* u = rep_proj->last_out(i);
            _igvn.rehash_node_delayed(u);
            int uses_found = u->replace_edge(rep_proj, old_mem);
            i -= uses_found;
          }
          _igvn.replace_input_of(replacement, ShenandoahBarrierNode::Memory, mem);
        }
        set_ctrl_and_loop(replacement, rep_ctrl);
        _igvn.replace_input_of(replacement, ShenandoahBarrierNode::Control, rep_ctrl);

        ShenandoahWriteBarrierNode::fix_memory_uses(mem, replacement, rep_proj, rep_ctrl, C->get_alias_index(replacement->adr_type()), this);
        assert(rep_proj->outcnt() != 0, "new proj should have uses");
      } else {
        if (replacement->in(ShenandoahBarrierNode::Memory) != mem) {
          _igvn._worklist.push(replacement->in(ShenandoahBarrierNode::Memory));
          _igvn.replace_input_of(replacement, ShenandoahBarrierNode::Memory, mem);
        }
        set_ctrl_and_loop(replacement, rep_ctrl);
        _igvn.replace_input_of(replacement, ShenandoahBarrierNode::Control, rep_ctrl);
      }
      if (n->Opcode() == Op_ShenandoahWriteBarrier) {
        if (n_proj != NULL) {
          lazy_replace(n_proj, n->in(ShenandoahBarrierNode::Memory));
        }
      }
      lazy_replace(n, replacement);
      if (rep_proj != NULL) {
        set_ctrl_and_loop(rep_proj, rep_ctrl);
      }
      return replacement;
    }
  }

  return NULL;
}

static void shenandoah_disconnect_barrier_mem(Node* wb, PhaseIterGVN& igvn) {
  Node* mem_in = wb->in(ShenandoahBarrierNode::Memory);
  Node* proj = wb->find_out_with(Op_ShenandoahWBMemProj);

  for (DUIterator_Last imin, i = proj->last_outs(imin); i >= imin; ) {
    Node* u = proj->last_out(i);
    igvn.rehash_node_delayed(u);
    int nb = u->replace_edge(proj, mem_in);
    assert(nb > 0, "no replacement?");
    i -= nb;
  }
}

Node* PhaseIdealLoop::shenandoah_move_above_predicates(Node* cl, Node* val_ctrl) {
  Node* entry = cl->in(LoopNode::EntryControl);
  Node* above_pred = skip_loop_predicates(entry);
  Node* ctrl = entry;
  while (ctrl != above_pred) {
    Node* next = ctrl->in(0);
    if (!is_dominator(val_ctrl, next)) {
      break;
    }
    ctrl = next;
  }
  return ctrl;
}

Node* PhaseIdealLoop::try_move_shenandoah_barrier_before_loop_helper(Node* n, Node* cl, Node* val_ctrl, Node* mem) {
  assert(cl->is_Loop(), "bad control");
  assert(n->Opcode() == Op_ShenandoahWriteBarrier, "only for shenandoah write barriers");
  Node* ctrl = shenandoah_move_above_predicates(cl, val_ctrl);
  Node* mem_ctrl = NULL;
  int alias = C->get_alias_index(n->adr_type());
  mem = shenandoah_dom_mem(mem, mem_ctrl, n, ctrl, alias);
  if (mem == NULL) {
    return NULL;
  }

  Node* old_mem = n->in(ShenandoahBarrierNode::Memory);
  Node* proj = n->find_out_with(Op_ShenandoahWBMemProj);
  if (old_mem != mem && !shenandoah_suitable_mem(mem, old_mem, proj)) {
    return NULL;
  }

  assert(shenandoah_memory_dominates_all_paths(mem, ctrl, alias), "can't fix the memory graph");
  set_ctrl_and_loop(n, ctrl);
  _igvn.replace_input_of(n, ShenandoahBarrierNode::Control, ctrl);
  if (old_mem != mem) {
    if (proj != NULL) {
      shenandoah_disconnect_barrier_mem(n, _igvn);
      ShenandoahWriteBarrierNode::fix_memory_uses(mem, n, proj, ctrl, C->get_alias_index(n->adr_type()), this);
      assert(proj->outcnt() > 0, "disconnected write barrier");
    }
    _igvn.replace_input_of(n, ShenandoahBarrierNode::Memory, mem);
  }
  if (proj != NULL) {
    set_ctrl_and_loop(proj, ctrl);
  }
  return n;
}

Node* PhaseIdealLoop::try_move_shenandoah_barrier_before_pre_loop(Node* c, Node* val_ctrl) {
  // A write barrier between a pre and main loop can get in the way of
  // vectorization. Move it above the pre loop if possible
  CountedLoopNode* cl = NULL;
  if (c->is_IfFalse() &&
      c->in(0)->is_CountedLoopEnd()) {
    cl = c->in(0)->as_CountedLoopEnd()->loopnode();
  } else if (c->is_IfProj() &&
             c->in(0)->is_If() &&
             c->in(0)->in(0)->is_IfFalse() &&
             c->in(0)->in(0)->in(0)->is_CountedLoopEnd()) {
    cl = c->in(0)->in(0)->in(0)->as_CountedLoopEnd()->loopnode();
  }
  if (cl != NULL &&
      cl->is_pre_loop() &&
      val_ctrl != cl &&
      is_dominator(val_ctrl, cl)) {
    return cl;
  }
  return NULL;
}

Node* PhaseIdealLoop::try_move_shenandoah_barrier_before_loop(Node* n, Node *n_ctrl) {
  if (n->Opcode() == Op_ShenandoahWriteBarrier) {
    IdealLoopTree *n_loop = get_loop(n_ctrl);
    Node* val = n->in(ShenandoahBarrierNode::ValueIn);
    Node* val_ctrl = get_ctrl(val);
    if (n_loop != _ltree_root && !n_loop->_irreducible) {
      IdealLoopTree *val_loop = get_loop(val_ctrl);
      Node* mem = n->in(ShenandoahBarrierNode::Memory);
      IdealLoopTree *mem_loop = get_loop(get_ctrl(mem));
      if (!n_loop->is_member(val_loop) &&
          n_loop->is_member(mem_loop)) {
        Node* n_loop_head = n_loop->_head;

        if (n_loop_head->is_Loop()) {
          Node* loop = n_loop_head;
          if (n_loop_head->is_CountedLoop() && n_loop_head->as_CountedLoop()->is_main_loop()) {
            Node* res = try_move_shenandoah_barrier_before_pre_loop(n_loop_head->in(LoopNode::EntryControl), val_ctrl);
            if (res != NULL) {
              loop = res;
            }
          }

          return try_move_shenandoah_barrier_before_loop_helper(n, loop, val_ctrl, mem);
        }
      }
    }
    Node* ctrl = try_move_shenandoah_barrier_before_pre_loop(n->in(0), val_ctrl);
    if (ctrl != NULL) {
      return try_move_shenandoah_barrier_before_loop_helper(n, ctrl, val_ctrl, n->in(ShenandoahBarrierNode::Memory));
    }
  }
  return NULL;
}

void PhaseIdealLoop::try_move_shenandoah_read_barrier(Node* n, Node *n_ctrl) {
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    ShenandoahReadBarrierNode* rb = (ShenandoahReadBarrierNode*)n;
    Node* mem = n->in(MemNode::Memory);
    int alias = C->get_alias_index(n->adr_type());
    const bool trace = false;

#ifdef ASSERT
    if (trace) { tty->print("Trying to move mem of"); n->dump(); }
#endif

    Node* new_mem = mem;

    ResourceMark rm;
    VectorSet seen(Thread::current()->resource_area());
    Node_List phis;

    for (;;) {
#ifdef ASSERT
      if (trace) { tty->print("Looking for dominator from"); mem->dump(); }
#endif
      if (mem->is_Proj() && mem->in(0)->is_Start()) {
        if (new_mem != n->in(MemNode::Memory)) {
#ifdef ASSERT
          if (trace) { tty->print("XXX Setting mem to"); new_mem->dump(); tty->print(" for "); n->dump(); }
#endif
          _igvn.replace_input_of(n, MemNode::Memory, new_mem);
        }
        return;
      }

      Node* candidate = mem;
      do {
        if (!rb->is_independent(mem)) {
          if (trace) { tty->print_cr("Not independent"); }
          if (new_mem != n->in(MemNode::Memory)) {
#ifdef ASSERT
            if (trace) { tty->print("XXX Setting mem to"); new_mem->dump(); tty->print(" for "); n->dump(); }
#endif
            _igvn.replace_input_of(n, MemNode::Memory, new_mem);
          }
          return;
        }
        if (seen.test_set(mem->_idx)) {
          if (trace) { tty->print_cr("Already seen"); }
          ShouldNotReachHere();
          // Strange graph
          if (new_mem != n->in(MemNode::Memory)) {
#ifdef ASSERT
            if (trace) { tty->print("XXX Setting mem to"); new_mem->dump(); tty->print(" for "); n->dump(); }
#endif
            _igvn.replace_input_of(n, MemNode::Memory, new_mem);
          }
          return;
        }
        if (mem->is_Phi()) {
          phis.push(mem);
        }
        mem = next_mem(mem, alias);
        if (mem->bottom_type() == Type::MEMORY) {
          candidate = mem;
        }
        assert(ShenandoahBarrierNode::is_dominator(ctrl_or_self(mem), n_ctrl, mem, n, this) == is_dominator(ctrl_or_self(mem), n_ctrl), "strange dominator");
#ifdef ASSERT
        if (trace) { tty->print("Next mem is"); mem->dump(); }
#endif
      } while (mem->bottom_type() != Type::MEMORY || !is_dominator(ctrl_or_self(mem), n_ctrl));

      assert(mem->bottom_type() == Type::MEMORY, "bad mem");

      bool not_dom = false;
      for (uint i = 0; i < phis.size() && !not_dom; i++) {
        Node* nn = phis.at(i);

#ifdef ASSERT
        if (trace) { tty->print("Looking from phi"); nn->dump(); }
#endif
        assert(nn->is_Phi(), "phis only");
        for (uint j = 2; j < nn->req() && !not_dom; j++) {
          Node* m = nn->in(j);
#ifdef ASSERT
          if (trace) { tty->print("Input %d is", j); m->dump(); }
#endif
          while (m != mem && !seen.test_set(m->_idx)) {
            if (ShenandoahBarrierNode::is_dominator(ctrl_or_self(m), ctrl_or_self(mem), m, mem, this)) {
              not_dom = true;
              // Scheduling anomaly
#ifdef ASSERT
              if (trace) { tty->print("Giving up"); m->dump(); }
#endif
              break;
            }
            if (!rb->is_independent(m)) {
              if (trace) { tty->print_cr("Not independent"); }
              if (new_mem != n->in(MemNode::Memory)) {
#ifdef ASSERT
                if (trace) { tty->print("XXX Setting mem to"); new_mem->dump(); tty->print(" for "); n->dump(); }
#endif
                _igvn.replace_input_of(n, MemNode::Memory, new_mem);
              }
              return;
            }
            if (m->is_Phi()) {
              phis.push(m);
            }
            m = next_mem(m, alias);
#ifdef ASSERT
            if (trace) { tty->print("Next mem is"); m->dump(); }
#endif
          }
        }
      }
      if (!not_dom) {
        new_mem = mem;
        phis.clear();
      } else {
        seen.Clear();
      }
    }
  }
}

CallStaticJavaNode* PhaseIdealLoop::shenandoah_pin_and_expand_barriers_null_check(ShenandoahBarrierNode* wb) {
  Node* val = wb->in(ShenandoahBarrierNode::ValueIn);

#ifdef ASSERT
  const Type* val_t = _igvn.type(val);
  assert(val_t->meet(TypePtr::NULL_PTR) != val_t, "should be not null");
#endif

  if (val->Opcode() == Op_CastPP &&
      val->in(0) != NULL &&
      val->in(0)->Opcode() == Op_IfTrue &&
      val->in(0)->as_Proj()->is_uncommon_trap_if_pattern(Deoptimization::Reason_none) &&
      val->in(0)->in(0)->is_If() &&
      val->in(0)->in(0)->in(1)->Opcode() == Op_Bool &&
      val->in(0)->in(0)->in(1)->as_Bool()->_test._test == BoolTest::ne &&
      val->in(0)->in(0)->in(1)->in(1)->Opcode() == Op_CmpP &&
      val->in(0)->in(0)->in(1)->in(1)->in(1) == val->in(1) &&
      val->in(0)->in(0)->in(1)->in(1)->in(2)->bottom_type() == TypePtr::NULL_PTR) {
    assert(val->in(0)->in(0)->in(1)->in(1)->in(1) == val->in(1), "");
    CallStaticJavaNode* unc = val->in(0)->as_Proj()->is_uncommon_trap_if_pattern(Deoptimization::Reason_none);
    return unc;
  }
  return NULL;
}

void PhaseIdealLoop::shenandoah_pin_and_expand_barriers_move_barrier(ShenandoahBarrierNode* wb) {
  Node* unc = shenandoah_pin_and_expand_barriers_null_check(wb);
  Node* val = wb->in(ShenandoahBarrierNode::ValueIn);

  if (unc != NULL) {
    Node* ctrl = get_ctrl(wb);
    Node* unc_ctrl = val->in(0);

    Node* branch = shenandoah_no_branches(ctrl, unc_ctrl, false);
    assert(branch == NULL || branch == NodeSentinel, "was not looking for a branch");
    if (branch == NodeSentinel) {
      return;
    }

    Node* mem = wb->in(ShenandoahBarrierNode::Memory);
    Node* old_mem = mem;

    Node* mem_ctrl = NULL;
    int alias = C->get_alias_index(wb->adr_type());
    mem = shenandoah_dom_mem(mem, mem_ctrl, wb, unc_ctrl, alias);
    if (mem == NULL) {
      return;
    }

    Node* proj = wb->find_out_with(Op_ShenandoahWBMemProj);
    if (mem != old_mem && !shenandoah_fix_mem_phis(mem, mem_ctrl, unc_ctrl, alias)) {
      return;
    }

    assert(mem == old_mem || shenandoah_memory_dominates_all_paths(mem, unc_ctrl, alias), "can't fix the memory graph");
    set_ctrl_and_loop(wb, unc_ctrl);
    if (wb->in(ShenandoahBarrierNode::Control) != NULL) {
      _igvn.replace_input_of(wb, ShenandoahBarrierNode::Control, unc_ctrl);
    }
    shenandoah_disconnect_barrier_mem(wb, _igvn);
    ShenandoahWriteBarrierNode::fix_memory_uses(mem, wb, proj, unc_ctrl, C->get_alias_index(wb->adr_type()), this);
    assert(proj->outcnt() > 0, "disconnected write barrier");
    _igvn.replace_input_of(wb, ShenandoahBarrierNode::Memory, mem);
    set_ctrl_and_loop(proj, unc_ctrl);
  }
}

Node* PhaseIdealLoop::shenandoah_pick_phi(Node* phi1, Node* phi2, Node_Stack& phis, VectorSet& visited) {
  assert(phis.size() == 0, "stack needs to be empty");
  uint i = 1;
  int phi_dominates = -1;
  for (;;) {
    assert(phi1->req() == phi2->req(), "strange pair of phis");
    assert(phis.size() % 2 == 0, "");
    Node* in1 = phi1->in(i);
    Node* in2 = phi2->in(i);

    if (in1->is_MergeMem()) {
      in1 = in1->as_MergeMem()->base_memory();
    }
    if (in2->is_MergeMem()) {
      in2 = in2->as_MergeMem()->base_memory();
    }

    if (in1 == in2) {
      //continue
    } else if (in1->is_Phi() && in2->is_Phi() && in1->in(0) == in2->in(0)) {
      assert(!visited.test_set(in1->_idx), "no loop");
      assert(!visited.test_set(in2->_idx), "no loop");
      phis.push(phi1, i+1);
      phis.push(phi2, i+1);
      phi1 = in1;
      phi2 = in2;
      i = 1;
    } else {
      Node* in1_c = get_ctrl(in1);
      Node* in2_c = get_ctrl(in2);
      if (ShenandoahBarrierNode::is_dominator(in1_c, in2_c, in1, in2, this)) {
        assert(!ShenandoahBarrierNode::is_dominator(in2_c, in1_c, in2, in1, this), "one has to dominate the other");
        assert(phi_dominates == -1 || phi_dominates == 1, "all inputs must dominate");
        phi_dominates = 1;
      } else {
        assert(ShenandoahBarrierNode::is_dominator(in2_c, in1_c, in2, in1, this), "one must dominate the other");
        assert(!ShenandoahBarrierNode::is_dominator(in1_c, in2_c, in1, in2, this), "one has to dominate the other");
        assert(phi_dominates == -1 || phi_dominates == 2, "all inputs must dominate");
        phi_dominates = 2;
      }
    }
    i++;

    while (i >= phi1->req() && phis.size() > 0) {
      i = phis.index();
      phi2 = phis.node();
      phis.pop();
      phi1 = phis.node();
      phis.pop();
    }

    if (i >= phi1->req() && phis.size() == 0) {
      Node* phi = NULL;
      if (phi_dominates == 1) {
        return phi2;
      } else if (phi_dominates == 2) {
        return phi1;
      } else {
        return phi1;
      }
    }
  }
  return NULL;
}

bool ShenandoahWriteBarrierNode::mem_is_valid(Node* m, Node* c, PhaseIdealLoop* phase) {
  return m != NULL && get_ctrl(m, phase) == c;
}

Node* ShenandoahWriteBarrierNode::find_raw_mem(Node* ctrl, Node* n, const Node_List& memory_nodes, PhaseIdealLoop* phase) {
  assert(n == NULL || phase->ctrl_or_self(n) == ctrl, "");
  Node* raw_mem = memory_nodes[ctrl->_idx];
  Node* c = ctrl;
  while (!mem_is_valid(raw_mem, c, phase) &&
         (!c->is_CatchProj() || raw_mem == NULL || c->in(0)->in(0)->in(0) != get_ctrl(raw_mem, phase))) {
    c = phase->idom(c);
    raw_mem = memory_nodes[c->_idx];
  }
  if (n != NULL && mem_is_valid(raw_mem, c, phase)) {
    while (!is_dominator_same_ctrl(c, raw_mem, n, phase) && phase->ctrl_or_self(raw_mem) == ctrl) {
      raw_mem = next_mem(raw_mem, Compile::AliasIdxRaw);
    }
    if (raw_mem->is_MergeMem()) {
      raw_mem = raw_mem->as_MergeMem()->memory_at(Compile::AliasIdxRaw);
    }
    if (!mem_is_valid(raw_mem, c, phase)) {
      do {
        c = phase->idom(c);
        raw_mem = memory_nodes[c->_idx];
      } while (!mem_is_valid(raw_mem, c, phase) &&
               (!c->is_CatchProj() || raw_mem == NULL || c->in(0)->in(0)->in(0) != get_ctrl(raw_mem, phase)));
    }
  }
  assert(raw_mem->bottom_type() == Type::MEMORY, "");
  return raw_mem;
}

Node* PhaseIdealLoop::shenandoah_find_bottom_mem(Node* ctrl) {
  Node* mem = NULL;
  Node* c = ctrl;
  do {
    if (c->is_Region()) {
      Node* phi_bottom = NULL;
      for (DUIterator_Fast imax, i = c->fast_outs(imax); i < imax; i++) {
        Node* u = c->fast_out(i);
        if (u->is_Phi() && u->bottom_type() == Type::MEMORY) {
          if (u->adr_type() == TypePtr::BOTTOM) {
            if (phi_bottom != NULL) {
              phi_bottom = NodeSentinel;
            } else {
              phi_bottom = u;
            }
          }
        }
      }
      if (phi_bottom != NULL) {
        if (phi_bottom != NodeSentinel) {
          mem = phi_bottom;
        } else {
          Node* phi = NULL;
          ResourceMark rm;
          Node_Stack phis(0);
          VectorSet visited(Thread::current()->resource_area());
          for (DUIterator_Fast imax, i = c->fast_outs(imax); i < imax; i++) {
            Node* u = c->fast_out(i);
            if (u->is_Phi() && u->bottom_type() == Type::MEMORY && u->adr_type() == TypePtr::BOTTOM) {
              if (phi == NULL) {
                phi = u;
              } else {
                phi = shenandoah_pick_phi(phi, u, phis, visited);
              }
            }
          }
          mem = phi;
        }
      }
    } else {
      if (c->is_Call() && c->as_Call()->adr_type() != NULL) {
        CallProjections projs;
        c->as_Call()->extract_projections(&projs, true, false);
        if (projs.fallthrough_memproj != NULL) {
          if (projs.fallthrough_memproj->adr_type() == TypePtr::BOTTOM) {
            if (projs.catchall_memproj == NULL) {
              mem = projs.fallthrough_memproj;
            } else {
              if (is_dominator(projs.fallthrough_catchproj, ctrl)) {
                mem = projs.fallthrough_memproj;
              } else {
                assert(is_dominator(projs.catchall_catchproj, ctrl), "one proj must dominate barrier");
                mem = projs.catchall_memproj;
              }
            }
          }
        } else {
          Node* proj = c->as_Call()->proj_out(TypeFunc::Memory);
          if (proj != NULL &&
              proj->adr_type() == TypePtr::BOTTOM) {
            mem = proj;
          }
        }
      } else {
        for (DUIterator_Fast imax, i = c->fast_outs(imax); i < imax; i++) {
          Node* u = c->fast_out(i);
          if (u->is_Proj() &&
              u->bottom_type() == Type::MEMORY &&
              u->adr_type() == TypePtr::BOTTOM) {
              assert(c->is_SafePoint() || c->is_MemBar() || c->is_Start(), "");
              assert(mem == NULL, "only one proj");
              mem = u;
          }
        }
        assert(!c->is_Call() || c->as_Call()->adr_type() != NULL || mem == NULL, "no mem projection expected");
      }
    }
    c = idom(c);
  } while (mem == NULL);
  return mem;
}

void PhaseIdealLoop::shenandoah_follow_barrier_uses(Node* n, Node* ctrl, Unique_Node_List& uses) {
  for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
    Node* u = n->fast_out(i);
    if (!u->is_CFG() && get_ctrl(u) == ctrl && (!u->is_Phi() || !u->in(0)->is_Loop() || u->in(LoopNode::LoopBackControl) != n)) {
      uses.push(u);
    }
  }
}

Node* ShenandoahWriteBarrierNode::get_ctrl(Node* n, PhaseIdealLoop* phase) {
  Node* c = phase->get_ctrl(n);
  if (n->is_Proj() && n->in(0)->is_Call()) {
    assert(c == n->in(0), "");
    CallNode* call = c->as_Call();
    CallProjections projs;
    call->extract_projections(&projs, true, false);
    if (projs.catchall_memproj != NULL) {
      if (projs.fallthrough_memproj == n) {
        c = projs.fallthrough_catchproj;
      } else {
        assert(projs.catchall_memproj == n, "");
        c = projs.catchall_catchproj;
      }
    }
  }
  return c;
}

Node* ShenandoahWriteBarrierNode::ctrl_or_self(Node* n, PhaseIdealLoop* phase) {
  if (phase->has_ctrl(n))
    return get_ctrl(n, phase);
  else {
    assert (n->is_CFG(), "must be a CFG node");
    return n;
  }
}

#ifdef ASSERT
static bool has_never_branch(Node* root) {
  for (uint i = 1; i < root->req(); i++) {
    Node* in = root->in(i);
    if (in != NULL && in->Opcode() == Op_Halt && in->in(0)->is_Proj() && in->in(0)->in(0)->Opcode() == Op_NeverBranch) {
      return true;
    }
  }
  return false;
}
#endif

void ShenandoahWriteBarrierNode::collect_memory_nodes(int alias, Node_List& memory_nodes, PhaseIdealLoop* phase) {
  Node_Stack stack(0);
  VectorSet visited(Thread::current()->resource_area());
  Node_List regions;

  // Walk the raw memory graph and create a mapping from CFG node to
  // memory node. Exclude phis for now.
  stack.push(phase->C->root(), 1);
  do {
    Node* n = stack.node();
    int opc = n->Opcode();
    uint i = stack.index();
    if (i < n->req()) {
      Node* mem = NULL;
      if (opc == Op_Root) {
        Node* in = n->in(i);
        int in_opc = in->Opcode();
        if (in_opc == Op_Return || in_opc == Op_Rethrow) {
          mem = in->in(TypeFunc::Memory);
        } else if (in_opc == Op_Halt) {
          if (in->in(0)->is_Region()) {
#ifdef ASSERT
            Node* r = in->in(0);
            for (uint j = 1; j <  r->req(); j++) {
              assert(r->in(j)->is_Proj() && r->in(j)->in(0)->Opcode() == Op_NeverBranch, "");
            }
#endif
          } else {
            Node* proj = in->in(0);
            assert(proj->is_Proj(), "");
            Node* in = proj->in(0);
            assert(in->is_CallStaticJava() || in->Opcode() == Op_NeverBranch || in->Opcode() == Op_Catch, "");
            if (in->is_CallStaticJava()) {
              mem = in->in(TypeFunc::Memory);
            } else if (in->Opcode() == Op_Catch) {
              Node* call = in->in(0)->in(0);
              assert(call->is_Call(), "");
              mem = call->in(TypeFunc::Memory);
            }
          }
        } else {
#ifdef ASSERT
          n->dump();
          in->dump();
#endif
          ShouldNotReachHere();
        }
      } else {
        assert(n->is_Phi() && n->bottom_type() == Type::MEMORY, "");
        assert(n->adr_type() == TypePtr::BOTTOM || phase->C->get_alias_index(n->adr_type()) == alias, "");
        mem = n->in(i);
      }
      i++;
      stack.set_index(i);
      if (mem == NULL) {
        continue;
      }
      for (;;) {
        if (visited.test_set(mem->_idx) || mem->is_Start()) {
          break;
        }
        if (mem->is_Phi()) {
          stack.push(mem, 2);
          mem = mem->in(1);
        } else if (mem->is_Proj()) {
          stack.push(mem, mem->req());
          mem = mem->in(0);
        } else if (mem->is_SafePoint() || mem->is_MemBar()) {
          mem = mem->in(TypeFunc::Memory);
        } else if (mem->is_MergeMem()) {
          mem = mem->as_MergeMem()->memory_at(alias);
        } else if (mem->is_Store() || mem->is_LoadStore() || mem->is_ClearArray()) {
          stack.push(mem, mem->req());
          mem = mem->in(MemNode::Memory);
        } else {
#ifdef ASSERT
          mem->dump();
#endif
          ShouldNotReachHere();
        }
      }
    } else {
      if (n->is_Phi()) {
        // Nothing
      } else if (!n->is_Root()) {
        Node* c = get_ctrl(n, phase);
        memory_nodes.map(c->_idx, n);
      }
      stack.pop();
    }
  } while(stack.is_nonempty());

  // Iterate over CFG nodes in rpo and propagate memory state to
  // compute memory state at regions, creating new phis if needed.
  Node_List rpo_list;
  visited.Clear();
  phase->rpo(phase->C->root(), stack, visited, rpo_list);
  Node* root = rpo_list.pop();
  assert(root == phase->C->root(), "");

  const bool trace = false;
#ifdef ASSERT
  if (trace) {
    for (int i = rpo_list.size() - 1; i >= 0; i--) {
      Node* c = rpo_list.at(i);
      if (memory_nodes[c->_idx] != NULL) {
        tty->print("X %d", c->_idx);  memory_nodes[c->_idx]->dump();
      }
    }
  }
#endif
  uint last = phase->C->unique();

#ifdef ASSERT
  uint8_t max_depth = 0;
  for (LoopTreeIterator iter(phase->ltree_root()); !iter.done(); iter.next()) {
    IdealLoopTree* lpt = iter.current();
    max_depth = MAX2(max_depth, lpt->_nest);
  }
#endif

  bool progress = true;
  int iteration = 0;
  Node_List dead_phis;
  while (progress) {
    progress = false;
    iteration++;
    assert(iteration <= 2+max_depth || phase->C->has_irreducible_loop(), "");
    if (trace) { tty->print_cr("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"); }
    IdealLoopTree* last_updated_ilt = NULL;
    for (int i = rpo_list.size() - 1; i >= 0; i--) {
      Node* c = rpo_list.at(i);

      Node* prev_mem = memory_nodes[c->_idx];
      if (c->is_Region()) {
        Node* prev_region = regions[c->_idx];
        Node* unique = NULL;
        for (uint j = 1; j < c->req() && unique != NodeSentinel; j++) {
          Node* m = memory_nodes[c->in(j)->_idx];
          assert(m != NULL || (c->is_Loop() && j == LoopNode::LoopBackControl && iteration == 1) || phase->C->has_irreducible_loop() || has_never_branch(phase->C->root()), "expect memory state");
          if (m != NULL) {
            if (m == prev_region && ((c->is_Loop() && j == LoopNode::LoopBackControl) || (prev_region->is_Phi() && prev_region->in(0) == c))) {
              assert(c->is_Loop() && j == LoopNode::LoopBackControl || phase->C->has_irreducible_loop(), "");
              // continue
            } else if (unique == NULL) {
              unique = m;
            } else if (m == unique) {
              // continue
            } else {
              unique = NodeSentinel;
            }
          }
        }
        assert(unique != NULL, "empty phi???");
        if (unique != NodeSentinel) {
          if (prev_region != NULL && prev_region->is_Phi() && prev_region->in(0) == c) {
            dead_phis.push(prev_region);
          }
          regions.map(c->_idx, unique);
        } else {
          Node* phi = NULL;
          if (prev_region != NULL && prev_region->is_Phi() && prev_region->in(0) == c && prev_region->_idx >= last) {
            phi = prev_region;
            for (uint k = 1; k < c->req(); k++) {
              Node* m = memory_nodes[c->in(k)->_idx];
              assert(m != NULL, "expect memory state");
              phi->set_req(k, m);
            }
          } else {
            for (DUIterator_Fast jmax, j = c->fast_outs(jmax); j < jmax && phi == NULL; j++) {
              Node* u = c->fast_out(j);
              if (u->is_Phi() && u->bottom_type() == Type::MEMORY &&
                  (u->adr_type() == TypePtr::BOTTOM || phase->C->get_alias_index(u->adr_type()) == alias)) {
                phi = u;
                for (uint k = 1; k < c->req() && phi != NULL; k++) {
                  Node* m = memory_nodes[c->in(k)->_idx];
                  assert(m != NULL, "expect memory state");
                  if (u->in(k) != m) {
                    phi = NULL;
                  }
                }
              }
            }
            if (phi == NULL) {
              phi = new (phase->C) PhiNode(c, Type::MEMORY, phase->C->get_adr_type(alias));
              for (uint k = 1; k < c->req(); k++) {
                Node* m = memory_nodes[c->in(k)->_idx];
                assert(m != NULL, "expect memory state");
                phi->init_req(k, m);
              }
            }
          }
          assert(phi != NULL, "");
          regions.map(c->_idx, phi);
        }
        Node* current_region = regions[c->_idx];
        if (current_region != prev_region) {
          progress = true;
          if (prev_region == prev_mem) {
            memory_nodes.map(c->_idx, current_region);
          }
        }
      } else if (prev_mem == NULL || prev_mem->is_Phi() || ctrl_or_self(prev_mem, phase) != c) {
        Node* m = memory_nodes[phase->idom(c)->_idx];
        assert(m != NULL, "expect memory state");
        if (m != prev_mem) {
          memory_nodes.map(c->_idx, m);
          progress = true;
        }
      }
#ifdef ASSERT
      if (trace) { tty->print("X %d", c->_idx);  memory_nodes[c->_idx]->dump(); }
#endif
    }
  }

  // Replace existing phi with computed memory state for that region
  // if different (could be a new phi or a dominating memory node if
  // that phi was found to be useless).
  while (dead_phis.size() > 0) {
    Node* n = dead_phis.pop();
    n->replace_by(phase->C->top());
    n->destruct();
  }
  for (int i = rpo_list.size() - 1; i >= 0; i--) {
    Node* c = rpo_list.at(i);
    if (c->is_Region()) {
      Node* n = regions[c->_idx];
      if (n->is_Phi() && n->_idx >= last && n->in(0) == c) {
        phase->register_new_node(n, c);
      }
    }
  }
  for (int i = rpo_list.size() - 1; i >= 0; i--) {
    Node* c = rpo_list.at(i);
    if (c->is_Region()) {
      Node* n = regions[c->_idx];
      for (DUIterator_Fast imax, i = c->fast_outs(imax); i < imax; i++) {
        Node* u = c->fast_out(i);
        if (u->is_Phi() && u->bottom_type() == Type::MEMORY &&
            u != n) {
          if (u->adr_type() == TypePtr::BOTTOM) {
            fix_memory_uses(u, n, n, c, alias, phase);
          } else if (phase->C->get_alias_index(u->adr_type()) == alias) {
            phase->lazy_replace(u, n);
            --i; --imax;
          }
        }
      }
    }
  }
}

void ShenandoahWriteBarrierNode::fix_raw_mem(Node* ctrl, Node* region, Node* raw_mem, Node* raw_mem_for_ctrl, Node* raw_mem_phi,
                                             Node_List& memory_nodes, Unique_Node_List& uses, PhaseIdealLoop* phase) {
  const bool trace = false;
  DEBUG_ONLY(if (trace) { tty->print("ZZZ control is"); ctrl->dump(); });
  DEBUG_ONLY(if (trace) { tty->print("ZZZ mem is"); raw_mem->dump(); });
  GrowableArray<Node*> phis;
  if (raw_mem_for_ctrl != raw_mem) {
    Node* old = raw_mem_for_ctrl;
    Node* prev = NULL;
    while (old != raw_mem) {
      assert(old->is_Store() || old->is_LoadStore() || old->is_ClearArray(), "");
      prev = old;
      old = old->in(MemNode::Memory);
    }
    assert(prev != NULL, "");
    memory_nodes.map(ctrl->_idx, raw_mem);
    memory_nodes.map(region->_idx, raw_mem_for_ctrl);
    phase->igvn().replace_input_of(prev, MemNode::Memory, raw_mem_phi);
  } else {
    memory_nodes.map(region->_idx, raw_mem_phi);
    uses.clear();
    uses.push(region);
    for(uint next = 0; next < uses.size(); next++ ) {
      Node *n = uses.at(next);
      assert(n->is_CFG(), "");
      DEBUG_ONLY(if (trace) { tty->print("ZZZ ctrl"); n->dump(); });
      for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
        Node* u = n->fast_out(i);
        if (!u->is_Root() && u->is_CFG() && u != n) {
          Node* m = memory_nodes[u->_idx];
          if (u->is_Region() && !has_mem_phi(phase->C, u, Compile::AliasIdxRaw)) {
            DEBUG_ONLY(if (trace) { tty->print("ZZZ region"); u->dump(); });
            DEBUG_ONLY(if (trace && m != NULL) { tty->print("ZZZ mem"); m->dump(); });

            if (!mem_is_valid(m, u, phase) || !m->is_Phi()) {
              bool push = true;
              bool create_phi = true;
              if (phase->is_dominator(region, u)) {
                create_phi = false;
              } else if (!phase->C->has_irreducible_loop()) {
                IdealLoopTree* loop = phase->get_loop(ctrl);
                bool do_check = true;
                IdealLoopTree* l = loop;
                create_phi = false;
                while (l != phase->ltree_root()) {
                  if (phase->is_dominator(l->_head, u) && phase->is_dominator(phase->idom(u), l->_head)) {
                    create_phi = true;
                    do_check = false;
                    break;
                  }
                  l = l->_parent;
                }

                if (do_check) {
                  assert(!create_phi, "");
                  IdealLoopTree* u_loop = phase->get_loop(u);
                  if (u_loop != phase->ltree_root() && u_loop->is_member(loop)) {
                    Node* c = ctrl;
                    while (!phase->is_dominator(c, u_loop->tail())) {
                      c = phase->idom(c);
                    }
                    if (!phase->is_dominator(c, u)) {
                      do_check = false;
                    }
                  }
                }

                if (do_check && phase->is_dominator(phase->idom(u), region)) {
                  create_phi = true;
                }
              }
              if (create_phi) {
                Node* phi = new (phase->C) PhiNode(u, Type::MEMORY, TypeRawPtr::BOTTOM);
                phase->register_new_node(phi, u);
                phis.push(phi);
                DEBUG_ONLY(if (trace) { tty->print("ZZZ new phi"); phi->dump(); });
                if (!mem_is_valid(m, u, phase)) {
                  DEBUG_ONLY(if (trace) { tty->print("ZZZ setting mem"); phi->dump(); });
                  memory_nodes.map(u->_idx, phi);
                } else {
                  DEBUG_ONLY(if (trace) { tty->print("ZZZ NOT setting mem"); m->dump(); });
                  for (;;) {
                    assert(m->is_Mem() || m->is_LoadStore() || m->is_Proj() /*|| m->is_MergeMem()*/, "");
                    Node* next = NULL;
                    if (m->is_Proj()) {
                      next = m->in(0);
                    } else {
                      next = m->in(MemNode::Memory);
                    }
                    if (phase->get_ctrl(next) != u) {
                      break;
                    }
                    if (next->is_MergeMem()) {
                      assert(phase->get_ctrl(next->as_MergeMem()->memory_at(Compile::AliasIdxRaw)) != u, "");
                      break;
                    }
                    if (next->is_Phi()) {
                      assert(next->adr_type() == TypePtr::BOTTOM && next->in(0) == u, "");
                      break;
                    }
                    m = next;
                  }

                  DEBUG_ONLY(if (trace) { tty->print("ZZZ setting to phi"); m->dump(); });
                  assert(m->is_Mem() || m->is_LoadStore(), "");
                  phase->igvn().replace_input_of(m, MemNode::Memory, phi);
                  push = false;
                }
              } else {
                DEBUG_ONLY(if (trace) { tty->print("ZZZ skipping region"); u->dump(); });
              }
              if (push) {
                uses.push(u);
              }
            }
          } else if (!mem_is_valid(m, u, phase) &&
                     !(u->Opcode() == Op_CProj && u->in(0)->Opcode() == Op_NeverBranch && u->as_Proj()->_con == 1)) {
            uses.push(u);
          }
        }
      }
    }
    for (int i = 0; i < phis.length(); i++) {
      Node* n = phis.at(i);
      Node* r = n->in(0);
      DEBUG_ONLY(if (trace) { tty->print("ZZZ fixing new phi"); n->dump(); });
      for (uint j = 1; j < n->req(); j++) {
        Node* m = find_raw_mem(r->in(j), NULL, memory_nodes, phase);
        phase->igvn().replace_input_of(n, j, m);
        DEBUG_ONLY(if (trace) { tty->print("ZZZ fixing new phi: %d", j); m->dump(); });
      }
    }
  }
  uint last = phase->C->unique();
  MergeMemNode* mm = NULL;
  int alias = Compile::AliasIdxRaw;
  DEBUG_ONLY(if (trace) { tty->print("ZZZ raw mem is"); raw_mem->dump(); });
  for (DUIterator i = raw_mem->outs(); raw_mem->has_out(i); i++) {
    Node* u = raw_mem->out(i);
    if (u->_idx < last) {
      if (u->is_Mem()) {
        if (phase->C->get_alias_index(u->adr_type()) == alias) {
          Node* m = find_raw_mem(phase->get_ctrl(u), u, memory_nodes, phase);
          if (m != raw_mem) {
            DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of use"); u->dump(); });
            phase->igvn().replace_input_of(u, MemNode::Memory, m);
            --i;
          }
        }
      } else if (u->is_MergeMem()) {
        MergeMemNode* u_mm = u->as_MergeMem();
        if (u_mm->memory_at(alias) == raw_mem) {
          MergeMemNode* newmm = NULL;
          for (DUIterator_Fast jmax, j = u->fast_outs(jmax); j < jmax; j++) {
            Node* uu = u->fast_out(j);
            assert(!uu->is_MergeMem(), "chain of MergeMems?");
            if (uu->is_Phi()) {
              assert(uu->adr_type() == TypePtr::BOTTOM, "");
              Node* region = uu->in(0);
              int nb = 0;
              for (uint k = 1; k < uu->req(); k++) {
                if (uu->in(k) == u) {
                  Node* m = find_raw_mem(region->in(k), NULL, memory_nodes, phase);
                  if (m != raw_mem) {
                    DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of phi %d", k); uu->dump(); });
                    if (newmm == NULL || 1) {
                      newmm = clone_merge_mem(u, raw_mem, alias, m, phase->ctrl_or_self(m), i, phase);
                    }
                    if (newmm != u) {
                      phase->igvn().replace_input_of(uu, k, newmm);
                      nb++;
                      --jmax;
                    }
                  }
                }
              }
              if (nb > 0) {
                --j;
              }
            } else {
              Node* m = find_raw_mem(phase->ctrl_or_self(uu), uu, memory_nodes, phase);
              if (m != raw_mem) {
                DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of use"); uu->dump(); });
                if (newmm == NULL || 1) {
                  newmm = clone_merge_mem(u, raw_mem, alias, m, phase->ctrl_or_self(m), i, phase);
                }
                if (newmm != u) {
                  phase->igvn().replace_input_of(uu, uu->find_edge(u), newmm);
                  --j, --jmax;
                }
              }
            }
          }
        }
      } else if (u->is_Phi()) {
        assert(u->bottom_type() == Type::MEMORY, "what else?");
        if (u->adr_type() == TypeRawPtr::BOTTOM || u->adr_type() == TypePtr::BOTTOM) {
          Node* region = u->in(0);
          bool replaced = false;
          for (uint j = 1; j < u->req(); j++) {
            if (u->in(j) == raw_mem) {
              Node* m = find_raw_mem(region->in(j), NULL, memory_nodes, phase);
              Node* nnew = m;
              if (m != raw_mem) {
                if (u->adr_type() == TypePtr::BOTTOM) {
                  if (mm == NULL || 1) {
                    mm = allocate_merge_mem(raw_mem, alias, m, phase->ctrl_or_self(m), phase);
                  }
                  nnew = mm;
                }
                DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of phi %d", j); u->dump(); });
                phase->igvn().replace_input_of(u, j, nnew);
                replaced = true;
              }
            }
          }
          if (replaced) {
            --i;
          }
        }
      } else if (u->adr_type() == TypePtr::BOTTOM || u->adr_type() == NULL) {
        assert(u->adr_type() != NULL ||
               u->Opcode() == Op_Rethrow ||
               u->Opcode() == Op_Return ||
               u->Opcode() == Op_SafePoint ||
               (u->is_CallStaticJava() && u->as_CallStaticJava()->uncommon_trap_request() != 0) ||
               (u->is_CallStaticJava() && u->as_CallStaticJava()->_entry_point == OptoRuntime::rethrow_stub()) ||
               u->Opcode() == Op_CallLeaf, "");
        Node* m = find_raw_mem(phase->ctrl_or_self(u), u, memory_nodes, phase);
        if (m != raw_mem) {
          if (mm == NULL || 1) {
            mm = allocate_merge_mem(raw_mem, alias, m, phase->get_ctrl(m), phase);
          }
          phase->igvn().replace_input_of(u, u->find_edge(raw_mem), mm);
          --i;
        }
      } else if (phase->C->get_alias_index(u->adr_type()) == alias) {
        Node* m = find_raw_mem(phase->ctrl_or_self(u), u, memory_nodes, phase);
        if (m != raw_mem) {
          DEBUG_ONLY(if (trace) { tty->print("ZZZ setting memory of use"); u->dump(); });
          phase->igvn().replace_input_of(u, u->find_edge(raw_mem), m);
          --i;
        }
      }
    }
  }
#ifdef ASSERT
  assert(raw_mem_phi->outcnt() > 0, "");
  for (int i = 0; i < phis.length(); i++) {
    Node* n = phis.at(i);
    assert(n->outcnt() > 0, "new phi must have uses now");
  }
#endif
}

bool ShenandoahBarrierNode::is_evacuation_in_progress_test(Node* iff) {
  if (!UseShenandoahGC) {
    return false;
  }

  assert(iff->is_If(), "bad input");
  if (iff->Opcode() != Op_If) {
    return false;
  }
  Node* bol = iff->in(1);
  if (!bol->is_Bool() || bol->as_Bool()->_test._test != BoolTest::ne) {
    return false;
  }
  Node* cmp = bol->in(1);
  if (cmp->Opcode() != Op_CmpI) {
    return false;
  }
  Node* in1 = cmp->in(1);
  Node* in2 = cmp->in(2);
  if (in2->find_int_con(-1) != 0) {
    return false;
  }
  if (in1->Opcode() != Op_AndI) {
    return false;
  }
  in2 = in1->in(2);
  if (in2->find_int_con(-1) != ShenandoahHeap::EVACUATION) {
    return false;
  }
  in1 = in1->in(1);

  return is_gc_state_load(in1);
}

bool ShenandoahBarrierNode::is_gc_state_load(Node *n) {
  if (!UseShenandoahGC) {
    return false;
  }

  if (n->Opcode() != Op_LoadB) {
    return false;
  }
  Node* addp = n->in(MemNode::Address);
  if (!addp->is_AddP()) {
    return false;
  }
  Node* base = addp->in(AddPNode::Address);
  Node* off = addp->in(AddPNode::Offset);
  if (base->Opcode() != Op_ThreadLocal) {
    return false;
  }
  if (off->find_intptr_t_con(-1) != in_bytes(JavaThread::gc_state_offset())) {
    return false;
  }
  return true;
}


void PhaseIdealLoop::shenandoah_test_evacuation_in_progress(Node* ctrl, int alias, Node*& raw_mem, Node*& wb_mem,
                                                            IfNode*& evacuation_iff, Node*& evac_in_progress,
                                                            Node*& evac_not_in_progress) {
  IdealLoopTree *loop = get_loop(ctrl);
  Node* thread = new (C) ThreadLocalNode();
  register_new_node(thread, ctrl);
  Node* offset = _igvn.MakeConX(in_bytes(JavaThread::gc_state_offset()));
  set_ctrl(offset, C->root());
  Node* gc_state_addr = new (C) AddPNode(C->top(), thread, offset);
  register_new_node(gc_state_addr, ctrl);
  uint gc_state_idx = Compile::AliasIdxRaw;
  const TypePtr* gc_state_adr_type = NULL; // debug-mode-only argument
  debug_only(gc_state_adr_type = C->get_adr_type(gc_state_idx));

  Node* gc_state = new (C) LoadBNode(ctrl, raw_mem, gc_state_addr, gc_state_adr_type, TypeInt::BYTE, MemNode::unordered);
  register_new_node(gc_state, ctrl);

  Node* evacuation_in_progress = new (C) AndINode(gc_state, _igvn.intcon(ShenandoahHeap::EVACUATION));
  register_new_node(evacuation_in_progress, ctrl);
  Node* evacuation_in_progress_cmp = new (C) CmpINode(evacuation_in_progress, _igvn.zerocon(T_INT));
  register_new_node(evacuation_in_progress_cmp, ctrl);
  Node* evacuation_in_progress_test = new (C) BoolNode(evacuation_in_progress_cmp, BoolTest::ne);
  register_new_node(evacuation_in_progress_test, ctrl);
  evacuation_iff = new (C) IfNode(ctrl, evacuation_in_progress_test, PROB_UNLIKELY(0.999), COUNT_UNKNOWN);
  register_control(evacuation_iff, loop, ctrl);

  assert(ShenandoahBarrierNode::is_evacuation_in_progress_test(evacuation_iff), "Should match the shape");
  assert(ShenandoahBarrierNode::is_gc_state_load(gc_state), "Should match the shape");

  evac_not_in_progress = new (C) IfFalseNode(evacuation_iff);
  register_control(evac_not_in_progress, loop, evacuation_iff);
  evac_in_progress = new (C) IfTrueNode(evacuation_iff);
  register_control(evac_in_progress, loop, evacuation_iff);
}

void PhaseIdealLoop::shenandoah_evacuation_not_in_progress_null_check(Node*& c, Node*& val, Node* unc_ctrl, Node*& unc_region) {
  if (unc_ctrl != NULL) {
    // Clone the null check in this branch to allow implicit null check
    IdealLoopTree *loop = get_loop(c);
    Node* iff = unc_ctrl->in(0);
    assert(iff->is_If(), "broken");
    Node* new_iff = iff->clone();
    new_iff->set_req(0, c);
    register_control(new_iff, loop, c);
    Node* iffalse = new (C) IfFalseNode(new_iff->as_If());
    register_control(iffalse, loop, new_iff);
    Node* iftrue = new (C) IfTrueNode(new_iff->as_If());
    register_control(iftrue, loop, new_iff);
    c = iftrue;
    unc_region = new (C) RegionNode(3);
    unc_region->init_req(1, iffalse);
    const Type *t = _igvn.type(val);
    assert(val->Opcode() == Op_CastPP, "expect cast to non null here");
    Node* uncasted_val = val->in(1);
    val = new (C) CastPPNode(uncasted_val, t);
    val->init_req(0, c);
    register_new_node(val, c);
  }
}

void PhaseIdealLoop::shenandoah_evacuation_not_in_progress(Node* c, Node* val, Node* unc_ctrl, Node* raw_mem, Node* wb_mem, Node* region,
                                                           Node* val_phi, Node* mem_phi, Node* raw_mem_phi, Node*& unc_region) {
  shenandoah_evacuation_not_in_progress_null_check(c, val, unc_ctrl, unc_region);
  region->init_req(1, c);
  if (ShenandoahWriteBarrierRB) {
    Node *rbfalse = new(C) ShenandoahReadBarrierNode(c, wb_mem, val);
    register_new_node(rbfalse, c);
    val_phi->init_req(1, rbfalse);
  } else {
    val_phi->init_req(1, val);
  }
  mem_phi->init_req(1, wb_mem);
  raw_mem_phi->init_req(1, raw_mem);
}

void PhaseIdealLoop::shenandoah_evacuation_in_progress_null_check(Node*& c, Node*& val, Node* evacuation_iff, Node* unc, Node* unc_ctrl,
                                                                  Node* unc_region, Unique_Node_List& uses) {
  if (unc != NULL) {
    // Clone the null check in this branch to allow implicit null check
    IdealLoopTree *loop = get_loop(c);
    Node* iff = unc_ctrl->in(0);
    assert(iff->is_If(), "broken");
    Node* new_iff = iff->clone();
    new_iff->set_req(0, c);
    register_control(new_iff, loop, c);
    Node* iffalse = new (C) IfFalseNode(new_iff->as_If());
    register_control(iffalse, loop, new_iff);
    Node* iftrue = new (C) IfTrueNode(new_iff->as_If());
    register_control(iftrue, loop, new_iff);
    c = iftrue;
    unc_region->init_req(2, iffalse);

    Node* proj = iff->as_If()->proj_out(0);
    assert(proj != unc_ctrl, "bad projection");
    Node* use = proj->unique_ctrl_out();

    assert(use == unc || use->is_Region(), "what else?");

    uses.clear();
    if (use == unc) {
      set_idom(use, unc_region, dom_depth(unc_region)+1);
      for (uint i = 1; i < unc->req(); i++) {
        Node* n = unc->in(i);
        if (has_ctrl(n) && get_ctrl(n) == proj) {
          uses.push(n);
        }
      }
    } else {
      assert(use->is_Region(), "what else?");
      uint idx = 1;
      for (; use->in(idx) != proj; idx++);
      for (DUIterator_Fast imax, i = use->fast_outs(imax); i < imax; i++) {
        Node* u = use->fast_out(i);
        if (u->is_Phi() && get_ctrl(u->in(idx)) == proj) {
          uses.push(u->in(idx));
        }
      }
    }
    for(uint next = 0; next < uses.size(); next++ ) {
      Node *n = uses.at(next);
      assert(get_ctrl(n) == proj, "bad control");
      set_ctrl_and_loop(n, unc_region);
      if (n->in(0) == proj) {
        _igvn.replace_input_of(n, 0, unc_region);
      }
      for (uint i = 0; i < n->req(); i++) {
        Node* m = n->in(i);
        if (m != NULL && has_ctrl(m) && get_ctrl(m) == proj) {
          uses.push(m);
        }
      }
    }

    _igvn.rehash_node_delayed(use);
    int nb = use->replace_edge(proj, unc_region);
    assert(nb == 1, "only use expected");
    register_control(unc_region, _ltree_root, evacuation_iff);

    _igvn.replace_input_of(iff, 1, _igvn.intcon(1));
    const Type *t = _igvn.type(val);
    assert(val->Opcode() == Op_CastPP, "expect cast to non null here");
    Node* uncasted_val = val->in(1);
    val = new (C) CastPPNode(uncasted_val, t);
    val->init_req(0, c);
    register_new_node(val, c);
  }
}

void PhaseIdealLoop::shenandoah_in_cset_fast_test(Node*& c, Node* rbtrue, Node* raw_mem, Node* wb_mem, Node* region, Node* val_phi, Node* mem_phi,
                                                  Node* raw_mem_phi) {
  if (ShenandoahWriteBarrierCsetTestInIR) {
    IdealLoopTree *loop = get_loop(c);
    Node* raw_rbtrue = new (C) CastP2XNode(c, rbtrue);
    register_new_node(raw_rbtrue, c);
    Node* cset_offset = new (C) URShiftXNode(raw_rbtrue, _igvn.intcon(ShenandoahHeapRegion::region_size_bytes_shift_jint()));
    register_new_node(cset_offset, c);
    Node* in_cset_fast_test_base_addr = _igvn.makecon(TypeRawPtr::make(ShenandoahHeap::in_cset_fast_test_addr()));
    set_ctrl(in_cset_fast_test_base_addr, C->root());
    Node* in_cset_fast_test_adr = new (C) AddPNode(C->top(), in_cset_fast_test_base_addr, cset_offset);
    register_new_node(in_cset_fast_test_adr, c);
    uint in_cset_fast_test_idx = Compile::AliasIdxRaw;
    const TypePtr* in_cset_fast_test_adr_type = NULL; // debug-mode-only argument
    debug_only(in_cset_fast_test_adr_type = C->get_adr_type(in_cset_fast_test_idx));
    Node* in_cset_fast_test_load = new (C) LoadBNode(c, raw_mem, in_cset_fast_test_adr, in_cset_fast_test_adr_type, TypeInt::BOOL, MemNode::unordered);
    register_new_node(in_cset_fast_test_load, c);
    Node* in_cset_fast_test_cmp = new (C) CmpINode(in_cset_fast_test_load, _igvn.zerocon(T_INT));
    register_new_node(in_cset_fast_test_cmp, c);
    Node* in_cset_fast_test_test = new (C) BoolNode(in_cset_fast_test_cmp, BoolTest::ne);
    register_new_node(in_cset_fast_test_test, c);
    IfNode* in_cset_fast_test_iff = new (C) IfNode(c, in_cset_fast_test_test, PROB_UNLIKELY(0.999), COUNT_UNKNOWN);
    register_control(in_cset_fast_test_iff, loop, c);

    Node* in_cset_fast_test_success = new (C) IfFalseNode(in_cset_fast_test_iff);
    register_control(in_cset_fast_test_success, loop, in_cset_fast_test_iff);

    region->init_req(3, in_cset_fast_test_success);
    val_phi->init_req(3, rbtrue);
    mem_phi->init_req(3, wb_mem);
    raw_mem_phi->init_req(3, raw_mem);

    Node* in_cset_fast_test_failure = new (C) IfTrueNode(in_cset_fast_test_iff);
    register_control(in_cset_fast_test_failure, loop, in_cset_fast_test_iff);

    c = in_cset_fast_test_failure;
  }
}

void PhaseIdealLoop::shenandoah_evacuation_in_progress(Node* c, Node* val, Node* evacuation_iff, Node* unc, Node* unc_ctrl,
                                                       Node* raw_mem, Node* wb_mem, Node* region, Node* val_phi, Node* mem_phi,
                                                       Node* raw_mem_phi, Node* unc_region, int alias, Unique_Node_List& uses) {
  shenandoah_evacuation_in_progress_null_check(c, val, evacuation_iff, unc, unc_ctrl, unc_region, uses);

  IdealLoopTree *loop = get_loop(c);

  Node* rbtrue;
  if (ShenandoahWriteBarrierRB) {
    rbtrue = new (C) ShenandoahReadBarrierNode(c, wb_mem, val);
    register_new_node(rbtrue, c);
  } else {
    rbtrue = val;
  }

  Node* in_cset_fast_test_failure = NULL;
  shenandoah_in_cset_fast_test(c, rbtrue, raw_mem, wb_mem, region, val_phi, mem_phi, raw_mem_phi);

  // The slow path stub consumes and produces raw memory in addition
  // to the existing memory edges
  Node* base = shenandoah_find_bottom_mem(c);

  MergeMemNode* mm = MergeMemNode::make(C, base);
  mm->set_memory_at(alias, wb_mem);
  mm->set_memory_at(Compile::AliasIdxRaw, raw_mem);
  register_new_node(mm, c);

  Node* call = new (C) CallLeafNoFPNode(OptoRuntime::shenandoah_write_barrier_Type(), StubRoutines::shenandoah_wb_C(), "shenandoah_write_barrier", TypeRawPtr::BOTTOM);
  call->init_req(TypeFunc::Control, c);
  call->init_req(TypeFunc::I_O, C->top());
  call->init_req(TypeFunc::Memory, mm);
  call->init_req(TypeFunc::FramePtr, C->top());
  call->init_req(TypeFunc::ReturnAdr, C->top());
  call->init_req(TypeFunc::Parms, rbtrue);
  register_control(call, loop, c);
  Node* ctrl_proj = new (C) ProjNode(call, TypeFunc::Control);
  register_control(ctrl_proj, loop, call);
  Node* mem_proj = new (C) ProjNode(call, TypeFunc::Memory);
  register_new_node(mem_proj, call);
  Node* res_proj = new (C) ProjNode(call, TypeFunc::Parms);
  register_new_node(res_proj, call);
  Node* res = new (C) CheckCastPPNode(ctrl_proj, res_proj, _igvn.type(val)->is_oopptr()->cast_to_nonconst());
  register_new_node(res, ctrl_proj);
  region->init_req(2, ctrl_proj);
  val_phi->init_req(2, res);
  mem_phi->init_req(2, mem_proj);
  raw_mem_phi->init_req(2, mem_proj);
  register_control(region, loop, evacuation_iff);

}

void PhaseIdealLoop::shenandoah_pin_and_expand_barriers() {
  const bool trace = false;

  // Collect raw memory state at CFG points in the entire graph and
  // record it in memory_nodes. Optimize the raw memory graph in the
  // process. Optimizing the memory graph also makes the memory graph
  // simpler.
  Node_List memory_nodes;
  ShenandoahWriteBarrierNode::collect_memory_nodes(Compile::AliasIdxRaw, memory_nodes, this);

  // Let's try to common write barriers again
  for (int i = C->shenandoah_barriers_count(); i > 0; i--) {
    ShenandoahBarrierNode* wb = C->shenandoah_barrier(i-1);
    Node* ctrl = get_ctrl(wb);
    try_common_shenandoah_barriers(wb, ctrl);
  }

  for (int i = 0; i < C->shenandoah_barriers_count(); i++) {
    ShenandoahBarrierNode* wb = C->shenandoah_barrier(i);
    Node* ctrl = get_ctrl(wb);

    Node* val = wb->in(ShenandoahBarrierNode::ValueIn);
    if (ctrl->is_Proj() && ctrl->in(0)->is_CallJava()) {
      assert(ShenandoahBarrierNode::is_dominator(get_ctrl(val), ctrl->in(0)->in(0), val, ctrl->in(0), this), "can't move");
      set_ctrl(wb, ctrl->in(0)->in(0));
    } else if (ctrl->is_CallRuntime()) {
      assert(ShenandoahBarrierNode::is_dominator(get_ctrl(val), ctrl->in(0), val, ctrl, this), "can't move");
      set_ctrl(wb, ctrl->in(0));
    }

    assert(wb->Opcode() == Op_ShenandoahWriteBarrier, "only for write barriers");
    // Look for a null check that dominates this barrier and move the
    // barrier right after the null check to enable implicit null
    // checks
    shenandoah_pin_and_expand_barriers_move_barrier(wb);

    ctrl = get_ctrl(wb);
  }

  Unique_Node_List uses;
  Unique_Node_List uses_to_ignore;
  for (int i = C->shenandoah_barriers_count(); i > 0; i--) {
    int cnt = C->shenandoah_barriers_count();
    ShenandoahBarrierNode* wb = C->shenandoah_barrier(i-1);
    assert(wb->Opcode() == Op_ShenandoahWriteBarrier, "only for write barriers");

    uint last = C->unique();
    Node* ctrl = get_ctrl(wb);

    Node* raw_mem = ShenandoahWriteBarrierNode::find_raw_mem(ctrl, wb, memory_nodes, this);
    Node* init_raw_mem = raw_mem;
    Node* raw_mem_for_ctrl = ShenandoahWriteBarrierNode::find_raw_mem(ctrl, NULL, memory_nodes, this);
    int alias = C->get_alias_index(wb->adr_type());
    Node* wb_mem =  wb->in(ShenandoahBarrierNode::Memory);

    Node* val = wb->in(ShenandoahBarrierNode::ValueIn);
    Node* wbproj = wb->find_out_with(Op_ShenandoahWBMemProj);
    IdealLoopTree *loop = get_loop(ctrl);

    assert(val->Opcode() != Op_ShenandoahWriteBarrier || C->has_irreducible_loop(), "No chain of write barriers");

    CallStaticJavaNode* unc = shenandoah_pin_and_expand_barriers_null_check(wb);
    Node* unc_ctrl = NULL;
    if (unc != NULL) {
      if (val->in(0) != ctrl) {
        unc = NULL;
      } else {
        unc_ctrl = val->in(0);
      }
    }

    Node* uncasted_val = val;
    if (unc != NULL) {
      uncasted_val = val->in(1);
    }

    Node* evac_in_progress = NULL;
    Node* evac_not_in_progress = NULL;
    IfNode* evacuation_iff = NULL;
    shenandoah_test_evacuation_in_progress(ctrl, alias, raw_mem, wb_mem, evacuation_iff, evac_in_progress, evac_not_in_progress);

    Node* region = new (C) RegionNode(4);
    Node* val_phi = new (C) PhiNode(region, val->bottom_type()->is_oopptr()->cast_to_nonconst());
    Node* mem_phi = PhiNode::make(region, wb_mem, Type::MEMORY, C->alias_type(wb->adr_type())->adr_type());
    Node* raw_mem_phi = PhiNode::make(region, raw_mem, Type::MEMORY, TypeRawPtr::BOTTOM);

    Node* unc_region = NULL;
    shenandoah_evacuation_not_in_progress(evac_not_in_progress, val, unc_ctrl, raw_mem, wb_mem,
                                          region, val_phi, mem_phi, raw_mem_phi, unc_region);

    shenandoah_evacuation_in_progress(evac_in_progress, val, evacuation_iff, unc, unc_ctrl,
                                      raw_mem, wb_mem, region, val_phi, mem_phi, raw_mem_phi,
                                      unc_region, alias, uses);
    Node* out_val = val_phi;
    register_new_node(val_phi, region);
    register_new_node(mem_phi, region);
    register_new_node(raw_mem_phi, region);

    // Update the control of all nodes that should be after the
    // barrier control flow
    uses.clear();
    // Every node that is control dependent on the barrier's input
    // control will be after the expanded barrier. The raw memory (if
    // its memory is control dependent on the barrier's input control)
    // must stay above the barrier.
    uses_to_ignore.clear();
    if (has_ctrl(init_raw_mem) && get_ctrl(init_raw_mem) == ctrl && !init_raw_mem->is_Phi()) {
      uses_to_ignore.push(init_raw_mem);
    }
    for (uint next = 0; next < uses_to_ignore.size(); next++) {
      Node *n = uses_to_ignore.at(next);
      for (uint i = 0; i < n->req(); i++) {
        Node* in = n->in(i);
        if (in != NULL && has_ctrl(in) && get_ctrl(in) == ctrl) {
          uses_to_ignore.push(in);
        }
      }
    }
    for (DUIterator_Fast imax, i = ctrl->fast_outs(imax); i < imax; i++) {
      Node* u = ctrl->fast_out(i);
      if (u->_idx < last &&
          u != wb &&
          !uses_to_ignore.member(u) &&
          (u->in(0) != ctrl || (!u->is_Region() && !u->is_Phi())) &&
          (ctrl->Opcode() != Op_CatchProj || u->Opcode() != Op_CreateEx)) {
        Node* old_c = ctrl_or_self(u);
        Node* c = old_c;
        if (c != ctrl ||
            ShenandoahBarrierNode::is_dominator_same_ctrl(old_c, wb, u, this) ||
            u->is_g1_marking_load()) {
          _igvn.rehash_node_delayed(u);
          int nb = u->replace_edge(ctrl, region);
          if (u->is_CFG()) {
            if (idom(u) == ctrl) {
              set_idom(u, region, dom_depth(region));
            }
          } else if (get_ctrl(u) == ctrl) {
            assert(u != init_raw_mem, "should leave input raw mem above the barrier");
            uses.push(u);
          }
          assert(nb == 1, "more than 1 ctrl input?");
          --i, imax -= nb;
        }
      }
    }

    if (wbproj != NULL) {
      _igvn.replace_input_of(wbproj, 0, C->top());
      lazy_replace(wbproj, mem_phi);
    }
    if (unc != NULL) {
      for (DUIterator_Fast imax, i = val->fast_outs(imax); i < imax; i++) {
        Node* u = val->fast_out(i);
        Node* c = ctrl_or_self(u);
        if (u != wb && (c != ctrl || ShenandoahBarrierNode::is_dominator_same_ctrl(c, wb, u, this))) {
          _igvn.rehash_node_delayed(u);
          int nb = u->replace_edge(val, out_val);
          --i, imax -= nb;
        }
      }
      if (val->outcnt() == 0) {
        lazy_update(val, out_val);
        _igvn._worklist.push(val);
      }
    }
    lazy_replace(wb, out_val);

    shenandoah_follow_barrier_uses(mem_phi, ctrl, uses);
    shenandoah_follow_barrier_uses(out_val, ctrl, uses);

    for(uint next = 0; next < uses.size(); next++ ) {
      Node *n = uses.at(next);
      assert(get_ctrl(n) == ctrl, "bad control");
      assert(n != init_raw_mem, "should leave input raw mem above the barrier");
      set_ctrl(n, region);
      shenandoah_follow_barrier_uses(n, ctrl, uses);
    }

    recompute_dom_depth();

    // The slow path call produces memory: hook the raw memory phi
    // from the expanded write barrier with the rest of the graph
    // which may require adding memory phis at every post dominated
    // region and at enclosing loop heads. Use the memory state
    // collected in memory_nodes to fix the memory graph. Update that
    // memory state as we go.
    ShenandoahWriteBarrierNode::fix_raw_mem(ctrl, region, init_raw_mem, raw_mem_for_ctrl, raw_mem_phi, memory_nodes, uses, this);
    assert(C->shenandoah_barriers_count() == cnt - 1, "not replaced");
  }

  assert(C->shenandoah_barriers_count() == 0, "all write barrier nodes should have been replaced");
}

#ifdef ASSERT
void ShenandoahBarrierNode::verify_raw_mem(RootNode* root) {
  const bool trace = false;
  ResourceMark rm;
  Unique_Node_List nodes;
  Unique_Node_List controls;
  Unique_Node_List memories;

  nodes.push(root);
  for (uint next = 0; next < nodes.size(); next++) {
    Node *n  = nodes.at(next);
    if (n->Opcode() == Op_CallLeafNoFP && n->as_Call()->_entry_point == StubRoutines::shenandoah_wb_C()) {
      controls.push(n);
      if (trace) { tty->print("XXXXXX verifying"); n->dump(); }
      for (uint next2 = 0; next2 < controls.size(); next2++) {
        Node *m = controls.at(next2);
        if (!m->is_Loop() || controls.member(m->in(LoopNode::EntryControl)) || 1) {
          for (DUIterator_Fast imax, i = m->fast_outs(imax); i < imax; i++) {
            Node* u = m->fast_out(i);
            if (u->is_CFG() && !u->is_Root() &&
                !(u->Opcode() == Op_CProj && u->in(0)->Opcode() == Op_NeverBranch && u->as_Proj()->_con == 1)) {
              if (trace) { tty->print("XXXXXX pushing control"); u->dump(); }
              controls.push(u);
            }
          }
        }
      }
      memories.push(n->as_Call()->proj_out(TypeFunc::Memory));
      for (uint next2 = 0; next2 < memories.size(); next2++) {
        Node *m = memories.at(next2);
        assert(m->bottom_type() == Type::MEMORY, "");
        if (!m->is_Phi() || !m->in(0)->is_Loop() || controls.member(m->in(0)->in(LoopNode::EntryControl)) || 1) {
          for (DUIterator_Fast imax, i = m->fast_outs(imax); i < imax; i++) {
            Node* u = m->fast_out(i);
            if (u->bottom_type() == Type::MEMORY && (u->is_Mem() || u->is_ClearArray())) {
              if (trace) { tty->print("XXXXXX pushing memory"); u->dump(); }
              memories.push(u);
            } else if (u->is_LoadStore()) {
              if (trace) { tty->print("XXXXXX pushing memory"); u->find_out_with(Op_SCMemProj)->dump(); }
              memories.push(u->find_out_with(Op_SCMemProj));
            } else if (u->is_MergeMem() && u->as_MergeMem()->memory_at(Compile::AliasIdxRaw) == m) {
              if (trace) { tty->print("XXXXXX pushing memory"); u->dump(); }
              memories.push(u);
            } else if (u->is_Phi()) {
              assert(u->bottom_type() == Type::MEMORY, "");
              if (u->adr_type() == TypeRawPtr::BOTTOM || u->adr_type() == TypePtr::BOTTOM) {
                assert(controls.member(u->in(0)), "");
                if (trace) { tty->print("XXXXXX pushing memory"); u->dump(); }
                memories.push(u);
              }
            } else if (u->is_SafePoint() || u->is_MemBar()) {
              for (DUIterator_Fast jmax, j = u->fast_outs(jmax); j < jmax; j++) {
                Node* uu = u->fast_out(j);
                if (uu->bottom_type() == Type::MEMORY) {
                  if (trace) { tty->print("XXXXXX pushing memory"); uu->dump(); }
                  memories.push(uu);
                }
              }
            }
          }
        }
      }
      for (uint next2 = 0; next2 < controls.size(); next2++) {
        Node *m = controls.at(next2);
        if (m->is_Region()) {
          bool all_in = true;
          for (uint i = 1; i < m->req(); i++) {
            if (!controls.member(m->in(i))) {
              all_in = false;
              break;
            }
          }
          if (trace) { tty->print("XXX verifying %s", all_in ? "all in" : ""); m->dump(); }
          bool found_phi = false;
          for (DUIterator_Fast jmax, j = m->fast_outs(jmax); j < jmax && !found_phi; j++) {
            Node* u = m->fast_out(j);
            if (u->is_Phi() && memories.member(u)) {
              found_phi = true;
              for (uint i = 1; i < u->req() && found_phi; i++) {
                Node* k = u->in(i);
                if (memories.member(k) != controls.member(m->in(i))) {
                  found_phi = false;
                }
              }
            }
          }
          assert(found_phi || all_in, "");
        }
      }
      controls.clear();
      memories.clear();
    }
    for( uint i = 0; i < n->len(); ++i ) {
      Node *m = n->in(i);
      if (m != NULL) {
        nodes.push(m);
      }
    }
  }
}
#endif
