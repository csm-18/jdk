/*
 * Copyright (c) 2013, 2021, Red Hat, Inc. All rights reserved.
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
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahConcurrentGC.hpp"
#include "gc/shenandoah/shenandoahControlThread.hpp"
#include "gc/shenandoah/shenandoahDegeneratedGC.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahFullGC.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "gc/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahMark.inline.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahOldGC.hpp"
#include "gc/shenandoah/shenandoahRootProcessor.inline.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVMOperations.hpp"
#include "gc/shenandoah/shenandoahWorkerPolicy.hpp"
#include "gc/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "memory/iterator.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/metaspaceStats.hpp"
#include "memory/universe.hpp"
#include "runtime/atomic.hpp"

ShenandoahControlThread::ShenandoahControlThread() :
  ConcurrentGCThread(),
  _alloc_failure_waiters_lock(Mutex::safepoint - 1, "ShenandoahAllocFailureGC_lock", true),
  _gc_waiters_lock(Mutex::safepoint - 1, "ShenandoahRequestedGC_lock", true),
  _control_lock(Mutex::nosafepoint - 1, "ShenandoahControlGC_lock", true),
  _periodic_task(this),
  _requested_gc_cause(GCCause::_no_cause_specified),
  _requested_generation(GenerationMode::GLOBAL),
  _degen_point(ShenandoahGC::_degenerated_outside_cycle),
  _degen_generation(NULL),
  _allocs_seen(0),
  _mode(none) {

  reset_gc_id();
  create_and_start();
  _periodic_task.enroll();
  if (ShenandoahPacing) {
    _periodic_pacer_notify_task.enroll();
  }
}

ShenandoahControlThread::~ShenandoahControlThread() {
  // This is here so that super is called.
}

void ShenandoahPeriodicTask::task() {
  _thread->handle_force_counters_update();
  _thread->handle_counters_update();
}

void ShenandoahPeriodicPacerNotify::task() {
  assert(ShenandoahPacing, "Should not be here otherwise");
  ShenandoahHeap::heap()->pacer()->notify_waiters();
}

void ShenandoahControlThread::run_service() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  GCMode default_mode = concurrent_normal;
  GenerationMode generation = GLOBAL;
  GCCause::Cause default_cause = GCCause::_shenandoah_concurrent_gc;

  double last_shrink_time = os::elapsedTime();
  uint age_period = 0;

  // Shrink period avoids constantly polling regions for shrinking.
  // Having a period 10x lower than the delay would mean we hit the
  // shrinking with lag of less than 1/10-th of true delay.
  // ShenandoahUncommitDelay is in msecs, but shrink_period is in seconds.
  double shrink_period = (double)ShenandoahUncommitDelay / 1000 / 10;

  ShenandoahCollectorPolicy* policy = heap->shenandoah_policy();

  // Heuristics are notified of allocation failures here and other outcomes
  // of the cycle. They're also used here to control whether the Nth consecutive
  // degenerated cycle should be 'promoted' to a full cycle. The decision to
  // trigger a cycle or not is evaluated on the regulator thread.
  ShenandoahHeuristics* global_heuristics = heap->global_generation()->heuristics();
  while (!in_graceful_shutdown() && !should_terminate()) {
    // Figure out if we have pending requests.
    bool alloc_failure_pending = _alloc_failure_gc.is_set();
    bool is_gc_requested = _gc_requested.is_set();
    GCCause::Cause requested_gc_cause = _requested_gc_cause;
    bool explicit_gc_requested = is_gc_requested && is_explicit_gc(requested_gc_cause);
    bool implicit_gc_requested = is_gc_requested && is_implicit_gc(requested_gc_cause);

    // This control loop iteration have seen this much allocations.
    size_t allocs_seen = Atomic::xchg(&_allocs_seen, (size_t)0, memory_order_relaxed);

    // Check if we have seen a new target for soft max heap size.
    bool soft_max_changed = check_soft_max_changed();

    // Choose which GC mode to run in. The block below should select a single mode.
    set_gc_mode(none);
    GCCause::Cause cause = GCCause::_last_gc_cause;
    ShenandoahGC::ShenandoahDegenPoint degen_point = ShenandoahGC::_degenerated_unset;

    if (alloc_failure_pending) {
      // Allocation failure takes precedence: we have to deal with it first thing
      log_info(gc)("Trigger: Handle Allocation Failure");

      cause = GCCause::_allocation_failure;

      // Consume the degen point, and seed it with default value
      degen_point = _degen_point;
      _degen_point = ShenandoahGC::_degenerated_outside_cycle;

      if (degen_point == ShenandoahGC::_degenerated_outside_cycle) {
        _degen_generation = heap->mode()->is_generational() ? heap->young_generation() : heap->global_generation();
      } else {
        assert(_degen_generation != NULL, "Need to know which generation to resume.");
      }

      ShenandoahHeuristics* heuristics = _degen_generation->heuristics();
      generation = _degen_generation->generation_mode();
      bool old_gen_evacuation_failed = heap->clear_old_evacuation_failure();

      // Do not bother with degenerated cycle if old generation evacuation failed.
      if (ShenandoahDegeneratedGC && heuristics->should_degenerate_cycle() && !old_gen_evacuation_failed) {
        heuristics->record_allocation_failure_gc();
        policy->record_alloc_failure_to_degenerated(degen_point);
        set_gc_mode(stw_degenerated);
      } else {
        heuristics->record_allocation_failure_gc();
        policy->record_alloc_failure_to_full();
        generation = GLOBAL;
        set_gc_mode(stw_full);
      }
    } else if (explicit_gc_requested) {
      cause = requested_gc_cause;
      generation = GLOBAL;
      log_info(gc)("Trigger: Explicit GC request (%s)", GCCause::to_string(cause));

      global_heuristics->record_requested_gc();

      if (ExplicitGCInvokesConcurrent) {
        policy->record_explicit_to_concurrent();
        set_gc_mode(default_mode);
        // Unload and clean up everything
        heap->set_unload_classes(global_heuristics->can_unload_classes());
      } else {
        policy->record_explicit_to_full();
        set_gc_mode(stw_full);
      }
    } else if (implicit_gc_requested) {
      cause = requested_gc_cause;
      generation = GLOBAL;
      log_info(gc)("Trigger: Implicit GC request (%s)", GCCause::to_string(cause));

      global_heuristics->record_requested_gc();

      if (ShenandoahImplicitGCInvokesConcurrent) {
        policy->record_implicit_to_concurrent();
        set_gc_mode(default_mode);

        // Unload and clean up everything
        heap->set_unload_classes(global_heuristics->can_unload_classes());
      } else {
        policy->record_implicit_to_full();
        set_gc_mode(stw_full);
      }
    } else {
      // We should only be here if the regulator requested a cycle or if
      // there is an old generation mark in progress.
      if (_requested_gc_cause == GCCause::_shenandoah_concurrent_gc) {
        // preemption was requested or this is a regular cycle
        cause = GCCause::_shenandoah_concurrent_gc;
        generation = _requested_generation;
        set_gc_mode(default_mode);

        // Don't start a new old marking if there is one already in progress.
        if (generation == OLD && heap->is_concurrent_old_mark_in_progress()) {
          set_gc_mode(marking_old);
        }

        if (generation == GLOBAL) {
          heap->set_unload_classes(global_heuristics->should_unload_classes());
        } else {
          heap->set_unload_classes(false);
        }
      } else if (heap->is_concurrent_old_mark_in_progress() || heap->is_concurrent_prep_for_mixed_evacuation_in_progress()) {
        // Nobody asked us to do anything, but we have an old-generation mark or old-generation preparation for
        // mixed evacuation in progress, so resume working on that.
        cause = GCCause::_shenandoah_concurrent_gc;
        generation = OLD;
        set_gc_mode(marking_old);
      }

      // Don't want to spin in this loop and start a cycle every time, so
      // clear requested gc cause. This creates a race with callers of the
      // blocking 'request_gc' method, but there it loops and resets the
      // '_requested_gc_cause' until a full cycle is completed.
      _requested_gc_cause = GCCause::_no_gc;
    }

    // Blow all soft references on this cycle, if handling allocation failure,
    // either implicit or explicit GC request, or we are requested to do so unconditionally.
    if (generation == GLOBAL && (alloc_failure_pending || implicit_gc_requested || explicit_gc_requested || ShenandoahAlwaysClearSoftRefs)) {
      heap->soft_ref_policy()->set_should_clear_all_soft_refs(true);
    }

    bool gc_requested = (_mode != none);
    assert (!gc_requested || cause != GCCause::_last_gc_cause, "GC cause should be set");

    if (gc_requested) {
      // GC is starting, bump the internal ID
      update_gc_id();

      heap->reset_bytes_allocated_since_gc_start();

      MetaspaceCombinedStats meta_sizes = MetaspaceUtils::get_combined_statistics();

      // If GC was requested, we are sampling the counters even without actual triggers
      // from allocation machinery. This captures GC phases more accurately.
      set_forced_counters_update(true);

      // If GC was requested, we better dump freeset data for performance debugging
      {
        ShenandoahHeapLocker locker(heap->lock());
        heap->free_set()->log_status();
      }

      heap->set_aging_cycle(false);
      {
        switch (_mode) {
          case concurrent_normal: {
            if ((generation == YOUNG) && (age_period-- == 0)) {
              heap->set_aging_cycle(true);
              age_period = ShenandoahAgingCyclePeriod - 1;
            }
            service_concurrent_normal_cycle(heap, generation, cause);
            break;
          }
          case stw_degenerated: {
            if (!service_stw_degenerated_cycle(cause, degen_point)) {
              // The degenerated GC was upgraded to a Full GC
              generation = GLOBAL;
            }
            break;
          }
          case stw_full: {
            service_stw_full_cycle(cause);
            break;
          }
          case marking_old: {
            assert(generation == OLD, "Expected old generation here");
            resume_concurrent_old_cycle(heap->old_generation(), cause);
            break;
          }
          default: {
            ShouldNotReachHere();
          }
        }
      }

      // If this was the requested GC cycle, notify waiters about it
      if (explicit_gc_requested || implicit_gc_requested) {
        notify_gc_waiters();
      }

      // If this was the allocation failure GC cycle, notify waiters about it
      if (alloc_failure_pending) {
        notify_alloc_failure_waiters();
      }

      // Report current free set state at the end of cycle, whether
      // it is a normal completion, or the abort.
      {
        ShenandoahHeapLocker locker(heap->lock());
        heap->free_set()->log_status();

        // Notify Universe about new heap usage. This has implications for
        // global soft refs policy, and we better report it every time heap
        // usage goes down.
        Universe::heap()->update_capacity_and_used_at_gc();

        // Signal that we have completed a visit to all live objects.
        Universe::heap()->record_whole_heap_examined_timestamp();
      }

      // Disable forced counters update, and update counters one more time
      // to capture the state at the end of GC session.
      handle_force_counters_update();
      set_forced_counters_update(false);

      // Retract forceful part of soft refs policy
      heap->soft_ref_policy()->set_should_clear_all_soft_refs(false);

      // Clear metaspace oom flag, if current cycle unloaded classes
      if (heap->unload_classes()) {
        assert(generation == GLOBAL, "Only unload classes during GLOBAL cycle");
        global_heuristics->clear_metaspace_oom();
      }

      // Commit worker statistics to cycle data
      heap->phase_timings()->flush_par_workers_to_cycle();
      if (ShenandoahPacing) {
        heap->pacer()->flush_stats_to_cycle();
      }

      // Print GC stats for current cycle
      {
        LogTarget(Info, gc, stats) lt;
        if (lt.is_enabled()) {
          ResourceMark rm;
          LogStream ls(lt);
          heap->phase_timings()->print_cycle_on(&ls);
          if (ShenandoahPacing) {
            heap->pacer()->print_cycle_on(&ls);
          }
        }
      }

      // Commit statistics to globals
      heap->phase_timings()->flush_cycle_to_global();

      // Print Metaspace change following GC (if logging is enabled).
      MetaspaceUtils::print_metaspace_change(meta_sizes);

      // GC is over, we are at idle now
      if (ShenandoahPacing) {
        heap->pacer()->setup_for_idle();
      }
    } else {
      // Allow allocators to know we have seen this much regions
      if (ShenandoahPacing && (allocs_seen > 0)) {
        heap->pacer()->report_alloc(allocs_seen);
      }
    }

    double current = os::elapsedTime();

    if (ShenandoahUncommit && (explicit_gc_requested || soft_max_changed || (current - last_shrink_time > shrink_period))) {
      // Explicit GC tries to uncommit everything down to min capacity.
      // Soft max change tries to uncommit everything down to target capacity.
      // Periodic uncommit tries to uncommit suitable regions down to min capacity.

      double shrink_before = (explicit_gc_requested || soft_max_changed) ?
                             current :
                             current - (ShenandoahUncommitDelay / 1000.0);

      size_t shrink_until = soft_max_changed ?
                             heap->soft_max_capacity() :
                             heap->min_capacity();

      service_uncommit(shrink_before, shrink_until);
      heap->phase_timings()->flush_cycle_to_global();
      last_shrink_time = current;
    }

    // Don't wait around if there was an allocation failure - start the next cycle immediately.
    if (!is_alloc_failure_gc()) {
      // The timed wait is necessary because this thread has a responsibility to send
      // 'alloc_words' to the pacer when it does not perform a GC.
      MonitorLocker lock(&_control_lock, Mutex::_no_safepoint_check_flag);
      lock.wait(ShenandoahControlIntervalMax);
    }
  }

  // Wait for the actual stop(), can't leave run_service() earlier.
  while (!should_terminate()) {
    os::naked_short_sleep(ShenandoahControlIntervalMin);
  }
}

// Young and old concurrent cycles are initiated by the regulator. Implicit
// and explicit GC requests are handled by the controller thread and always
// run a global cycle (which is concurrent by default, but may be overridden
// by command line options). Old cycles always degenerate to a global cycle.
// Young cycles are degenerated to complete the young cycle.  Young
// and old degen may upgrade to Full GC.  Full GC may also be
// triggered directly by a System.gc() invocation.
//
//
//      +-----+ Idle +-----+-----------+---------------------+
//      |         +        |           |                     |
//      |         |        |           |                     |
//      |         |        v           |                     |
//      |         |  Bootstrap Old +-- | ------------+       |
//      |         |   +                |             |       |
//      |         |   |                |             |       |
//      |         v   v                v             v       |
//      |    Resume Old <----------+ Young +--> Young Degen  |
//      |     +  +                                   +       |
//      v     |  |                                   |       |
//   Global <-+  |                                   |       |
//      +        |                                   |       |
//      |        v                                   v       |
//      +--->  Global Degen +--------------------> Full <----+
//
void ShenandoahControlThread::service_concurrent_normal_cycle(
  const ShenandoahHeap* heap, const GenerationMode generation, GCCause::Cause cause) {

  switch (generation) {
    case YOUNG: {
      // Run a young cycle. This might or might not, have interrupted an ongoing
      // concurrent mark in the old generation. We need to think about promotions
      // in this case. Promoted objects should be above the TAMS in the old regions
      // they end up in, but we have to be sure we don't promote into any regions
      // that are in the cset.
      log_info(gc, ergo)("Start GC cycle (YOUNG)");
      service_concurrent_cycle(heap->young_generation(), cause, false);
      heap->young_generation()->log_status();
      break;
    }
    case GLOBAL: {
      log_info(gc, ergo)("Start GC cycle (GLOBAL)");
      service_concurrent_cycle(heap->global_generation(), cause, false);
      heap->global_generation()->log_status();
      break;
    }
    case OLD: {
      log_info(gc, ergo)("Start GC cycle (OLD)");
      service_concurrent_old_cycle(heap, cause);
      heap->old_generation()->log_status();
      break;
    }
    default:
      ShouldNotReachHere();
  }
}

void ShenandoahControlThread::service_concurrent_old_cycle(const ShenandoahHeap* heap, GCCause::Cause &cause) {
  // Configure the young generation's concurrent mark to put objects in
  // old regions into the concurrent mark queues associated with the old
  // generation. The young cycle will run as normal except that rather than
  // ignore old references it will mark and enqueue them in the old concurrent
  // mark but it will not traverse them.
  ShenandoahGeneration* old_generation = heap->old_generation();
  ShenandoahYoungGeneration* young_generation = heap->young_generation();

  assert(!heap->is_concurrent_old_mark_in_progress(), "Old already in progress.");
  assert(old_generation->task_queues()->is_empty(), "Old mark queues should be empty.");

  young_generation->set_old_gen_task_queues(old_generation->task_queues());
  young_generation->set_mark_incomplete();
  old_generation->set_mark_incomplete();
  service_concurrent_cycle(young_generation, cause, true);
  if (!heap->cancelled_gc()) {
    // Reset the degenerated point. Normally this would happen at the top
    // of the control loop, but here we have just completed a young cycle
    // which has bootstrapped the old concurrent marking.
    _degen_point = ShenandoahGC::_degenerated_outside_cycle;

    // TODO: Bit of a hack here to keep the phase timings happy as we transition
    // to concurrent old marking. We need to revisit this.
    heap->phase_timings()->flush_par_workers_to_cycle();
    heap->phase_timings()->flush_cycle_to_global();

    // From here we will 'resume' the old concurrent mark. This will skip reset
    // and init mark for the concurrent mark. All of that work will have been
    // done by the bootstrapping young cycle. In order to simplify the debugging
    // effort, the old cycle will ONLY complete the mark phase. No actual
    // collection of the old generation is happening here.
    set_gc_mode(marking_old);
    resume_concurrent_old_cycle(old_generation, cause);
  }
}

bool ShenandoahControlThread::check_soft_max_changed() const {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  size_t new_soft_max = Atomic::load(&SoftMaxHeapSize);
  size_t old_soft_max = heap->soft_max_capacity();
  if (new_soft_max != old_soft_max) {
    new_soft_max = MAX2(heap->min_capacity(), new_soft_max);
    new_soft_max = MIN2(heap->max_capacity(), new_soft_max);
    if (new_soft_max != old_soft_max) {
      log_info(gc)("Soft Max Heap Size: " SIZE_FORMAT "%s -> " SIZE_FORMAT "%s",
                   byte_size_in_proper_unit(old_soft_max), proper_unit_for_byte_size(old_soft_max),
                   byte_size_in_proper_unit(new_soft_max), proper_unit_for_byte_size(new_soft_max)
      );
      heap->set_soft_max_capacity(new_soft_max);
      return true;
    }
  }
  return false;
}

void ShenandoahControlThread::resume_concurrent_old_cycle(ShenandoahGeneration* generation, GCCause::Cause cause) {

  assert(ShenandoahHeap::heap()->is_concurrent_old_mark_in_progress() ||
         ShenandoahHeap::heap()->is_concurrent_prep_for_mixed_evacuation_in_progress(),
         "Old mark or mixed-evac prep should be in progress");
  log_debug(gc)("Resuming old generation with " UINT32_FORMAT " marking tasks queued.", generation->task_queues()->tasks());

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  GCIdMark gc_id_mark;
  ShenandoahGCSession session(cause, generation);

  TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
  // We can only tolerate being cancelled during concurrent marking or during preparation for mixed
  // evacuation. This flag here (passed by reference) is used to control precisely where the regulator
  // is allowed to cancel a GC.
  ShenandoahOldGC gc(generation, _allow_old_preemption);
  if (gc.collect(cause)) {
    // Old collection is complete, the young generation no longer needs this
    // reference to the old concurrent mark so clean it up.
    heap->young_generation()->set_old_gen_task_queues(NULL);
    generation->heuristics()->record_success_concurrent();
    heap->shenandoah_policy()->record_success_concurrent();
  }

  if (heap->cancelled_gc()) {
    // It's possible the gc cycle was cancelled after the last time
    // the collection checked for cancellation. In which case, the
    // old gc cycle is still completed, and we have to deal with this
    // cancellation. We set the degeneration point to be outside
    // the cycle because if this is an allocation failure, that is
    // what must be done (there is no degenerated old cycle). If the
    // cancellation was due to a heuristic wanting to start a young
    // cycle, then we are not actually going to a degenerated cycle,
    // so the degenerated point doesn't matter here.
    check_cancellation_or_degen(ShenandoahGC::_degenerated_outside_cycle);
  }
}

void ShenandoahControlThread::service_concurrent_cycle(ShenandoahGeneration* generation, GCCause::Cause cause, bool do_old_gc_bootstrap) {
  // Normal cycle goes via all concurrent phases. If allocation failure (af) happens during
  // any of the concurrent phases, it first degrades to Degenerated GC and completes GC there.
  // If second allocation failure happens during Degenerated GC cycle (for example, when GC
  // tries to evac something and no memory is available), cycle degrades to Full GC.
  //
  // There are also a shortcut through the normal cycle: immediate garbage shortcut, when
  // heuristics says there are no regions to compact, and all the collection comes from immediately
  // reclaimable regions.
  //
  // ................................................................................................
  //
  //                                    (immediate garbage shortcut)                Concurrent GC
  //                             /-------------------------------------------\
  //                             |                                           |
  //                             |                                           |
  //                             |                                           |
  //                             |                                           v
  // [START] ----> Conc Mark ----o----> Conc Evac --o--> Conc Update-Refs ---o----> [END]
  //                   |                    |                 |              ^
  //                   | (af)               | (af)            | (af)         |
  // ..................|....................|.................|..............|.......................
  //                   |                    |                 |              |
  //                   |                    |                 |              |      Degenerated GC
  //                   v                    v                 v              |
  //               STW Mark ----------> STW Evac ----> STW Update-Refs ----->o
  //                   |                    |                 |              ^
  //                   | (af)               | (af)            | (af)         |
  // ..................|....................|.................|..............|.......................
  //                   |                    |                 |              |
  //                   |                    v                 |              |      Full GC
  //                   \------------------->o<----------------/              |
  //                                        |                                |
  //                                        v                                |
  //                                      Full GC  --------------------------/
  //
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (check_cancellation_or_degen(ShenandoahGC::_degenerated_outside_cycle)) return;

  GCIdMark gc_id_mark;
  ShenandoahGCSession session(cause, generation);

  TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());

  ShenandoahConcurrentGC gc(generation, do_old_gc_bootstrap);
  if (gc.collect(cause)) {
    // Cycle is complete
    generation->heuristics()->record_success_concurrent();
    heap->shenandoah_policy()->record_success_concurrent();
  } else {
    assert(heap->cancelled_gc(), "Must have been cancelled");
    check_cancellation_or_degen(gc.degen_point());
    assert(generation->generation_mode() != OLD, "Old GC takes a different control path");
    // Concurrent young-gen collection degenerates to young
    // collection.  Same for global collections.
    _degen_generation = generation;
  }
}

bool ShenandoahControlThread::check_cancellation_or_degen(ShenandoahGC::ShenandoahDegenPoint point) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (!heap->cancelled_gc()) {
    return false;
  }

  if (in_graceful_shutdown()) {
    return true;
  }

  assert(_degen_point == ShenandoahGC::_degenerated_outside_cycle,
         "Should not be set yet: %s", ShenandoahGC::degen_point_to_string(_degen_point));

  if (is_alloc_failure_gc()) {
    _degen_point = point;
    return true;
  }

  if (_preemption_requested.is_set()) {
    assert(_requested_generation == YOUNG, "Only young GCs may preempt old.");
    _preemption_requested.unset();

    // Old generation marking is only cancellable during concurrent marking.
    // Once final mark is complete, the code does not check again for cancellation.
    // If old generation was cancelled for an allocation failure, we wouldn't
    // make it to this case. The calling code is responsible for forcing a
    // cancellation due to allocation failure into a degenerated cycle.
    _degen_point = point;
    heap->clear_cancelled_gc(false /* clear oom handler */);
    return true;
  }

  fatal("Cancel GC either for alloc failure GC, or gracefully exiting, or to pause old generation marking.");
  return false;
}

