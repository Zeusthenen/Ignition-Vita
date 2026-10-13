// src/runtime/win32_shims_locale.cpp — see win32_shims_locale.h.
//
// The short aliases below (misc, cp_ansi, gread_mb...) match the names the
// bodies were originally written against, avoiding a blind rename across
// every body.
#include "win32_shims_locale.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "guest_locale.h"
#include "guest_scratch.h"
#include "guest_str.h"
#include "guest_thread_ctx.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>
using namespace d2rt;

// Local aliases, same role as in win32_shims_kernel32.cpp.
static inline uint32_t misc(uint32_t n){ return wx86_scratch_alloc(n); }
static inline uint32_t cp_ansi(){ return wx86_cp_ansi(); }
static inline uint32_t cp_oem (){ return wx86_cp_oem(); }
static inline std::string gread_mb(Cpu& c, uint32_t p, int len){ return wx86_gread_mb(c,p,len); }
static inline std::vector<uint16_t> gread_wc(Cpu& c, uint32_t p, int len){ return wx86_gread_wc(c,p,len); }
static inline void gwrite_wc(Cpu& c, uint32_t p, uint16_t w){ wx86_gwrite_wc(c,p,w); }
static inline void set_lasterr(Cpu& c, uint32_t v){ wx86_set_lasterr(c,v); }

