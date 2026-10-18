package org.VitaPlex.app;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * How VitaPlex's previous runs ended, written into vitaplex.log at start.
 *
 * A crash leaves nothing in VitaPlex's own log: a native crash kills the
 * process mid-line, and a Java exception goes to logcat. One report was a log
 * that simply stopped while typing in search. Android keeps its own record of
 * every process death (Android 11+): the reason, the app's state and memory,
 * and for a native crash (Android 12+) the tombstone, with the signal and the
 * crashing thread's backtrace. Reading those back on the next start puts the
 * answer in the next log, including for a crash that happened before this
 * code shipped, as long as Android still holds the record.
 *
 * Java exceptions from now on are also written to the log as they happen.
 */
final class ExitReasons {
    private static final String TAG = "VitaPlexExit";
    private static final String PREFS = "vitaplex_exit_reasons";
    private static final String KEY_SEEN = "last_seen_timestamp";
    private static final int MAX_RECORDS = 16;
    private static final int MAX_FRAMES = 48;
    private static final int MAX_TRACE_BYTES = 8 * 1024 * 1024;

    private ExitReasons() {}

    static void start(Context ctx) {
        installCrashLogger();
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                Records.logPrevious(ctx);
            } catch (Throwable t) {
                Log.w(TAG, "exit reasons", t);
                NetworkMonitor.log("could not read how earlier runs ended: " + t);
            }
        }
    }

    // A Java exception nobody catches: write it to the log, then let Android's
    // own handler crash the app as before.
    private static void installCrashLogger() {
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override public void uncaughtException(Thread thread, Throwable e) {
                try {
                    StringWriter sw = new StringWriter();
                    e.printStackTrace(new PrintWriter(sw));
                    NetworkMonitor.log("CRASH: Java exception in thread \"" + thread.getName() + "\":");
                    String[] lines = sw.toString().split("\n");
                    for (int i = 0; i < lines.length && i < 80; i++) {
                        NetworkMonitor.log("  " + lines[i].trim());
                    }
                } catch (Throwable ignore) {
                    // Crashing anyway; logcat still gets it below.
                }
                if (previous != null) previous.uncaughtException(thread, e);
            }
        });
    }

    // Android 11+ only, in a class of its own so older versions never load the
    // ApplicationExitInfo references.
    private static final class Records {
        static void logPrevious(Context ctx) {
            ActivityManager am = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
            if (am == null) return;
            List<ApplicationExitInfo> infos =
                am.getHistoricalProcessExitReasons(ctx.getPackageName(), 0, MAX_RECORDS);
            if (infos == null || infos.isEmpty()) return;

            SharedPreferences prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
            final long seen = prefs.getLong(KEY_SEEN, 0);
            long newest = seen;

            // Newest first from Android; oldest first in the log reads better.
            for (int i = infos.size() - 1; i >= 0; i--) {
                ApplicationExitInfo info = infos.get(i);
                if (info.getTimestamp() <= seen) continue;
                newest = Math.max(newest, info.getTimestamp());
                log(info);
            }
            if (newest != seen) prefs.edit().putLong(KEY_SEEN, newest).apply();
        }

        private static void log(ApplicationExitInfo info) {
            final int reason = info.getReason();
            StringBuilder sb = new StringBuilder("earlier run ended ");
            sb.append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
                          .format(new Date(info.getTimestamp())));
            sb.append(": ").append(reasonName(reason, info.getStatus()));
            sb.append(" (app ").append(NetworkMonitor.importanceName(info.getImportance()));
            sb.append(", pss ").append(info.getPss() / 1024).append(" MB");
            sb.append(", rss ").append(info.getRss() / 1024).append(" MB)");
            String desc = info.getDescription();
            if (desc != null && !desc.isEmpty()) sb.append(": ").append(desc);
            NetworkMonitor.log(sb.toString());

            if (reason == ApplicationExitInfo.REASON_CRASH) {
                NetworkMonitor.log("  its stack trace is at the end of that run's log, if this "
                                   + "build was running then (CRASH: lines)");
            } else if (reason == ApplicationExitInfo.REASON_CRASH_NATIVE && Build.VERSION.SDK_INT >= 31) {
                byte[] trace = readTrace(info);
                if (trace != null) logTombstone(trace);
            } else if (reason == ApplicationExitInfo.REASON_ANR) {
                byte[] trace = readTrace(info);
                if (trace != null) logAnrMainThread(new String(trace, StandardCharsets.UTF_8));
            }
        }

        private static String reasonName(int reason, int status) {
            switch (reason) {
                case ApplicationExitInfo.REASON_EXIT_SELF:               return "exited (code " + status + ")";
                case ApplicationExitInfo.REASON_SIGNALED:                return "killed by signal " + status;
                case ApplicationExitInfo.REASON_LOW_MEMORY:              return "KILLED FOR LOW MEMORY";
                case ApplicationExitInfo.REASON_CRASH:                   return "CRASHED (Java)";
                case ApplicationExitInfo.REASON_CRASH_NATIVE:            return "CRASHED (native, signal " + status + ")";
                case ApplicationExitInfo.REASON_ANR:                     return "NOT RESPONDING (ANR)";
                case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE:  return "failed to start";
                case ApplicationExitInfo.REASON_PERMISSION_CHANGE:       return "stopped for a permission change";
                case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "KILLED FOR EXCESSIVE RESOURCE USE";
                case ApplicationExitInfo.REASON_USER_REQUESTED:          return "stopped by the user";
                case ApplicationExitInfo.REASON_USER_STOPPED:            return "force-stopped";
                case ApplicationExitInfo.REASON_DEPENDENCY_DIED:         return "a dependency died";
                case ApplicationExitInfo.REASON_OTHER:                   return "killed by the system";
                case 14:                                                 return "killed while frozen";
                case 15:                                                 return "stopped for a package change";
                case 16:                                                 return "stopped for an app update";
                default:                                                 return "reason " + reason;
            }
        }

        private static byte[] readTrace(ApplicationExitInfo info) {
            try (InputStream in = info.getTraceInputStream()) {
                if (in == null) return null;
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    if (out.size() > MAX_TRACE_BYTES) break;
                }
                return out.toByteArray();
            } catch (Throwable t) {
                NetworkMonitor.log("  could not read the crash report: " + t);
                return null;
            }
        }
    }

    // An ANR trace is text: every thread's stack. The main thread's is the one
    // that was stuck.
    private static void logAnrMainThread(String trace) {
        int start = trace.indexOf("\"main\"");
        if (start < 0) {
            NetworkMonitor.log("  no main thread in the ANR trace");
            return;
        }
        int end = trace.indexOf("\n\n", start);
        String[] lines = trace.substring(start, end > 0 ? end : trace.length()).split("\n");
        for (int i = 0; i < lines.length && i < 40; i++) NetworkMonitor.log("  " + lines[i]);
    }

    // ---- Native crash: the tombstone, a protobuf ----
    //
    // Field numbers from debuggerd's tombstone.proto, as compiled into the
    // framework's own TombstoneProtos: Tombstone.tid = 6, signal_info = 10,
    // abort_message = 14, causes = 15, threads = 16 (map entries: key = 1,
    // value = 2); Signal.number = 1, name = 2, code_name = 4,
    // has_fault_address = 8, fault_address = 9; Cause.human_readable = 1;
    // Thread.id = 1, name = 2, current_backtrace = 4; BacktraceFrame.rel_pc = 1,
    // function_name = 4, function_offset = 5, file_name = 6.

    private static void logTombstone(byte[] data) {
        try {
            Pb t = new Pb(data, 0, data.length);
            int tid = -1;
            String signal = null, abort = null;
            List<String> causes = new ArrayList<>();
            Map<Integer, Pb> threads = new HashMap<>();
            while (t.more()) {
                int tag = t.tag(), field = tag >>> 3, wire = tag & 7;
                if (field == 6 && wire == 0) {
                    tid = (int) t.varint();
                } else if (field == 10 && wire == 2) {
                    signal = signalLine(t.sub());
                } else if (field == 14 && wire == 2) {
                    abort = t.str();
                } else if (field == 15 && wire == 2) {
                    Pb c = t.sub();
                    while (c.more()) {
                        int ct = c.tag();
                        if ((ct >>> 3) == 1 && (ct & 7) == 2) causes.add(c.str());
                        else c.skip(ct & 7);
                    }
                } else if (field == 16 && wire == 2) {
                    Pb e = t.sub();
                    int key = -1;
                    Pb value = null;
                    while (e.more()) {
                        int et = e.tag();
                        if ((et >>> 3) == 1 && (et & 7) == 0) key = (int) e.varint();
                        else if ((et >>> 3) == 2 && (et & 7) == 2) value = e.sub();
                        else e.skip(et & 7);
                    }
                    if (value != null) threads.put(key, value);
                } else {
                    t.skip(wire);
                }
            }
            if (signal != null) NetworkMonitor.log("  " + signal);
            if (abort != null && !abort.isEmpty()) NetworkMonitor.log("  abort message: " + abort);
            for (String c : causes) NetworkMonitor.log("  cause: " + c);
            Pb crashed = threads.get(tid);
            if (crashed == null) {
                NetworkMonitor.log("  crashing thread " + tid + " not in the report ("
                                   + data.length + " bytes)");
                return;
            }
            logThread(crashed, tid);
        } catch (Throwable e) {
            NetworkMonitor.log("  could not decode the crash report (" + data.length + " bytes): " + e);
        }
    }

    private static String signalLine(Pb s) {
        int number = 0;
        String name = "", code = "";
        boolean hasFault = false;
        long fault = 0;
        while (s.more()) {
            int tag = s.tag(), field = tag >>> 3, wire = tag & 7;
            if (field == 1 && wire == 0) number = (int) s.varint();
            else if (field == 2 && wire == 2) name = s.str();
            else if (field == 4 && wire == 2) code = s.str();
            else if (field == 8 && wire == 0) hasFault = s.varint() != 0;
            else if (field == 9 && wire == 0) fault = s.varint();
            else s.skip(wire);
        }
        String line = "signal " + number + " " + name + (code.isEmpty() ? "" : " (" + code + ")");
        if (hasFault) line += String.format(Locale.US, ", fault address 0x%x", fault);
        return line;
    }

    private static void logThread(Pb th, int tid) {
        String name = "";
        List<String> frames = new ArrayList<>();
        while (th.more()) {
            int tag = th.tag(), field = tag >>> 3, wire = tag & 7;
            if (field == 2 && wire == 2) {
                name = th.str();
            } else if (field == 4 && wire == 2) {
                if (frames.size() < MAX_FRAMES) frames.add(frameLine(frames.size(), th.sub()));
                else th.skip(wire);
            } else {
                th.skip(wire);
            }
        }
        NetworkMonitor.log("  crashed in thread \"" + name + "\" (tid " + tid + "):");
        for (String f : frames) NetworkMonitor.log("    " + f);
    }

    private static String frameLine(int index, Pb f) {
        long relPc = 0, offset = 0;
        String function = "", file = "";
        while (f.more()) {
            int tag = f.tag(), field = tag >>> 3, wire = tag & 7;
            if (field == 1 && wire == 0) relPc = f.varint();
            else if (field == 4 && wire == 2) function = f.str();
            else if (field == 5 && wire == 0) offset = f.varint();
            else if (field == 6 && wire == 2) file = f.str();
            else f.skip(wire);
        }
        String line = String.format(Locale.US, "#%02d pc %08x  %s", index, relPc, file);
        if (!function.isEmpty()) line += " (" + function + "+" + offset + ")";
        return line;
    }

    // Just enough protobuf to walk the fields above: varints and
    // length-delimited fields read, everything else skipped.
    private static final class Pb {
        private final byte[] b;
        private int p;
        private final int end;

        Pb(byte[] b, int start, int end) {
            this.b = b;
            this.p = start;
            this.end = end;
        }

        boolean more() { return p < end; }

        int tag() { return (int) varint(); }

        long varint() {
            long r = 0;
            for (int shift = 0; shift < 64; shift += 7) {
                if (p >= end) throw new IllegalStateException("truncated varint");
                int x = b[p++] & 0xff;
                r |= (long) (x & 0x7f) << shift;
                if ((x & 0x80) == 0) return r;
            }
            throw new IllegalStateException("varint too long");
        }

        private int length() {
            long len = varint();
            if (len < 0 || len > end - p) throw new IllegalStateException("bad length " + len);
            return (int) len;
        }

        Pb sub() {
            int len = length();
            Pb s = new Pb(b, p, p + len);
            p += len;
            return s;
        }

        String str() {
            int len = length();
            String s = new String(b, p, len, StandardCharsets.UTF_8);
            p += len;
            return s;
        }

        void skip(int wire) {
            switch (wire) {
                case 0: varint(); break;
                case 1: advance(8); break;
                case 2: advance(length()); break;
                case 5: advance(4); break;
                default: throw new IllegalStateException("wire type " + wire);
            }
        }

        private void advance(int n) {
            if (n > end - p) throw new IllegalStateException("truncated field");
            p += n;
        }
    }
}