void ShenandoahControlThread::stop_service() {
  // Nothing to do here.
}

void ShenandoahControlThread::service_stw_full_cycle(GCCause::Cause cause) {
  ShenandoahHeap* const heap = ShenandoahHeap::heap();

  GCIdMark gc_id_mark;
  ShenandoahGCSession session(cause, heap->global_generation());

  ShenandoahFullGC gc;
  gc.collect(cause);

  heap->global_generation()->heuristics()->record_success_full();
  heap->shenandoah_policy()->record_success_full();
}

bool ShenandoahControlThread::service_stw_degenerated_cycle(GCCause::Cause cause, ShenandoahGC::ShenandoahDegenPoint point) {
  assert (point != ShenandoahGC::_degenerated_unset, "Degenerated point should be set");
  ShenandoahHeap* const heap = ShenandoahHeap::heap();

  GCIdMark gc_id_mark;
  ShenandoahGCSession session(cause, _degen_generation);

  ShenandoahDegenGC gc(point, _degen_generation);

  // Just in case degenerated cycle preempted old-gen marking, clear the old-gen task queues.
  heap->young_generation()->set_old_gen_task_queues(NULL);

  gc.collect(cause);

  assert(heap->young_generation()->task_queues()->is_empty(), "Unexpected young generation marking tasks");
  assert(heap->old_generation()->task_queues()->is_empty(), "Unexpected old generation marking tasks");
  assert(heap->global_generation()->task_queues()->is_empty(), "Unexpected global generation marking tasks");

  _degen_generation->heuristics()->record_success_degenerated();
  heap->shenandoah_policy()->record_success_degenerated();
  return !gc.upgraded_to_full();
}

