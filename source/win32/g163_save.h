// win32/g163_save.h -- iteration-G163, PURE (stdio only; the file operations can be replaced, tools/g163_test.cpp does it to inject failures).
//
// The game saves its configuration (ign_3dfx.btz: the options, the unlocked leagues and cars, the names) with fopen("wb") + one WriteFile of 0x597 bytes + CloseHandle (the race cleanup 0x0041CA0C, the options code 0x0041E545): the file is
// TRUNCATED when it is opened, so a write that fails half way (card full, card removed, the process killed) leaves a partial or empty save and the unlocks are lost. The port now gives the game a temporary file of the same name + ".tmp"
// to write to, and replaces the real file only when the whole block has been written (commit()). If anything fails the previous save is kept untouched. A save whose commit was interrupted between the removal of the old file and the
// rename (the only window left) is found by recover() the next time the file is opened for reading.
// Nothing here writes a log line: the counters (Stats) are summarised by the telemetry at the race boundaries.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ign { namespace g163 {

struct Stats {
    uint32_t opens = 0, writes = 0, closes = 0, commits = 0, kept_old = 0, copy_fallbacks = 0, tmp_open_failed = 0, recoveries = 0, invalid = 0, failed_commits = 0;
    uint32_t bytes = 0; int32_t last_bytes = -1; bool last_valid = false; uint32_t last_digest = 0, last_n = 0xFFFFFFFFu, last_m = 0xFFFFFFFFu; uint8_t last_result = 0;
};
inline Stats g_stats;   // one per process; filled by win32/kernel32_ign.cpp, summarised by win32/glide2x_ign.cpp

struct Ops {   // stdio by default
    int (*remove_)(const char*) = std::remove;
    int (*rename_)(const char*, const char*) = std::rename;
    FILE* (*fopen_)(const char*, const char*) = std::fopen;
};
inline const Ops& default_ops() { static const Ops o; return o; }

enum Result : uint8_t { kCommitted = 0, kCommittedByCopy, kKeptOldShortWrite, kKeptOldEmpty, kKeptOldUnreadable, kFailedTmpKept, kResultCount };
inline const char* result_name(uint32_t r) { static const char* const n[kResultCount] = {"committed", "committed_by_copy", "kept_old_short_write", "kept_old_empty", "kept_old_unreadable", "failed_tmp_kept"}; return r < kResultCount ? n[r] : "?"; }

inline std::string tmp_path(const std::string& fin) { return fin + ".tmp"; }
inline uint32_t fnv32(const uint8_t* p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; } return h; }
inline bool read_all(const std::string& path, std::vector<uint8_t>& out, const Ops& ops) {
    FILE* f = ops.fopen_(path.c_str(), "rb"); if (!f) return false;
    out.clear(); uint8_t buf[512]; size_t got; while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + got);
    const bool err = std::ferror(f) != 0; std::fclose(f); return !err;
}
inline bool exists(const std::string& path, const Ops& ops) { FILE* f = ops.fopen_(path.c_str(), "rb"); if (!f) return false; std::fclose(f); return true; }

// `tmp` is already closed. requested = the bytes the game asked to write in total, written = the bytes the host wrote, writeFailed = a short write or an error on the way.
// expectBlock: the size of a valid block (0x597) and version (0x1F) for the `valid` flag of the statistics only: the commit does not refuse another size (the game owns the format).
inline Result commit(const std::string& fin, size_t requested, size_t written, bool writeFailed, Stats& st, const Ops& ops = default_ops(), size_t expectSize = 0x597u, uint32_t expectVersion = 0x1Fu) {
    const std::string tmp = tmp_path(fin);
    auto keep_old = [&](Result r) { ops.remove_(tmp.c_str()); ++st.kept_old; st.last_result = r; return r; };
    if (writeFailed || written != requested) return keep_old(kKeptOldShortWrite);
    if (written == 0) return keep_old(kKeptOldEmpty);
    std::vector<uint8_t> data;
    if (!read_all(tmp, data, ops) || data.size() != written) return keep_old(kKeptOldUnreadable);   // what is on the card is not what was written
    st.last_bytes = (int32_t)data.size(); st.last_digest = fnv32(data.data(), data.size());
    uint32_t ver = 0; if (data.size() >= 4) std::memcpy(&ver, data.data(), 4);
    st.last_valid = data.size() == expectSize && ver == expectVersion; if (!st.last_valid) ++st.invalid;
    if (data.size() >= 0x58Fu) { std::memcpy(&st.last_n, data.data() + 0x587, 4); std::memcpy(&st.last_m, data.data() + 0x58B, 4); }
    ops.remove_(fin.c_str());                                   // the old file (a missing one is fine)
    if (ops.rename_(tmp.c_str(), fin.c_str()) == 0) { ++st.commits; st.last_result = kCommitted; return kCommitted; }
    // the rename failed: write the verified bytes to the real name; the temporary file stays until that has worked
    FILE* f = ops.fopen_(fin.c_str(), "wb");
    if (f) {
        const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0; const bool closed = std::fclose(f) == 0;
        if (ok && closed) { ops.remove_(tmp.c_str()); ++st.commits; ++st.copy_fallbacks; st.last_result = kCommittedByCopy; return kCommittedByCopy; }
    }
    ++st.failed_commits; st.last_result = kFailedTmpKept; return kFailedTmpKept;   // tmp holds the complete save; recover() promotes it
}
// Called when the game opens the file for READING: a complete temporary file with no real file next to it is the save of an interrupted commit.
inline bool recover(const std::string& fin, Stats& st, const Ops& ops = default_ops()) {
    if (exists(fin, ops)) return false;
    const std::string tmp = tmp_path(fin); if (!exists(tmp, ops)) return false;
    if (ops.rename_(tmp.c_str(), fin.c_str()) != 0) return false;
    ++st.recoveries; return true;
}

// the state of one open save handle (the handle is the key of the map in kernel32_ign.cpp)
struct Pending { std::string tmp, fin; size_t requested = 0, written = 0; bool failed = false; };
inline void note_write(Pending& p, size_t requested, size_t written, Stats& st) { p.requested += requested; p.written += written; if (written != requested) p.failed = true; ++st.writes; st.bytes += (uint32_t)written; }

} }   // namespace ign::g163
