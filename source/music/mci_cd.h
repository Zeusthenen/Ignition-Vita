// music/mci_cd.h -- the virtual CD-audio MCI device (G132).
//   mciSendStringA("... cdaudio ...")  ->  CdAudio::exec()  ->  TOC / state machine  ->  MusicPlayer  ->  TrackNN.ogg
// exec() is called on the GUEST thread: it parses, answers from the TOC and posts at most one command to the music
// thread. It never decodes, opens an audio file, waits for the port or sleeps.
#pragma once
#include "music/cd_toc.h"
#include "music/music_player.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace ign { namespace music {

// mmsystem.h MCI error codes (MCIERR_BASE = 256). The game only tests "non-zero"; honest values are still used.
constexpr int kMciErrInvalidDeviceId  = 257;
constexpr int kMciErrUnrecognized     = 261;
constexpr int kMciErrHardware         = 262;
constexpr int kMciErrInvalidDevName   = 263;
constexpr int kMciErrDeviceOpen       = 265;
constexpr int kMciErrParamOverflow    = 268;
constexpr int kMciErrUnsupported      = 274;
constexpr int kMciErrOutOfRange       = 282;
constexpr int kMciErrBadTimeFormat    = 293;

// Bounded write of an MCI reply into a caller buffer of `dstLen` bytes: copies at most dstLen-1 characters and always
// NUL-terminates. Returns the number of bytes to write INCLUDING the NUL (0 when dst is null or dstLen is 0: nothing may
// be written). *truncated is set when the reply did not fit (the caller then reports MCIERR_PARAM_OVERFLOW).
size_t format_reply(const std::string& reply, char* dst, uint32_t dstLen, bool* truncated);

struct MciResult {
    int rc = 0;
    std::string reply;           // text for lpstrReturnString ("" = none)
    bool known = true;           // false: not one of the supported commands
};

class CdAudio {
public:
    enum State { kClosed = 0, kOpened, kStopped, kStarting, kPlaying, kStopping, kError };
    // musicDir ends with '/' ("ux0:data/ignition/Music/").
    CdAudio(MusicPlatform* plat, const std::string& musicDir);
    ~CdAudio() { shutdown(); }

    MciResult exec(const char* command);
    void shutdown();                              // = close cdaudio + join; idempotent
    State state();
    static const char* state_name(State s);
    int summary_line(char* out, unsigned n);      // 0 = nothing to report
    const Toc& toc() const { return toc_; }
    uint64_t unknown_commands() const { return unknown_.load(); }
    static std::string normalize(const char* s);  // lower-case, trimmed, single spaces

private:
    void refresh_locked();
    uint64_t pos_samples_locked();
    uint32_t pos_frames_locked();
    MciResult do_open();
    MciResult do_play(const std::vector<std::string>& tk);
    MciResult do_status(const std::vector<std::string>& tk);

    MusicPlatform* plat_;
    std::string dir_;
    MusicPlayer player_;
    std::mutex mu_;
    State state_ = kClosed;
    Toc toc_;
    bool tocTried_ = false;
    bool muted_ = false;
    int curTrack_ = 1;
    uint64_t endSample_ = 0;
    uint64_t lastPos_ = 0;                        // source samples at the last stop/end
    PlayRequest lastReq_;
    std::atomic<uint64_t> unknown_{0};
    uint32_t playsLogged_ = 0;
};

}} // namespace ign::music
