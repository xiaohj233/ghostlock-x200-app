/* SPDX-License-Identifier: Apache-2.0 */
package com.ghostlock.x200.app;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** The UI index is the currently running step (0..8); 9 means all done. */
final class StageProgress {
    private static final Pattern START = Pattern.compile("^\\[GL\\] UI_STAGE START ([0-8])$");

    private StageProgress() {}

    static int fromLog(String line) {
        if (line == null) return -1;
        if (line.equals("[GL] === ALL STAGES PASS ===")) return 9;
        Matcher match = START.matcher(line);
        return match.matches() ? Integer.parseInt(match.group(1)) : -1;
    }
}
