#include "runtime/ignition_paths.h"
#include "diagnostics/log.h"

namespace ign {

std::string translate_path(const char* guest_path) {
    std::string in(guest_path ? guest_path : "");

    // Strip a drive letter ("C:\..." / "C:/...").
    if (in.size() >= 2 && in[1] == ':') in.erase(0, 2);

    // Normalize separators, then drop any leading ones.
    std::string rel;
    rel.reserve(in.size());
    for (char c : in) rel.push_back(c == '\\' ? '/' : c);
    size_t start = rel.find_first_not_of('/');
    rel = (start == std::string::npos) ? std::string() : rel.substr(start);

    std::string host = std::string(kDataRoot) + rel;
    if (kVerboseFileIoDiagnostics) {
        IGN_LOG("[PATH]", "\"%s\" -> %s", guest_path ? guest_path : "(null)", host.c_str());
    }
    return host;
}

} // namespace ign
