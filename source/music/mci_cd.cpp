#include "music/mci_cd.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ign { namespace music {

namespace {
constexpr uint32_t kTolFrames = 1;     // `from` may differ from a track start by <= 1 frame (MSF rounding), never more

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == ' ') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } } else cur.push_back(c); }
    if (!cur.empty()) v.push_back(cur);
    return v;
}
bool parse_uint(const std::string& s, unsigned* v) {
    if (s.empty() || s.size() > 4) return false;
    unsigned x = 0; for (char c : s) { if (c < '0' || c > '9') return false; x = x * 10 + (unsigned)(c - '0'); }
    *v = x; return true;
}
}

size_t format_reply(const std::string& reply, char* dst, uint32_t dstLen, bool* truncated) {
    if (truncated) *truncated = false;
    if (!dst || dstLen == 0) return 0;
    size_t n = reply.size();
    if (n > (size_t)dstLen - 1) { n = (size_t)dstLen - 1; if (truncated) *truncated = true; }
    memcpy(dst, reply.data(), n);
    dst[n] = 0;
    return n + 1;
}

std::string CdAudio::normalize(const char* s) {
    std::string out; bool space = true;
    for (const char* p = s ? s : ""; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!space) { out.push_back(' '); space = true; } }
        else { out.push_back((char)tolower(c)); space = false; }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

const char* CdAudio::state_name(State s) {
    switch (s) { case kClosed: return "closed"; case kOpened: return "opened"; case kStopped: return "stopped"; case kStarting: return "starting";
                 case kPlaying: return "playing"; case kStopping: return "stopping"; default: return "error"; }
}

CdAudio::CdAudio(MusicPlatform* plat, const std::string& musicDir) : plat_(plat), dir_(musicDir), player_(plat) {}

CdAudio::State CdAudio::state() { std::lock_guard<std::mutex> lk(mu_); refresh_locked(); return state_; }

void CdAudio::refresh_locked() {
    if (state_ != kStarting && state_ != kPlaying) return;
    switch (player_.run()) {
        case MusicPlayer::kPlaying: state_ = kPlaying; break;
        case MusicPlayer::kEnded:   state_ = kStopped; lastPos_ = endSample_; break;           // end of the requested range
        case MusicPlayer::kFailed:  state_ = player_.backend_failed() ? kError : kStopped; break;
        case MusicPlayer::kIdle:    state_ = kStopped; break;
        default: break;                                                                       // kStarting
    }
}

uint64_t CdAudio::pos_samples_locked() {
    if (state_ == kPlaying || state_ == kStarting) {
        const uint64_t s = out_frames_to_src_samples(player_.delivered_out_frames(), lastReq_.srcRate);
        return s < endSample_ ? s : endSample_;
    }
    return lastPos_;
}
uint32_t CdAudio::pos_frames_locked() {
    const TrackInfo& t = toc_.t[curTrack_];
    if (t.data || !t.rate) return t.startFrame;
    uint32_t f = t.startFrame + (uint32_t)(pos_samples_locked() * kCdFps / t.rate);
    return f < t.endFrame ? f : t.endFrame;
}