void ShenandoahControlThread::service_uncommit(double shrink_before, size_t shrink_until) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  // Determine if there is work to do. This avoids taking heap lock if there is
  // no work available, avoids spamming logs with superfluous logging messages,
  // and minimises the amount of work while locks are taken.

  if (heap->committed() <= shrink_until) return;

  bool has_work = false;
  for (size_t i = 0; i < heap->num_regions(); i++) {
    ShenandoahHeapRegion *r = heap->get_region(i);
    if (r->is_empty_committed() && (r->empty_time() < shrink_before)) {
      has_work = true;
      break;
    }
  }

  if (has_work) {
    heap->entry_uncommit(shrink_before, shrink_until);
  }
}

bool ShenandoahControlThread::is_explicit_gc(GCCause::Cause cause) const {
  return GCCause::is_user_requested_gc(cause) ||
         GCCause::is_serviceability_requested_gc(cause);
}

bool ShenandoahControlThread::is_implicit_gc(GCCause::Cause cause) const {
  return !is_explicit_gc(cause) && cause != GCCause::_shenandoah_concurrent_gc;
}

void ShenandoahControlThread::request_gc(GCCause::Cause cause) {
  assert(GCCause::is_user_requested_gc(cause) ||
         GCCause::is_serviceability_requested_gc(cause) ||
         cause == GCCause::_metadata_GC_clear_soft_refs ||
         cause == GCCause::_full_gc_alot ||
         cause == GCCause::_wb_full_gc ||
         cause == GCCause::_wb_breakpoint ||
         cause == GCCause::_scavenge_alot,
         "only requested GCs here");

  if (is_explicit_gc(cause)) {
    if (!DisableExplicitGC) {
      handle_requested_gc(cause);
    }
  } else {
    handle_requested_gc(cause);
  }
}

