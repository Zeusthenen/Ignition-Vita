// music/music_vita.h -- creates the Vita implementation of MusicPlatform (second sceAudioOut port: MAIN, 48 kHz).
#pragma once
#include "music/music_platform.h"
#include <memory>

namespace ign { namespace music {
std::unique_ptr<MusicPlatform> make_platform();
}}
