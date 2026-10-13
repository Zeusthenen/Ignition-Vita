#include "guest_toolhelp.h"

std::map<uint32_t,WxSnapState>& wx86_snapshots(){
    static std::map<uint32_t,WxSnapState> t; return t;
}
