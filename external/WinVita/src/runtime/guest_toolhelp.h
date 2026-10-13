// src/runtime/guest_toolhelp.h — Toolhelp32 snapshot state.
//
// CreateToolhelp32Snapshot freezes a list of threads and processes, which
// Process32First/Next and Thread32First/Next then iterate. The iteration
// cursor and frozen list are OS semantics, not game logic.
//
// NOT included here: the MODULE list that Module32First/Next iterates. It
// derives from the module table each consumer's loader builds differently,
// so it stays with the consumer.
#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <vector>

struct WxSnapState {
    std::vector<uint32_t> tids; size_t tcur=0;
    std::vector<uint32_t> pids; size_t pcur=0;
};

// Snapshot table, exposed by reference for the same reason as the file
// tables: transfer ownership without rewriting call sites.
std::map<uint32_t,WxSnapState>& wx86_snapshots();
