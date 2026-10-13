#include "music/music_service.h"
#include "music/music_vita.h"

#include <memory>

namespace ign { namespace music {

namespace {
std::unique_ptr<MusicPlatform> g_plat;
std::unique_ptr<CdAudio> g_cd;
}

void service_init(const char* musicDir) {
    if (g_cd) return;
    g_plat = make_platform();
    g_cd.reset(new CdAudio(g_plat.get(), musicDir));
}
bool service_enabled() { return (bool)g_cd; }
MciResult service_exec(const char* command) { return g_cd->exec(command); }
void service_shutdown() { if (g_cd) g_cd->shutdown(); }
int service_summary(char* out, unsigned n) { return g_cd ? g_cd->summary_line(out, n) : 0; }

}} // namespace ign::music