bool ShenandoahControlThread::request_concurrent_gc(GenerationMode generation) {
  if (_preemption_requested.is_set() || _gc_requested.is_set() || ShenandoahHeap::heap()->cancelled_gc()) {
    // ignore subsequent requests from the heuristics
    return false;
  }

  if (_mode == none) {
    _requested_gc_cause = GCCause::_shenandoah_concurrent_gc;
    _requested_generation = generation;
    notify_control_thread();
    return true;
  }

  if (preempt_old_marking(generation)) {
    log_info(gc)("Preempting old generation mark to allow young GC.");
    _requested_gc_cause = GCCause::_shenandoah_concurrent_gc;
    _requested_generation = generation;
    _preemption_requested.set();
    ShenandoahHeap::heap()->cancel_gc(GCCause::_shenandoah_concurrent_gc);
    notify_control_thread();
    return true;
  }

  return false;
}

void ShenandoahControlThread::notify_control_thread() {
  MonitorLocker locker(&_control_lock, Mutex::_no_safepoint_check_flag);
  _control_lock.notify();
}

bool ShenandoahControlThread::preempt_old_marking(GenerationMode generation) {
  return generation == YOUNG && _allow_old_preemption.try_unset();
}

void ShenandoahControlThread::handle_requested_gc(GCCause::Cause cause) {
  // Make sure we have at least one complete GC cycle before unblocking
  // from the explicit GC request.
  //
  // This is especially important for weak references cleanup and/or native
  // resources (e.g. DirectByteBuffers) machinery: when explicit GC request
  // comes very late in the already running cycle, it would miss lots of new
  // opportunities for cleanup that were made available before the caller
  // requested the GC.

  MonitorLocker ml(&_gc_waiters_lock);
  size_t current_gc_id = get_gc_id();
  size_t required_gc_id = current_gc_id + 1;
  while (current_gc_id < required_gc_id) {
    // Although setting gc request is under _gc_waiters_lock, but read side (run_service())
    // does not take the lock. We need to enforce following order, so that read side sees
    // latest requested gc cause when the flag is set.
    _requested_gc_cause = cause;
    _gc_requested.set();
    notify_control_thread();
    if (cause != GCCause::_wb_breakpoint) {
      ml.wait();
    }
    current_gc_id = get_gc_id();
  }
}

