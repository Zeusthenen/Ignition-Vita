// diagnostics/log.h -- the persistent, tagged diagnostic log (G134: G132's simple open/append/close logger, relocated and thinned).
//
// What this is, and is not (G134, after G133's buffered engine was withdrawn):
//   * Every line that passes the static allowlist is written with fopen(append)+fprintf+fclose, at once, so the last line written
//     survives a hard crash. No buffering, no background writer, no dynamic memory, no static constructors, nothing runs before main().
//   * The files live in ux0:data/ignition/logs/ (boot.txt, ignitionvita.log, ignition_critical.log, ignition_session_id.txt). main()
//     calls logs_prepare() as its very first action: it creates the folder (an existing one is fine) and rotates current -> previous
//     (*.previous.*, one generation). If the folder cannot be created the legacy paths in ux0:data/ignition/ are used (explicit,
//     decided there, recorded in a [G134] line).
//   * Only the thread that called logs_prepare() (main() -- which is also the guest's main thread) ever touches the disk. Any other
//     thread (audio sink, music thread, guest CreateThread threads) copies its already-allowed line into a small lock-free static
//     ring; the main thread writes it on its next log call. A full ring drops the line and counts it.
//   * The allowlist (log.cpp) keeps a few dozen lines per run: errors, lifecycle, checkpoints (IGN_CP) and a handful of capped
//     milestones that the frozen modules already emit. Everything else is dropped before it is formatted.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ign {

// First action of main(): creates ux0:data/ignition/logs/ and rotates the previous generation. Idempotent. Returns false when the
// folder could not be created or written (legacy paths are then used for ignitionvita.log / ignition_critical.log /
// ignition_session_id.txt; boot.txt keeps its literal logs/ path inside WinVita).
bool logs_prepare();
// One-line description of what logs_prepare() did (for the first checkpoint).
void logs_describe(char* out, size_t n);

// Marks this process's log as started. Never truncates: the previous session was already rotated by logs_prepare().
void log_init();

void log_line(const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// iteration-G138: true when the (static) main-log allowlist has a rule for this tag, i.e. when log_line(tag, ...) can ever write it (caps and message filters still apply inside log_line).
// Lets a hot call site skip formatting AND argument evaluation for a tag that is always dropped. Pure function of the tag text; no state.
bool log_tag_enabled(const char* tag);

uint32_t ignition_session_id();

void critical_log(const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// G134 checkpoint: ONE line, "#<n> <name> | <detail>", written at once to BOTH ignitionvita.log and ignition_critical.log.
// For stages (not frames/triangles/samples/resources). The highest # in either file is the last stage completed.
void checkpoint(const char* name, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// Writes whatever other threads queued (main thread only; other threads: no-op). Called implicitly by every log call on the main thread.
void log_drain_pending();

// G150-BEGIN
// iteration-G150: log batches. Between log_batch_begin() and log_batch_end() the lines of the calling (owner) thread are collected in memory and written with one open/append/close per file when the outermost batch
// ends (the card costs 11-20 ms per open/close, whatever the line length). Same text, same files, same order. Lines to the critical log (errors, halts, checkpoints) still reach the card at once.
void log_batch_begin();
void log_batch_end();
void log_batch_set_enabled(bool on);   // control file token g150-logbatch=off (A/B): batches become no-ops, every line is written at once like before
void log_get_batch_stats(uint32_t* flushes, uint32_t* lines, uint64_t* totalUs, uint64_t* maxUs);
struct LogBatch { LogBatch() { log_batch_begin(); } ~LogBatch() { log_batch_end(); } LogBatch(const LogBatch&) = delete; LogBatch& operator=(const LogBatch&) = delete; };
// G150-END
// G151-BEGIN
// iteration-G151: release windows. While the filter is set, log_line drops the lines of a race window (tags ending in -RACE] and [G146-ELIGIBILITY]); the caller sets it around the lines of one window only.
void log_set_window_filter(bool suppress);
uint32_t log_window_lines_dropped();
// G151-END
struct LogStats { uint32_t mainLines, critLines, ringQueued, ringDrops, suppressed; };
void log_get_stats(LogStats* out);

// Kept for kernel32_ign.cpp's [G74-LOGGER-PERF] readers (frozen): cost of the write path on the main thread.
void log_get_perf_stats(uint64_t* out_total_us, uint64_t* out_max_us, uint64_t* out_line_count);

inline constexpr bool kG75LowOverheadRuntimeLog = true;
inline constexpr bool kG78CentralLogFilterEnabled = true;

} // namespace ign

#define IGN_LOG(tag, ...) ::ign::log_line(tag, __VA_ARGS__)
#define IGN_CRITICAL(tag, ...) ::ign::critical_log(tag, __VA_ARGS__)
#define IGN_CP(name, ...) ::ign::checkpoint(name, __VA_ARGS__)
