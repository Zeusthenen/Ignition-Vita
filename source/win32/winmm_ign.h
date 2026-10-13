// win32/winmm_ign.h — the exact WINMM entry points Ign_d3d.exe imports
// (per the binary's own import table: joyGetPosEx, joyGetDevCapsA,
// mciSendStringA, timeKillEvent, timeBeginPeriod, timeSetEvent,
// timeEndPeriod, timeGetTime). WinVita's engine implements none of WINMM.
#pragma once

namespace d2rt { class Bridge; }

namespace ign {

// Timers are backed by WinVita's own host clock (wx86_now_ms); joystick and
// MCI/CD-audio are honest "no driver" failures for now (see README for the
// later phase that redirects mciSendStringA's cdaudio commands to OGG).
void winmm_ign_install(d2rt::Bridge& br);

} // namespace ign