void ShenandoahControlThread::handle_alloc_failure(ShenandoahAllocRequest& req) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  assert(current()->is_Java_thread(), "expect Java thread here");

  if (try_set_alloc_failure_gc()) {
    // Only report the first allocation failure
    log_info(gc)("Failed to allocate %s, " SIZE_FORMAT "%s",
                 req.type_string(),
                 byte_size_in_proper_unit(req.size() * HeapWordSize), proper_unit_for_byte_size(req.size() * HeapWordSize));

    // Now that alloc failure GC is scheduled, we can abort everything else
    heap->cancel_gc(GCCause::_allocation_failure);
  }

  MonitorLocker ml(&_alloc_failure_waiters_lock);
  while (is_alloc_failure_gc()) {
    ml.wait();
  }
}

void ShenandoahControlThread::handle_alloc_failure_evac(size_t words) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  if (try_set_alloc_failure_gc()) {
    // Only report the first allocation failure
    log_info(gc)("Failed to allocate " SIZE_FORMAT "%s for evacuation",
                 byte_size_in_proper_unit(words * HeapWordSize), proper_unit_for_byte_size(words * HeapWordSize));
  }

  // Forcefully report allocation failure
  heap->cancel_gc(GCCause::_shenandoah_allocation_failure_evac);
}

