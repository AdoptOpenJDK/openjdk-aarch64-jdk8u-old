/*
 * Copyright (c) 2017 Red Hat, Inc. and/or its affiliates.
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

/**
 * @test TestHeapAlloc
 * @key gc
 * @summary Fail to expand Java heap should not result fatal errors
 * @library /testlibrary
 * @ignore // heap is not resized in a conventional manner, ignore for now
 */

import com.oracle.java.testlibrary.*;

import java.util.*;
import java.util.concurrent.*;

public class TestHeapAlloc {

  public static void main(String[] args) throws Exception {
    if (!Platform.isDebugBuild()) {
      System.out.println("Test requires debug build. Please silently for release build");
      return;
    }

    ProcessBuilder pb = ProcessTools.createJavaProcessBuilder(
        true,
        "-Xmx1G",
        "-Xms256M",
        "-XX:+UseShenandoahGC",
        "-XX:ShenandoahFailHeapExpansionAfter=50",
        "-XX:+ShenandoahLogDebug",
        AllocALotObjects.class.getName());
    OutputAnalyzer output = new OutputAnalyzer(pb.start());
    output.shouldContain("Artificially fails heap expansion");
    output.shouldHaveExitValue(0);
  }

  public static class AllocALotObjects {
    public static void main(String[] args) {
      try {
        ArrayList<String> list = new ArrayList<>();
        long count = 500 * 1024 * 1024 / 16; // 500MB allocation
        for (long index = 0; index < count; index ++) {
          String sink = "string " + index;
          list.add(sink);
        }

        int index = ThreadLocalRandom.current().nextInt(0, (int)count);
        System.out.print(list.get(index));
      } catch (OutOfMemoryError e) {
      }
    }
  }

}
