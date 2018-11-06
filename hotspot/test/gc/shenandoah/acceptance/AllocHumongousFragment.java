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

/*
 * @test AllocHumongousFragment
 * @summary Make sure Shenandoah can recover from humongous allocation fragmentation
 *
 * @run main/othervm -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=passive      -XX:+ShenandoahDegeneratedGC -XX:+ShenandoahVerify AllocHumongousFragment
 * @run main/othervm -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=passive      -XX:-ShenandoahDegeneratedGC -XX:+ShenandoahVerify AllocHumongousFragment
 * @run main/othervm -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=passive      -XX:+ShenandoahDegeneratedGC                       AllocHumongousFragment
 * @run main/othervm -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=passive      -XX:-ShenandoahDegeneratedGC                       AllocHumongousFragment
 *
 * @run main/othervm/timeout=240 -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=aggressive -XX:+ShenandoahOOMDuringEvacALot -XX:+ShenandoahVerify AllocHumongousFragment
 * @run main/othervm/timeout=240 -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=aggressive -XX:+ShenandoahAllocFailureALot  -XX:+ShenandoahVerify AllocHumongousFragment
 * @run main/othervm/timeout=240 -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=aggressive -XX:+ShenandoahOOMDuringEvacALot                       AllocHumongousFragment
 * @run main/othervm/timeout=240 -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=aggressive -XX:+ShenandoahAllocFailureALot                        AllocHumongousFragment
 *
 * @run main/othervm -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=adaptive     -XX:+ShenandoahVerify AllocHumongousFragment
 * @run main/othervm -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=static       -XX:+ShenandoahVerify AllocHumongousFragment
 *
 * @run main/othervm -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=adaptive     AllocHumongousFragment
 * @run main/othervm -verbose:gc -XX:+UseShenandoahGC -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -Xmx1g -Xms1g -XX:ShenandoahTargetNumRegions=2048 -XX:ShenandoahGCHeuristics=static       AllocHumongousFragment
 */

import java.util.*;
import java.util.concurrent.*;

public class AllocHumongousFragment {

  static final long TARGET_MB = Long.getLong("target",  30_000); // 30 Gb allocations
  static final long LIVE_MB   = Long.getLong("occupancy",  700); // 700 Mb alive

  static volatile Object sink;

  static List<int[]> objects;

  public static void main(String[] args) throws Exception {
    final int min =     128*1024;
    final int max = 16*1024*1024;
    final long count = TARGET_MB * 1024 * 1024 / (16 + 4*(min + (max-min)/2));

    objects = new ArrayList<>();
    long current = 0;

    Random r = new Random();
    for (long c = 0; c < count; c++) {
      while (current > LIVE_MB * 1024 * 1024) {
        int idx = ThreadLocalRandom.current().nextInt(objects.size());
        int[] remove = objects.remove(idx);
        current -= remove.length * 4 + 16;
      }

      int[] newObj = new int[min + r.nextInt(max-min)];
      current += newObj.length * 4 + 16;
      objects.add(newObj);
      sink = new Object();

      System.out.println("Allocated: " + (current / 1024 / 1024) + " Mb");
    }
  }

}
