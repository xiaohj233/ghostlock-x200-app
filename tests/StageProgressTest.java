/* SPDX-License-Identifier: Apache-2.0 */
package com.ghostlock.x200.app;

public final class StageProgressTest {
    private static void expect(int expected, String line) {
        if (StageProgress.fromLog(line) != expected) throw new AssertionError(line);
    }
    public static void main(String[] args) {
        for (int i = 0; i <= 8; i++) expect(i, "[GL] UI_STAGE START " + i);
        expect(-1, "[GL] STAGE4 OK: 1 candidate(s)");
        expect(-1, "[GL] STAGE5 OK (round 1)");
        expect(-1, "[GL] STAGE6.0a OK");
        expect(-1, "[GL] STAGE7 FAIL");
        expect(-1, "[GL] UI_STAGE START 9");
        expect(-1, "unrelated UI_STAGE START 1");
        expect(-1, null);
        expect(9, "[GL] === ALL STAGES PASS ===");
        System.out.println("progress parser: 17 cases PASS");
    }
}
