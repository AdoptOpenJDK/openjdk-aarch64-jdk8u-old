/*
 * Copyright (c) 2016, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAH_GLOBALS_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAH_GLOBALS_HPP

#include "runtime/globals.hpp"

#define SHENANDOAH_FLAGS(develop, \
                         develop_pd, \
                         product, \
                         product_pd, \
                         diagnostic, \
                         experimental, \
                         notproduct, \
                         manageable, \
                         product_rw) \
                                                                            \
  product(bool, ShenandoahOptimizeStaticFinals, true,                       \
          "Optimize barriers on static final fields. "                      \
          "Turn it off for maximum compatibility with reflection or JNI "   \
          "code that manipulates final fields.")                            \
                                                                            \
  product(bool, ShenandoahOptimizeInstanceFinals, false,                    \
          "Optimize barriers on final instance fields."                     \
          "Turn it off for maximum compatibility with reflection or JNI "   \
          "code that manipulates final fields.")                            \
                                                                            \
  product(bool, ShenandoahOptimizeStableFinals, false,                      \
          "Optimize barriers on stable fields."                             \
          "Turn it off for maximum compatibility with reflection or JNI "   \
          "code that manipulates final fields.")                            \
                                                                            \
  product(uintx, ShenandoahHeapRegionSize, 0,                               \
          "Size of the Shenandoah regions. "                                \
          "Determined automatically by default.")                           \
                                                                            \
  experimental(uintx, ShenandoahMinRegionSize, 256 * K,                     \
          "Minimum heap region size. ")                                     \
                                                                            \
  experimental(uintx, ShenandoahMaxRegionSize, 32 * M,                      \
          "Maximum heap region size. ")                                     \
                                                                            \
  experimental(intx, ShenandoahHumongousThreshold, 100,                     \
          "How large should the object be to get allocated in humongous "   \
          "region, in percents of heap region size. This also caps the "    \
          "maximum TLAB size.")                                             \
                                                                            \
  experimental(uintx, ShenandoahTargetNumRegions, 2048,                     \
          "Target number of regions. We try to get around that many "       \
          "regions, based on ShenandoahMinRegionSize and "                  \
          "ShenandoahMaxRegionSizeSize. ")                                  \
                                                                            \
  product(ccstr, ShenandoahGCHeuristics, "adaptive",                        \
          "The heuristics to use in Shenandoah GC. Possible values: "       \
          "adaptive (adapt to maintain the given amount of free memory), "  \
          "static (start concurrent GC when static free heap threshold "    \
          "and static allocation threshold are tripped), "                  \
          "passive (do not start concurrent GC, wait for Full GC) "         \
          "aggressive (run concurrent GC continuously, evacuate everything), " \
          "compact (run GC with lower footprint target)."                    \
          "Defaults to adaptive")                                           \
                                                                            \
  experimental(ccstr, ShenandoahUpdateRefsEarly, "adaptive",                \
          "Run a separate concurrent reference updating phase after"        \
          "concurrent evacuation. Possible values: 'on', 'off', 'adaptive'")\
                                                                            \
  experimental(uintx, ShenandoahEvacAssist, 10,                             \
          "How many objects to evacuate on WB assist path. "                \
          "Use zero to disable.")                                           \
                                                                            \
  product(uintx, ShenandoahRefProcFrequency, 5,                             \
          "How often should (weak, soft, etc) references be processed. "    \
          "References get processed at every Nth GC cycle. "                \
          "Set to 0 to disable reference processing. "                      \
          "Defaults to process references every 5 cycles.")                 \
                                                                            \
  product(uintx, ShenandoahUnloadClassesFrequency, 5,                       \
          "How often should classes get unloaded. "                         \
          "Class unloading is performed at every Nth GC cycle. "            \
          "Set to 0 to disable concurrent class unloading. "                \
          "Defaults to unload classes every 5 cycles.")                     \
                                                                            \
  product(bool, ShenandoahLogTrace, false,                                  \
          "Turns on logging in Shenandoah at trace level. ")                \
                                                                            \
  product(bool, ShenandoahLogDebug, false,                                  \
          "Turns on logging in Shenandoah at debug level. ")                \
                                                                            \
  product(bool, ShenandoahLogInfo, false,                                   \
          "Turns on logging in Shenandoah at info level. ")                 \
                                                                            \
  product(bool, ShenandoahLogWarning, false,                                \
          "Turns on logging in Shenandoah at warning level. ")              \
                                                                            \
  product_rw(uintx, ShenandoahFullGCThreshold, 3,                           \
          "How many back-to-back Degenerated GCs to do before triggering "  \
          "a Full GC. Defaults to 3.")                                      \
                                                                            \
  product_rw(uintx, ShenandoahGarbageThreshold, 60,                         \
          "Sets the percentage of garbage a region need to contain before " \
          "it can be marked for collection. Applies to "                    \
          "Shenandoah GC dynamic Heuristic mode only (ignored otherwise). " \
          "Defaults to 60%.")                                               \
                                                                            \
  product_rw(uintx, ShenandoahFreeThreshold, 10,                            \
          "Set the percentage of free heap at which a GC cycle is started. " \
          "Applies to Shenandoah GC dynamic Heuristic mode only "           \
          "(ignored otherwise). Defaults to 10%.")                          \
                                                                            \
  product_rw(uintx, ShenandoahAllocationThreshold, 0,                       \
          "Set percentage of memory allocated since last GC cycle before "  \
          "a new GC cycle is started. "                                     \
          "Applies to Shenandoah GC dynamic Heuristic mode only "           \
          "(ignored otherwise). Defauls to 0%.")                            \
									    \
  experimental(uintx, ShenandoahMergeUpdateRefsMinGap, 100,                 \
               "If GC is currently running in separate update-refs mode "   \
               "this numbers gives the threshold when to switch to "        \
               "merged update-refs mode. Number is percentage relative to"  \
               "duration(marking)+duration(update-refs).")                  \
                                                                            \
  experimental(uintx, ShenandoahMergeUpdateRefsMaxGap, 200,                 \
               "If GC is currently running in merged update-refs mode "     \
               "this numbers gives the threshold when to switch to "        \
               "separate update-refs mode. Number is percentage relative "  \
               "to duration(marking)+duration(update-refs).")               \
                                                                            \
  experimental(uintx, ShenandoahInitFreeThreshold, 70,                      \
               "Initial remaining free threshold for learning steps in "    \
               "heuristics. In percents of total heap size.")               \
                                                                            \
  experimental(uintx, ShenandoahMinFreeThreshold, 10,                       \
               "Minimum remaining free threshold for adaptive heuristics")  \
                                                                            \
  experimental(uintx, ShenandoahLearningSteps, 5,                           \
               "Number of GC cycles to run in order to learn application "  \
               "and GC performance for adaptive heuristics.")               \
                                                                            \
  experimental(uintx, ShenandoahAllocSpikeFactor, 5,                        \
               "The amount of heap space to reserve for absorbing the "     \
               "allocation spikes. Larger value wastes more memory in "     \
               "non-emergency cases, but provides more safety in emergency "\
               "cases. In percents of total heap size.")                    \
                                                                            \
  experimental(uintx, ShenandoahEvacReserve, 5,                             \
               "Maximum amount of free space to reserve for evacuation. "   \
               "Larger values make GC more aggressive, while leaving less " \
               "headroom for application to allocate in. "                  \
               "In percents of free space available.")                      \
                                                                            \
  experimental(double, ShenandoahEvacWaste, 1.2,                            \
               "How much waste evacuations produce within the reserved "    \
               "space. Larger values make evacuations more resilient "      \
               "against allocation failures, at expense of smaller csets "  \
               "on each cycle.")                                            \
                                                                            \
  experimental(bool, ShenandoahEvacReserveOverflow, true,                   \
               "Allow evacuations to overflow the reserved space. "         \
               "Enabling it will make evacuations more resilient when "     \
               "evacuation reserve/waste is incorrect, at the risk that "   \
               "application allocations run out of memory too early.")      \
                                                                            \
  experimental(uintx, ShenandoahImmediateThreshold, 90,                     \
               "If mark identifies more than this much immediate garbage "  \
               "regions, it shall recycle them, and shall not continue the "\
               "rest of the GC cycle. The value is in percents of total "   \
               "number of candidates for collection set. Setting this "     \
               "threshold to 100% effectively disables this shortcut.")     \
                                                                            \
  experimental(uintx, ShenandoahGuaranteedGCInterval, 5*60*1000,            \
               "Adaptive and dynamic heuristics would guarantee a GC cycle "\
               "at least with this interval. This is useful when large idle"\
               " intervals are present, where GC can run without stealing " \
               "time from active application. Time is in milliseconds.")    \
                                                                            \
  experimental(uintx, ShenandoahHappyCyclesThreshold, 3,                    \
          "How many successful marking cycles before improving free "       \
               "threshold for adaptive heuristics")                         \
                                                                            \
  experimental(uintx, ShenandoahMarkLoopStride, 1000,                       \
          "How many items are processed during one marking step")           \
                                                                            \
  experimental(bool, ShenandoahConcurrentScanCodeRoots, true,               \
          "Scan code roots concurrently, instead of during a pause")        \
                                                                            \
  experimental(uintx, ShenandoahCodeRootsStyle, 2,                          \
          "Use this style to scan code cache:"                              \
          " 0 - sequential iterator;"                                       \
          " 1 - parallel iterator;"                                         \
          " 2 - parallel iterator with cset filters;")                      \
                                                                            \
  experimental(bool, ShenandoahUncommit, true,                              \
          "Allow Shenandoah to uncommit unused memory.")                    \
                                                                            \
  experimental(uintx, ShenandoahUncommitDelay, 5*60*1000,                   \
           "Shenandoah would start to uncommit memory for regions that were"\
           " not used for more than this time. First use after that would " \
           "incur allocation stalls. Actively used regions would never be " \
           "uncommitted, because they never decay. Time is in milliseconds."\
           "Setting this delay to 0 effectively makes Shenandoah to "       \
           "uncommit the regions almost immediately.")                      \
                                                                            \
  experimental(bool, ShenandoahDontIncreaseWBFreq, true,                    \
          "Common 2 WriteBarriers or WriteBarrier and a ReadBarrier only "  \
          "if the resulting WriteBarrier isn't executed more frequently")   \
                                                                            \
  experimental(bool, ShenandoahWriteBarrierCsetTestInIR, true,              \
          "Perform cset test in IR rather than in the stub")                \
                                                                            \
  experimental(bool, UseShenandoahOWST, true,                               \
          "Use Shenandoah work stealing termination protocol")              \
                                                                            \
  experimental(size_t, ShenandoahSATBBufferSize, 1 * K,                     \
          "Number of entries in an SATB log buffer.")                       \
                                                                            \
  product_rw(int, ShenandoahRegionSamplingRate, 40,                         \
          "Sampling rate for heap region sampling. "                        \
          "Number of milliseconds between samples")                         \
                                                                            \
  product_rw(bool, ShenandoahRegionSampling, false,                         \
          "Turns on heap region sampling via JVMStat")                      \
                                                                            \
  experimental(uintx, ShenandoahControlIntervalMin, 1,                      \
              "The minumum sleep interval for control loop that drives "    \
              "the cycles. Lower values would increase GC responsiveness "  \
              "to changing heap conditions, at the expense of higher perf " \
              "overhead. Time is in milliseconds.")                         \
                                                                            \
  experimental(uintx, ShenandoahControlIntervalMax, 10,                     \
              "The maximum sleep interval for control loop that drives "    \
              "the cycles. Lower values would increase GC responsiveness "  \
              "to changing heap conditions, at the expense of higher perf " \
              "overhead. Time is in milliseconds.")                         \
                                                                            \
  experimental(uintx, ShenandoahControlIntervalAdjustPeriod, 1000,          \
              "The time period for one step in control loop interval "      \
              "adjustment. Lower values make adjustments faster, at the "   \
              "expense of higher perf overhead. Time is in milliseconds.")  \
                                                                            \
  diagnostic(bool, ShenandoahSATBBarrier, true,                             \
          "Turn on/off SATB barriers in Shenandoah")                        \
                                                                            \
  diagnostic(bool, ShenandoahWriteBarrier, true,                            \
          "Turn on/off write barriers in Shenandoah")                       \
                                                                            \
  diagnostic(bool, ShenandoahWriteBarrierRB, true,                          \
          "Turn on/off RB on WB fastpath in Shenandoah.")                   \
                                                                            \
  diagnostic(bool, ShenandoahReadBarrier, true,                             \
          "Turn on/off read barriers in Shenandoah")                        \
                                                                            \
  diagnostic(bool, ShenandoahCASBarrier, true,                              \
          "Turn on/off CAS barriers in Shenandoah")                         \
                                                                            \
  diagnostic(bool, ShenandoahAcmpBarrier, true,                             \
          "Turn on/off acmp barriers in Shenandoah")                        \
                                                                            \
  diagnostic(bool, ShenandoahCloneBarrier, true,                            \
          "Turn on/off clone barriers in Shenandoah")                       \
                                                                            \
  diagnostic(bool, ShenandoahStoreCheck, false,                             \
          "Emit additional code that checks objects are written to only"    \
          " in to-space")                                                   \
                                                                            \
  diagnostic(bool, ShenandoahVerify, false,                                 \
          "Verify the Shenandoah garbage collector")                        \
                                                                            \
  diagnostic(intx, ShenandoahVerifyLevel, 4,                                \
          "Shenandoah verification level: "                                 \
          "0 = basic heap checks; "                                         \
          "1 = previous level, plus basic region checks; "                  \
          "2 = previous level, plus all roots; "                            \
          "3 = previous level, plus all reachable objects; "                \
          "4 = previous level, plus all marked objects")                    \
                                                                            \
  diagnostic(bool, ShenandoahAllocationTrace, false,                        \
          "Trace allocation latencies and stalls. Can be expensive when "   \
          "lots of allocations happen, and may introduce scalability "      \
          "bottlenecks.")                                                   \
                                                                            \
  diagnostic(intx, ShenandoahAllocationStallThreshold, 10000,               \
          "When allocation tracing is enabled, the allocation stalls "      \
          "larger than this threshold would be reported as warnings. "      \
          "Time is in microseconds.")                                       \
                                                                            \
  develop(bool, VerifyStrictOopOperations, false,                           \
          "Verify that == and != are not used on oops. Only in fastdebug")  \
                                                                            \
  develop(bool, ShenandoahVerifyOptoBarriers, false,                        \
          "Verify no missing barriers in c2")                               \
                                                                            \
  develop(intx, ShenandoahFailHeapExpansionAfter, -1,                       \
          "Artificially fails heap expansion after specified times."        \
          "Used to verify allocation handling. Default -1 to disable it.")  \
                                                                            \
  product(bool, ShenandoahAlwaysPreTouch, false,                            \
          "Pre-touch heap memory, overrides global AlwaysPreTouch")         \
                                                                            \
  experimental(intx, ShenandoahMarkScanPrefetch, 32,                        \
          "How many objects to prefetch ahead when traversing mark bitmaps." \
          "Set to 0 to disable prefetching.")                               \
                                                                            \
  experimental(bool, ShenandoahFastSyncRoots, true,                         \
          "Enable fast synchronizer roots scanning")                        \
                                                                            \
  experimental(bool, ShenandoahPreclean, true,                              \
              "Do preclean phase before final mark")                        \
                                                                            \
  experimental(bool, ShenandoahHumongousMoves, true,                        \
          "Allow moving humongous regions. This makes GC more resistant "   \
          "to external fragmentation that may otherwise fail other "        \
          "humongous allocations, at the expense of higher GC copying "     \
          "costs.")                                                         \
                                                                            \
  diagnostic(bool, ShenandoahOOMDuringEvacALot, false,                      \
          "Simulate OOM during evacuation frequently.")                     \
                                                                            \
  diagnostic(bool, ShenandoahAllocFailureALot, false,                       \
          "Make lots of artificial allocation failures.")                   \
                                                                            \
  diagnostic(bool, ShenandoahDegeneratedGC, true,                           \
          "Use Degenerated GC.")                                            \
                                                                            \
  experimental(bool, ShenandoahPacing, true,                                \
          "Pace application allocations to give GC chance to start "        \
          "and complete.")                                                  \
                                                                            \
  experimental(uintx, ShenandoahPacingMaxDelay, 10,                         \
          "Max delay for pacing application allocations. "                  \
          "Time is in milliseconds.")                                       \
                                                                            \
  experimental(uintx, ShenandoahPacingIdleSlack, 2,                         \
          "Percent of heap counted as non-taxable allocations during idle. "\
          "Larger value makes the pacing milder during idle phases, "       \
          "requiring less rendezvous with control thread. Lower value "     \
          "makes the pacing control less responsive to out-of-cycle allocs.")\
                                                                            \
  experimental(uintx, ShenandoahPacingCycleSlack, 10,                       \
          "Percent of free space taken as non-taxable allocations during "  \
          "the GC cycle. Larger value makes the pacing milder at the "      \
          "beginning of the GC cycle. Lower value makes the pacing less "   \
          "uniform during the cycle.")                                      \
                                                                            \
  experimental(double, ShenandoahPacingSurcharge, 1.1,                      \
          "Additional pacing tax surcharge to help unclutter the heap. "    \
          "Larger values makes the pacing more aggressive. Lower values "   \
          "risk GC cycles finish with less memory than were available at "  \
          "the beginning of it.")                                           \
                                                                            \
  experimental(uintx, ShenandoahCriticalFreeThreshold, 1,                   \
          "Percent of heap that needs to be free after recovery cycles, "   \
          "either Degenerated or Full GC. If this much space is not "       \
          "available, next recovery step would triggered.")                 \
                                                                            \
  experimental(uintx, ShenandoahSATBBufferFlushInterval, 100,               \
          "Forcefully flush non-empty SATB buffers at this interval. "      \
          "Time is in milliseconds.")                                       \
                                                                            \
  diagnostic(bool, ShenandoahAllowMixedAllocs, true,                        \
          "Allow mixing mutator and collector allocations in a single "     \
          "region")                                                         \
                                                                            \
  diagnostic(bool, ShenandoahTerminationTrace, false,                       \
          "Tracing task termination timings")                               \
                                                                            \
  diagnostic(bool, ShenandoahElasticTLAB, true,                             \
          "Use Elastic TLABs with Shenandoah")                              \
                                                                            \
  diagnostic(bool, ShenandoahCompileCheck, false,                           \
          "Assert that methods are successfully compilable")                \
                                                                            \
  experimental(bool, ShenandoahAlwaysClearSoftRefs, false,                  \
          "Clear soft references unconditionally")                          \

SHENANDOAH_FLAGS(DECLARE_DEVELOPER_FLAG, \
                 DECLARE_PD_DEVELOPER_FLAG,     \
                 DECLARE_PRODUCT_FLAG,          \
                 DECLARE_PD_PRODUCT_FLAG,       \
                 DECLARE_DIAGNOSTIC_FLAG,       \
                 DECLARE_EXPERIMENTAL_FLAG,     \
                 DECLARE_NOTPRODUCT_FLAG,       \
                 DECLARE_MANAGEABLE_FLAG,       \
                 DECLARE_PRODUCT_RW_FLAG)

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAH_GLOBALS_HPP