void win32_shims_locale_install(Bridge& br){
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("KERNEL32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s); };

    // ---- console: handles, code pages, writing -------------------------------
    K("GetStdHandle",1,[](Cpu&c){ return c.arg(0)|0x10u; });
    K("SetStdHandle",2,[](Cpu&){ return 1u; });
    K("GetConsoleCP",0,[](Cpu&){ return 437u; });
    K("GetConsoleOutputCP",0,[](Cpu&){ return 437u; });
    K("GetConsoleMode",2,[](Cpu&c){ set_lasterr(c,6); return 0u; });              // no console
    K("WriteConsoleA",5,[](Cpu&c){ if(c.arg(3)) c.write_u32(c.arg(3),c.arg(2)); return 1u; });
    K("SetConsoleCtrlHandler",2,[](Cpu&){ return 1u; });

    // ---- environment: an EMPTY block, and every variable reported missing ----
    K("GetEnvironmentStringsA",0,[](Cpu&c){ uint32_t a=misc(2); c.write_u32(a,0); return a; });
    K("GetEnvironmentStringsW",0,[](Cpu&c){ uint32_t a=misc(4); c.write_u32(a,0); return a; });
    K("GetEnvironmentStrings",0,[](Cpu&c){ uint32_t a=misc(2); c.write_u32(a,0); return a; });
    K("GetEnvironmentVariableA",3,[](Cpu&c){ set_lasterr(c,203); return 0u; });   // ENVVAR_NOT_FOUND
    K("SetEnvironmentVariableA",2,[](Cpu&){ return 1u; });

    // ---- locale: code pages, identifiers, LCTYPE table -----------------------
    K("GetACP",0,[](Cpu&){ return cp_ansi(); });     // code page for the active locale
    K("GetOEMCP",0,[](Cpu&){ return cp_oem(); });    // OEM code page for the active locale
    K("GetUserDefaultLCID",0,[](Cpu&){ return wx86_locale_lcid(); });
    K("GetSystemDefaultLCID",0,[](Cpu&){ return wx86_locale_lcid(); });
    K("GetSystemDefaultLangID",0,[](Cpu&){ return wx86_locale_lcid()&0xFFFFu; });
    K("GetThreadLocale",0,[](Cpu&){ return wx86_locale_lcid(); });
    K("GetUserDefaultLangID",0,[](Cpu&){ return wx86_locale_lcid()&0xFFFFu; });   // low 16 of the active LCID
    // UI-language variants: kept coherent with the active locale rather than
    // a hardcoded constant, since some callers resolve these dynamically via
    // GetProcAddress and act on the returned language id.
    K("GetSystemDefaultUILanguage",0,[](Cpu&){ return wx86_locale_lcid()&0xFFFFu; });
    K("GetUserDefaultUILanguage",0,[](Cpu&){ return wx86_locale_lcid()&0xFFFFu; });
    // LCID -> values table lives in guest_locale.cpp (wx86_locale_info).
    auto locinfo=[](uint32_t lctype)->const char*{ return wx86_locale_info(lctype); };
    static auto LI=locinfo;
    K("GetLocaleInfoA",4,[](Cpu&c){ uint32_t lctype=c.arg(1),buf=c.arg(2); int cch=(int)c.arg(3);
        const char* v=LI(lctype);
        if(lctype & 0x20000000u){ /*LOCALE_RETURN_NUMBER*/ uint32_t n=(uint32_t)std::strtoul(v,nullptr,10); if(cch>=2) c.write_u32(buf,n); return 2u; }
        int need=(int)std::strlen(v)+1; if(cch==0) return (uint32_t)need; c.write(buf,v,need); return (uint32_t)need; });
    K("GetLocaleInfoW",4,[](Cpu&c){ uint32_t lctype=c.arg(1),buf=c.arg(2); int cch=(int)c.arg(3);
        const char* v=LI(lctype);
        if(lctype & 0x20000000u){ uint32_t n=(uint32_t)std::strtoul(v,nullptr,10); if(cch>=2) c.write_u32(buf,n); return 2u; }
        int need=(int)std::strlen(v)+1; if(cch==0) return (uint32_t)need;
        for(int i=0;i<need;i++) gwrite_wc(c,buf+2*i,(uint16_t)(uint8_t)v[i]); return (uint32_t)need; });

    // ---- strings: classification, case, ANSI/UTF-16 conversion ---------------
    // Character classification: body lives in guest_locale.cpp (wx86_ctype1).
    auto ctype1=[](uint32_t ch)->uint16_t{ return wx86_ctype1(ch); };
    static auto CT1=ctype1;
    K("GetStringTypeW",4,[](Cpu&c){ uint32_t it=c.arg(0),src=c.arg(1); int cch=(int)c.arg(2); uint32_t out=c.arg(3);
        auto w=gread_wc(c,src,cch); for(size_t i=0;i<w.size();i++) gwrite_wc(c,out+2*i, it==1?CT1(w[i]):0); return 1u; });
    K("GetStringTypeA",5,[](Cpu&c){ uint32_t it=c.arg(1),src=c.arg(2); int cch=(int)c.arg(3); uint32_t out=c.arg(4);
        auto s=gread_mb(c,src,cch); for(size_t i=0;i<s.size();i++) gwrite_wc(c,out+2*i, it==1?CT1((uint8_t)s[i]):0); return 1u; });
    K("LCMapStringW",6,[](Cpu&c){ uint32_t fl=c.arg(1),src=c.arg(2); int cch=(int)c.arg(3); uint32_t dst=c.arg(4); int cd=(int)c.arg(5);
        auto w=gread_wc(c,src,cch); int outn=(int)w.size()+(cch<0?1:0); if(cd==0) return (uint32_t)outn;
        int n=0; for(size_t i=0;i<w.size()&&n<cd;i++){ uint16_t ch=w[i];
            if((fl&0x100)&&ch>='A'&&ch<='Z') ch+=32; if((fl&0x200)&&ch>='a'&&ch<='z') ch-=32; gwrite_wc(c,dst+2*n++,ch); }
        if(cch<0&&n<cd) gwrite_wc(c,dst+2*n++,0); return (uint32_t)n; });
    K("LCMapStringA",6,[](Cpu&c){ uint32_t fl=c.arg(1),src=c.arg(2); int cch=(int)c.arg(3); uint32_t dst=c.arg(4); int cd=(int)c.arg(5);
        auto s=gread_mb(c,src,cch); int outn=(int)s.size()+(cch<0?1:0); if(cd==0) return (uint32_t)outn;
        int n=0; for(size_t i=0;i<s.size()&&n<cd;i++){ uint8_t ch=(uint8_t)s[i];
            if((fl&0x100)&&ch>='A'&&ch<='Z') ch+=32; if((fl&0x200)&&ch>='a'&&ch<='z') ch-=32; c.write(dst+n++,&ch,1); }
        if(cch<0&&n<cd){ uint8_t z=0; c.write(dst+n++,&z,1);} return (uint32_t)n; });
    K("MultiByteToWideChar",6,[](Cpu&c){ uint32_t src=c.arg(2); int cb=(int)c.arg(3); uint32_t dst=c.arg(4); int cch=(int)c.arg(5);
        auto s=gread_mb(c,src,cb); int outn=(int)s.size()+(cb<0?1:0); if(cch==0) return (uint32_t)outn;
        int n=0; for(size_t i=0;i<s.size()&&n<cch;i++) gwrite_wc(c,dst+2*n++,(uint16_t)(uint8_t)s[i]);
        if(cb<0&&n<cch) gwrite_wc(c,dst+2*n++,0); return (uint32_t)n; });
    K("WideCharToMultiByte",8,[](Cpu&c){ uint32_t src=c.arg(2); int cw=(int)c.arg(3); uint32_t dst=c.arg(4); int cb=(int)c.arg(5);
        auto w=gread_wc(c,src,cw); int outn=(int)w.size()+(cw<0?1:0); if(cb==0) return (uint32_t)outn;
        int n=0; for(size_t i=0;i<w.size()&&n<cb;i++){ uint8_t b=(uint8_t)(w[i]&0xff); c.write(dst+n++,&b,1); }
        if(cw<0&&n<cb){ uint8_t z=0; c.write(dst+n++,&z,1);} return (uint32_t)n; });
    K("CompareStringW",6,[](Cpu&c){ auto a=gread_wc(c,c.arg(2),(int)c.arg(3)<0?-1:(int)c.arg(3));
        auto b=gread_wc(c,c.arg(4),(int)c.arg(5)<0?-1:(int)c.arg(5));
        size_t i=0; for(; i<a.size()&&i<b.size(); ++i){ if(a[i]!=b[i]) break; }
        uint16_t x=i<a.size()?a[i]:0, y=i<b.size()?b[i]:0;
        return x<y?1u:(x>y?3u:2u); });                                          // CSTR_LESS/EQUAL/GREATER
    K("CompareStringA",6,[](Cpu&c){ bool ic=(c.arg(1)&1)!=0;      // NORM_IGNORECASE
        auto rd=[&](uint32_t p,int n){ std::string r; uint8_t b; for(int i=0;(n<0||i<n)&&i<0x10000;i++){ c.read(p+i,&b,1); if(!b&&n<0) break; if(n>=0&&i>=n) break; r+=(char)b; } return r; };
        std::string a=rd(c.arg(2),(int)c.arg(3)), b2=rd(c.arg(4),(int)c.arg(5));
        if(ic){ for(auto&ch:a) ch=(char)std::tolower((unsigned char)ch); for(auto&ch:b2) ch=(char)std::tolower((unsigned char)ch); }
        int r=a.compare(b2); return r<0?1u:(r==0?2u:3u); });      // CSTR_LESS/EQUAL/GREATER
    // lstrcmpA/lstrcpyA: not required by any shipped guest, kept
    // Win32-faithful regardless.
    K("lstrlenA",1,[](Cpu&c){ uint32_t p=c.arg(0); if(!p) return 0u; uint32_t n=0; uint8_t b=0;
        while(n<0x100000){ c.read(p+n,&b,1); if(!b) break; ++n; } return n; });
    K("lstrcmpA",2,[](Cpu&c){ uint32_t a=c.arg(0),b=c.arg(1);
        if(!a||!b) return a==b?0u:(a?1u:0xFFFFFFFFu);
        for(uint32_t i=0;i<0x100000;i++){ uint8_t x=0,y=0; c.read(a+i,&x,1); c.read(b+i,&y,1);
            if(x!=y) return x<y?0xFFFFFFFFu:1u;
            if(!x) return 0u; }
        return 0u; });
    K("lstrcpyA",2,[](Cpu&c){ uint32_t d=c.arg(0),s2=c.arg(1);
        if(!d||!s2) return 0u;
        for(uint32_t i=0;i<0x100000;i++){ uint8_t b=0; c.read(s2+i,&b,1); c.write(d+i,&b,1); if(!b) break; }
        return d; });                                        // Win32 returns the DESTINATION

    // ---- date/time formatting -------------------------------------------------
    auto sysnow=[](Cpu&c,uint32_t pst,int idx){ int v[8]={0};      // SYSTEMTIME or local time
        if(pst){ for(int i=0;i<8;i++){ uint16_t w; c.read(pst+2*i,&w,2); v[i]=w; } }
        else { std::time_t t=std::time(nullptr); std::tm tmv{}; localtime_r(&t,&tmv);
               v[0]=tmv.tm_year+1900; v[1]=tmv.tm_mon+1; v[3]=tmv.tm_mday; v[4]=tmv.tm_hour; v[5]=tmv.tm_min; v[6]=tmv.tm_sec; }
        return v[idx]; };
    K("GetDateFormatA",6,[sysnow](Cpu&c){ uint32_t buf=c.arg(4),cch=c.arg(5);
        char t[16]; int n=std::snprintf(t,sizeof t,"%02d/%02d/%04d",sysnow(c,c.arg(2),3),sysnow(c,c.arg(2),1),sysnow(c,c.arg(2),0))+1;
        if(!cch) return (uint32_t)n; if(!buf||(int)cch<n){ set_lasterr(c,122); return 0u; }
        c.write(buf,t,(uint32_t)n); return (uint32_t)n; });
    K("GetTimeFormatA",6,[sysnow](Cpu&c){ uint32_t buf=c.arg(4),cch=c.arg(5);
        char t[16]; int n=std::snprintf(t,sizeof t,"%02d:%02d:%02d",sysnow(c,c.arg(2),4),sysnow(c,c.arg(2),5),sysnow(c,c.arg(2),6))+1;
        if(!cch) return (uint32_t)n; if(!buf||(int)cch<n){ set_lasterr(c,122); return 0u; }
        c.write(buf,t,(uint32_t)n); return (uint32_t)n; });
}