void ShenandoahControlThread::notify_alloc_failure_waiters() {
  _alloc_failure_gc.unset();
  MonitorLocker ml(&_alloc_failure_waiters_lock);
  ml.notify_all();
}

bool ShenandoahControlThread::try_set_alloc_failure_gc() {
  return _alloc_failure_gc.try_set();
}

bool ShenandoahControlThread::is_alloc_failure_gc() {
  return _alloc_failure_gc.is_set();
}

void ShenandoahControlThread::notify_gc_waiters() {
  _gc_requested.unset();
  MonitorLocker ml(&_gc_waiters_lock);
  ml.notify_all();
}

void ShenandoahControlThread::handle_counters_update() {
  if (_do_counters_update.is_set()) {
    _do_counters_update.unset();
    ShenandoahHeap::heap()->monitoring_support()->update_counters();
  }
}

void ShenandoahControlThread::handle_force_counters_update() {
  if (_force_counters_update.is_set()) {
    _do_counters_update.unset(); // reset these too, we do update now!
    ShenandoahHeap::heap()->monitoring_support()->update_counters();
  }
}

void ShenandoahControlThread::notify_heap_changed() {
  // This is called from allocation path, and thus should be fast.

  // Update monitoring counters when we took a new region. This amortizes the
  // update costs on slow path.
  if (_do_counters_update.is_unset()) {
    _do_counters_update.set();
  }
}