MciResult CdAudio::exec(const char* command) {
    MciResult r;
    const std::string norm = normalize(command);
    const std::vector<std::string> tk = split(norm);
    if (tk.size() < 2) { r.rc = kMciErrUnrecognized; r.known = false; unknown_.fetch_add(1); return r; }
    if (tk[1] != "cdaudio") { r.rc = kMciErrInvalidDevName; r.known = false; unknown_.fetch_add(1); return r; }
    const std::string& verb = tk[0];
    std::lock_guard<std::mutex> lk(mu_);
    refresh_locked();
    if (verb == "open") {
        if (tk.size() != 2) { r.rc = kMciErrUnsupported; r.known = false; unknown_.fetch_add(1); return r; }
        return do_open();
    }
    if (state_ == kClosed) { r.rc = kMciErrInvalidDevName; return r; }          // not opened: the alias does not exist
    if (verb == "close" && tk.size() == 2) {
        player_.shutdown();
        state_ = kClosed; lastPos_ = 0;
        return r;
    }
    if (verb == "stop" && tk.size() == 2) {
        if (state_ == kStarting || state_ == kPlaying) { lastPos_ = pos_samples_locked(); player_.post_stop(); state_ = kStopped; }
        return r;
    }
    if (verb == "set") {
        if (tk.size() == 5 && tk[2] == "time" && tk[3] == "format") {
            if (tk[4] == "msf") return r;
            r.rc = kMciErrBadTimeFormat; return r;                                // only MSF exists in this device
        }
        if (tk.size() == 5 && tk[2] == "audio" && tk[3] == "all" && (tk[4] == "on" || tk[4] == "off")) {
            muted_ = (tk[4] == "off"); player_.set_mute(muted_);                  // music only; DirectSound effects untouched
            return r;
        }
        r.rc = kMciErrUnsupported; r.known = false; unknown_.fetch_add(1); return r;
    }
    if (verb == "status") return do_status(tk);
    if (verb == "play") return do_play(tk);
    r.rc = kMciErrUnrecognized; r.known = false; unknown_.fetch_add(1);
    return r;
}

MciResult CdAudio::do_open() {
    MciResult r;
    if (state_ != kClosed) { r.rc = kMciErrDeviceOpen; return r; }
    if (!tocTried_ || !toc_.valid) {
        tocTried_ = true;
        std::vector<std::string> log; std::string err;
        const bool ok = build_toc(dir_, &toc_, &log, &err);
        for (const std::string& l : log) plat_->log(l.c_str());
        if (!ok) {
            plat_->log(("open cdaudio REFUSED: no valid music collection (" + err + ") -> the game will see no CD").c_str());
            tocTried_ = false;                                                    // a later open may retry (files copied meanwhile)
            r.rc = kMciErrHardware;
            return r;
        }
        char b[200]; snprintf(b, sizeof b, "backend ready: dir=%s decoder=stb_vorbis 1.22 (buffer %u B) port=MAIN %d Hz block=%d; nothing is opened until the first play",
                              dir_.c_str(), OggSource::buffer_bytes(), kOutRate, kBlockFrames);
        plat_->log(b);
    }
    state_ = kOpened; curTrack_ = 1; lastPos_ = 0;
    return r;
}

MciResult CdAudio::do_status(const std::vector<std::string>& tk) {
    MciResult r;
    auto is = [&](size_t i, const char* w) { return tk.size() > i && tk[i] == w; };
    if (tk.size() == 4 && is(2, "media") && is(3, "present")) {
        if (state_ == kError || !toc_.valid) { r.rc = kMciErrHardware; return r; }
        r.reply = "true"; return r;
    }
    if (state_ == kError) { r.rc = kMciErrHardware; return r; }                   // every other query depends on the backend
    if (tk.size() == 5 && is(2, "number") && is(3, "of") && is(4, "tracks")) { r.reply = std::to_string(kTracks); return r; }
    if (tk.size() == 5 && is(2, "position") && is(3, "track")) {
        unsigned n; if (!parse_uint(tk[4], &n) || n < 1 || n > (unsigned)kTracks) { r.rc = kMciErrOutOfRange; return r; }
        r.reply = frames_to_msf(toc_.t[n].startFrame); return r;
    }
    if (tk.size() == 3 && is(2, "length")) { r.reply = frames_to_msf(toc_.totalFrames); return r; }
    if (tk.size() == 5 && is(2, "length") && is(3, "track")) {
        unsigned n; if (!parse_uint(tk[4], &n) || n < 1 || n > (unsigned)kTracks) { r.rc = kMciErrOutOfRange; return r; }
        r.reply = frames_to_msf(toc_.t[n].lengthFrames); return r;
    }
    if (tk.size() == 3 && is(2, "position")) { r.reply = frames_to_msf(pos_frames_locked()); return r; }
    if (tk.size() == 4 && is(2, "current") && is(3, "track")) { r.reply = std::to_string(curTrack_); return r; }
    if (tk.size() == 3 && is(2, "mode")) { r.reply = state_ == kPlaying ? "playing" : "stopped"; return r; }
    r.rc = kMciErrUnsupported; r.known = false; unknown_.fetch_add(1);
    return r;
}

