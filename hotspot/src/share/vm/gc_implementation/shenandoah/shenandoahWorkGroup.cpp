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

#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkGroup.hpp"
#include "gc_implementation/shenandoah/shenandoahLogging.hpp"

ShenandoahWorkerScope::ShenandoahWorkerScope(ShenandoahWorkGang* workers, uint nworkers, const char* msg, bool check) :
  _n_workers(nworkers),
  _workers(workers) {
  assert(msg != NULL, "Missing message");
  log_info(gc, task)("Using %u of %u workers for %s",
                     nworkers, ShenandoahHeap::heap()->max_workers(), msg);

  if (check) {
    ShenandoahHeap::heap()->assert_gc_workers(nworkers);
  }
  _workers->set_active_workers(nworkers);
}

ShenandoahWorkerScope::~ShenandoahWorkerScope() {
  assert(_workers->active_workers() == _n_workers,
    "Active workers can not be changed within this scope");
}

ShenandoahPushWorkerScope::ShenandoahPushWorkerScope(ShenandoahWorkGang* workers, uint nworkers, bool check) :
  _n_workers(nworkers),
  _old_workers(workers->active_workers()),
  _workers(workers) {
  _workers->set_active_workers(nworkers);

  // bypass concurrent/parallel protocol check for non-regular paths, e.g. verifier, etc.
  if (check) {
    ShenandoahHeap::heap()->assert_gc_workers(nworkers);
  }
}

ShenandoahPushWorkerScope::~ShenandoahPushWorkerScope() {
  assert(_workers->active_workers() == _n_workers,
    "Active workers can not be changed within this scope");
  // Restore old worker value
  _workers->set_active_workers(_old_workers);
}
