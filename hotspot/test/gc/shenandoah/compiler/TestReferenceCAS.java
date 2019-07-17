/*
 * Copyright (c) 2016, 2018, Red Hat, Inc. All rights reserved.
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
 * @test TestReferenceCAS
 * @summary Shenandoah reference CAS test
 * @key gc
 *
 * @run main/othervm -Diters=20000 -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC                                                 TestReferenceCAS
 * @run main/othervm -Diters=100   -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC -Xint                                           TestReferenceCAS
 * @run main/othervm -Diters=20000 -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC -XX:-TieredCompilation                          TestReferenceCAS
 * @run main/othervm -Diters=20000 -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC -XX:TieredStopAtLevel=1                         TestReferenceCAS
 * @run main/othervm -Diters=20000 -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC -XX:TieredStopAtLevel=4                         TestReferenceCAS
 *
 * @run main/othervm -Diters=20000 -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC -XX:-UseCompressedOops                          TestReferenceCAS
 * @run main/othervm -Diters=100   -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC -XX:-UseCompressedOops -Xint                    TestReferenceCAS
 * @run main/othervm -Diters=20000 -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC -XX:-UseCompressedOops -XX:-TieredCompilation   TestReferenceCAS
 * @run main/othervm -Diters=20000 -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC -XX:-UseCompressedOops -XX:TieredStopAtLevel=1  TestReferenceCAS
 * @run main/othervm -Diters=20000 -XX:+UnlockDiagnosticVMOptions -XX:+UnlockExperimentalVMOptions -XX:ShenandoahGCHeuristics=aggressive -XX:+UseShenandoahGC -XX:-UseCompressedOops -XX:TieredStopAtLevel=4  TestReferenceCAS
 */

import java.lang.reflect.Field;

public class TestReferenceCAS {

    static final int ITERS = Integer.getInteger("iters", 1);
    static final int WEAK_ATTEMPTS = Integer.getInteger("weakAttempts", 10);

    static final sun.misc.Unsafe UNSAFE;
    static final long V_OFFSET;

    static {
        try {
            Field f = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            UNSAFE = (sun.misc.Unsafe) f.get(null);
        } catch (Exception e) {
            throw new RuntimeException("Unable to get Unsafe instance.", e);
        }

        try {
            Field vField = TestReferenceCAS.class.getDeclaredField("v");
            V_OFFSET = UNSAFE.objectFieldOffset(vField);
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    Object v;

    private static void assertEquals(boolean a, boolean b, String msg) {
        if (a != b) {
            throw new RuntimeException("a (" + a + ") != b (" + b + "): " + msg);
        }
    }

    private static void assertEquals(Object a, Object b, String msg) {
        if (!a.equals(b)) {
            throw new RuntimeException("a (" + a.toString() + ") != b (" + b.toString() + "): " + msg);
        }
    }

    public static void main(String[] args) {
        TestReferenceCAS t = new TestReferenceCAS();
        for (int c = 0; c < ITERS; c++) {
            testAccess(t, V_OFFSET);
        }
    }

    static void testAccess(Object base, long offset) {
        String foo = new String("foo");
        String bar = new String("bar");
        String baz = new String("baz");
        UNSAFE.putObject(base, offset, "foo");
        {
            String newval = bar;
            boolean r = UNSAFE.compareAndSwapObject(base, offset, "foo", newval);
            assertEquals(r, true, "success compareAndSwap Object");
            assertEquals(newval, "bar", "must not destroy newval");
            Object x = UNSAFE.getObject(base, offset);
            assertEquals(x, "bar", "success compareAndSwap Object value");
        }

        {
            String newval = baz;
            boolean r = UNSAFE.compareAndSwapObject(base, offset, "foo", newval);
            assertEquals(r, false, "failing compareAndSwap Object");
            assertEquals(newval, "baz", "must not destroy newval");
            Object x = UNSAFE.getObject(base, offset);
            assertEquals(x, "bar", "failing compareAndSwap Object value");
        }
    }

}