MciResult CdAudio::do_play(const std::vector<std::string>& tk) {
    MciResult r;
    if (!(tk.size() == 6 && tk[2] == "from" && tk[4] == "to")) { r.rc = kMciErrUnsupported; r.known = false; unknown_.fetch_add(1); return r; }
    uint32_t a = 0, b = 0; bool oor = false;
    if (!msf_to_frames(tk[3].c_str(), &a, &oor) || !msf_to_frames(tk[5].c_str(), &b, &oor)) { r.rc = oor ? kMciErrOutOfRange : kMciErrUnrecognized; return r; }
    if (state_ == kError || !toc_.valid) { r.rc = kMciErrHardware; return r; }
    // `from` must be the start of a track (within one frame): never silently play another track for an arbitrary value
    int n = 0;
    for (int i = 1; i <= kTracks; ++i) {
        const uint32_t s = toc_.t[i].startFrame;
        if (a + kTolFrames >= s && a <= s + kTolFrames) { n = i; break; }
    }
    if (!n) { r.rc = kMciErrOutOfRange; return r; }
    const TrackInfo& t = toc_.t[n];
    if (t.data) { r.rc = kMciErrHardware; return r; }                              // the data track has no audio
    if (b + kTolFrames < t.startFrame || b + 1 > t.endFrame + kTolFrames) { r.rc = kMciErrOutOfRange; return r; }   // `to` outside this track (inclusive end)
    uint64_t endSample = t.samples;
    const uint32_t relFrames = b >= t.startFrame ? b - t.startFrame + 1 : 0;
    if (relFrames == 0) { r.rc = kMciErrOutOfRange; return r; }
    if (relFrames < t.lengthFrames) { const uint64_t s = frames_to_samples_ceil(relFrames, t.rate); if (s < endSample) endSample = s; }
    if (state_ == kStarting && curTrack_ == n && endSample == endSample_) return r;   // the game polled again while the stream starts: no restart
    PlayRequest req; req.track = n; req.path = t.path; req.srcRate = t.rate; req.endSample = endSample;
    player_.set_mute(muted_);
    if (!player_.post_play(req)) { state_ = kError; r.rc = kMciErrHardware; plat_->log(("play: backend unavailable: " + player_.last_error()).c_str()); return r; }
    state_ = kStarting; curTrack_ = n; endSample_ = endSample; lastReq_ = req; lastPos_ = 0;
    if (playsLogged_ < 40) {
        ++playsLogged_;
        char bb[260]; snprintf(bb, sizeof bb, "play cdaudio from %s to %s -> track %d (%s), %llu of %llu samples (%.2f s)%s", tk[3].c_str(), tk[5].c_str(), n, t.path.c_str(),
                               (unsigned long long)endSample, (unsigned long long)t.samples, (double)endSample / (double)t.rate, muted_ ? " [muted]" : "");
        plat_->log(bb);
        if (playsLogged_ == 40) plat_->log("(further plays are counted in the 30 s summary, not logged)");
    }
    return r;
}

void CdAudio::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    player_.shutdown();
    if (state_ != kClosed && plat_) plat_->log("cdaudio closed: music thread joined, decoder and port released");
    state_ = kClosed;
}

int CdAudio::summary_line(char* out, unsigned n) {
    uint64_t pos, len; int tr; bool mu;
    { std::lock_guard<std::mutex> lk(mu_); refresh_locked(); pos = pos_samples_locked(); tr = curTrack_; mu = muted_;
      len = toc_.valid ? toc_.t[tr].samples : 0; }
    char body[900];
    const int k = player_.summary_line(body, sizeof body, tr, pos, len, mu);
    if (k <= 0) return 0;
    return snprintf(out, n, "state=%s unknown_cmds=%llu | %s", state_name(state()), (unsigned long long)unknown_.load(), body);
}

}} // namespace ign::music