void ShenandoahControlThread::pacing_notify_alloc(size_t words) {
  assert(ShenandoahPacing, "should only call when pacing is enabled");
  Atomic::add(&_allocs_seen, words, memory_order_relaxed);
}

void ShenandoahControlThread::set_forced_counters_update(bool value) {
  _force_counters_update.set_cond(value);
}

void ShenandoahControlThread::reset_gc_id() {
  Atomic::store(&_gc_id, (size_t)0);
}

void ShenandoahControlThread::update_gc_id() {
  Atomic::inc(&_gc_id);
}

size_t ShenandoahControlThread::get_gc_id() {
  return Atomic::load(&_gc_id);
}

void ShenandoahControlThread::print() const {
  print_on(tty);
}

void ShenandoahControlThread::print_on(outputStream* st) const {
  st->print("Shenandoah Concurrent Thread");
  Thread::print_on(st);
  st->cr();
}

void ShenandoahControlThread::start() {
  create_and_start();
}

void ShenandoahControlThread::prepare_for_graceful_shutdown() {
  _graceful_shutdown.set();
}

bool ShenandoahControlThread::in_graceful_shutdown() {
  return _graceful_shutdown.is_set();
}

const char* ShenandoahControlThread::gc_mode_name(ShenandoahControlThread::GCMode mode) {
  switch (mode) {
    case none:              return "idle";
    case concurrent_normal: return "normal";
    case stw_degenerated:   return "degenerated";
    case stw_full:          return "full";
    case marking_old:       return "old mark";
    default:                return "unknown";
  }
}

void ShenandoahControlThread::set_gc_mode(ShenandoahControlThread::GCMode new_mode) {
  if (_mode != new_mode) {
    log_info(gc)("Transition from: %s to: %s", gc_mode_name(_mode), gc_mode_name(new_mode));
    _mode = new_mode;
  }
}
