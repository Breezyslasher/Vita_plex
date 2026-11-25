#pragma once

// Writes the log file from a thread of its own. Each platform's log sink
// formats the line and hands it here; written by the thread logging, a line
// cost it a write to storage (line-buffered): ~10 ms a line on the Vita's
// memory card, where the frame log had ten lines take 100 ms of the UI thread
// and every focus move pay for its "Giving focus" line.
//
// The writer wakes on each line, writes what is queued and flushes, so a line
// reaches the file within moments. An error line is written and flushed before
// the call returns (after what was queued, so the order holds): what a crash
// leaves in the file is everything up to the last error, as before.

#include <cstdio>
#include <string>

namespace vitaplex {
namespace logfile {

// Starts the writer for `file` (fully buffered from here: the writer flushes).
void start(FILE* file);

// A finished line, newline included. `now`: written and flushed before
// returning, after anything still queued.
void write(std::string line, bool now);

// Writes what is queued and stops the writer; the caller closes the file.
void stop();

}  // namespace logfile
}  // namespace vitaplex
