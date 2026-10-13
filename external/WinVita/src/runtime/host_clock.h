// src/runtime/host_clock.h — the host's MONOTONIC clock.
//
// Not to be confused with the GUEST clock (the GetTickCount the game reads,
// which can be virtual, scaled, or frozen by a test bench). This one is
// real elapsed time from the machine's point of view: it measures host
// durations — an audio grain, a wait, a budget.
//
// This is an engine primitive for a simple but sufficient reason: its
// implementation depends on the PLATFORM, not the game. On console it's the
// Sony kernel clock, elsewhere it's the libc CLOCK_MONOTONIC. A port has
// nothing useful to add here.
//
// Monotonic: never goes backward, unaffected by wall-clock changes. No
// guarantee on the origin — only DIFFERENCES are meaningful.
#pragma once
#include <cstdint>

// Microseconds since an arbitrary origin.
uint64_t wx86_now_us();
// Milliseconds since the same origin (wx86_now_us()/1000).
uint64_t wx86_now_ms();
