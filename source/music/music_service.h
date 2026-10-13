// music/music_service.h -- the single entry point the rest of the port uses for the virtual CD (G132).
// winmm_ign.cpp forwards mciSendStringA here; ignition_boot.cpp initialises it before the guest runs and shuts it
// down before dsound::shutdown(). When the service is not initialised (control run `nomusic=1`) exec() reports
// "not handled" and the caller keeps the G131 behaviour (honest generic MCI error).
#pragma once
#include "music/mci_cd.h"

namespace ign { namespace music {

void service_init(const char* musicDir);            // creates the objects; opens no file, no port, no thread
bool service_enabled();
MciResult service_exec(const char* command);        // only valid when enabled
void service_shutdown();                            // close cdaudio: stop, join the music thread, release the port
int  service_summary(char* out, unsigned n);        // 0 = nothing to report (never played)

}} // namespace ign::music
