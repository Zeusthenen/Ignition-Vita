// src/runtime/guest_files.h — guest file-handle tables.
//
// A Win32 file handle, its logical position, its resolved path, and its
// mapping views are OS semantics, not game logic, so they live in the
// engine. Path resolution (where data lives, platform naming conventions,
// root) stays with the consumer.
//
// Tables are exposed by reference rather than through an accessor family:
// this transfers ownership without rewriting call sites on a hot I/O path.
// Accessors could replace this later without changing ownership.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

// Logical position tracked without a syscall; fseek only happens when needed.
struct WxFPos { long logical=0; long real=0; int dir=0; };  // dir: 0 none, 1 read, 2 write
// File mapping view.
struct WxFMap { std::string path; uint32_t size; uint32_t view; };

std::map<uint32_t,FILE*>&       wx86_files();        // handle -> host stream
std::map<uint32_t,std::string>& wx86_file_paths();   // handle -> resolved path
std::map<uint32_t,WxFPos>&      wx86_file_pos();     // handle -> logical position
std::map<uint32_t,WxFMap>&      wx86_file_maps();    // mapping handle -> view

// Mapping-handle counter, kept in a separate namespace from waitable-object
// handles so the two handle spaces can't collide.
uint32_t wx86_fmap_next_id();
