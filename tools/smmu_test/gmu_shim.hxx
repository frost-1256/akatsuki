// SPDX-License-Identifier: MIT
#pragma once
#include "nt_shim.hxx"
#include <cwchar>
#include <vector>
#include <string>
using UCHAR=unsigned char;
using WCHAR=wchar_t;
using PWCHAR=wchar_t*;
using PCWSTR=const wchar_t*;
#define _IRQL_requires_(x)
#define PASSIVE_LEVEL 0
#define PAGE_SIZE 4096
#define PAGE_READONLY 4
#define MmNonCached 0
#define NT_ASSERT(x) assert(x)
#define REG_BINARY 3
inline void RtlCopyMemory(void* a,const void* b,SIZE_T n) { std::memcpy(a,b,n); }
inline void RtlFillMemory(void* a,SIZE_T n,UCHAR value) { std::memset(a,value,n); }
template<class... T> inline void RtlStringCchPrintfW(wchar_t* p,SIZE_T n,const wchar_t* f,T... args) {
    _snwprintf_s(p,n,_TRUNCATE,f,args...);
}
inline void RtlStringCchCatW(wchar_t* p,SIZE_T n,const wchar_t* a) { wcsncat_s(p,n,a,_TRUNCATE); }
static ULONG gPages[9][4096];
static ULONG secvidPage[PAGE_SIZE/sizeof(ULONG)];
static ULONG hwcgIsdbPage[PAGE_SIZE/sizeof(ULONG)], hwcgSpPage[PAGE_SIZE/sizeof(ULONG)];
static std::vector<std::pair<ULONG,ULONG>> hwcgWrites;
static ULONG failHwcgDword=0xFFFFFFFF;
static bool failHwcgSpSet=false;
inline ULONG HwcgDword(volatile ULONG* p) {
    if(p==hwcgSpPage+0x80) return 0x1A880;
    for(ULONG i=0;i<3;++i) if(p==gPages[2]+0x309+i) return 0x23B09+i;
    for(ULONG i=0;i<PAGE_SIZE/sizeof(ULONG);++i) {
        if(p==gPages[8]+i) return i;
        if(p==hwcgIsdbPage+i) return 0x400+i;
    }
    return 0xFFFFFFFF;
}
static std::vector<ULONG> secvidWrites;
static ULONG failSecvidOffset=0xFFFFFFFF;
static ULONG addrPages[10][PAGE_SIZE/sizeof(ULONG)];
static const ULONG addrPageOffsets[]={0x02000,0x03000,0x21000,0x23000,0x27000,
                                      0x2F000,0x29000,0x25000,0x2B000,0x2D000};
static const ULONG addrDwords[]={0x0842,0x0C01,0x8601,0x8E05,0x9E01,0xBE05,
                                 0xA601,0x9601,0x0E00,0xAE01,0xB601,0xF810};
static std::vector<ULONG> addrWrites;
static std::vector<std::pair<ULONG,ULONG>> cfgWrites;
static ULONG failCfgDword=0xFFFFFFFF;
static std::vector<std::pair<ULONG,ULONG>> stageEWrites;
static ULONG failStageEDword=0xFFFFFFFF;
static bool allowStageFWrite=false, ignoreStageFWrite=false;
static std::vector<ULONG> stageFWrites;
static ULONG dropGxAtStep=0xFFFFFFFF;
static std::vector<std::pair<ULONG,ULONG>> g1Writes;
static bool allowG1Writes=false;
static ULONG failG1Dword=0xFFFFFFFF;
static const ULONG g1Dwords[]={0x830,0x831,0x800,0x801,0x802,0x804,0x805};
static bool allowG2Writes=false, g2Completes=true, g2FenceForever=false, g2Hang=false;
static bool g2RbbmBusy=false, g2GmuBusy=false;
static bool g2KeepGxOn=false;
static ULONG g2FenceDrops=0;
static ULONG* g2Ring=nullptr;
static ULONG* g2Shadow=nullptr;
static std::vector<std::pair<ULONG,ULONG>> g2Writes;
static bool allowG3Writes=false, g3Completes=true, g3Scratch=true, g3Mem=true, g3Reg=true;
static bool g3BadGuard1=false, g3BadGuard3=false, g3FenceForever=false;
static ULONG g3FenceDrops=0, g3ErrorBits=0;
static std::vector<ULONG> g3Writes;
static bool allowG3bWrites=false, g3bCompletes=true, g3bEvent=true, g3bBadGuard=false, g3bFenceForever=false;
static ULONG g3bFenceDrops=0, g3bErrorBits=0, g3bDamage=0;
static std::vector<ULONG> g3bWrites;
static bool allowG3cWrites=false, g3cCompletes=true, g3cEvent=true, g3cBadGuard=false, g3cFenceForever=false;
static bool g3cShowStatus=true, g3cShowUnmasked=true, g3cIgnoreClear=false, g3cPending=false, g3cPolling=false;
static ULONG g3cFenceDrops=0, g3cErrorBits=0, g3cDamage=0, g3cStatusDelay=0, g3cUnmaskedDelay=0, g3cSample=0;
static ULONG g3cMaskChangeStep=0xFFFFFFFF;
static std::vector<ULONG> g3cWrites, g3cClearWrites;
static bool allowG4Writes=false, g4Completes=true, g4Scratch=true, g4Mem=true, g4Event=true, g4FenceForever=false;
static bool g4Scratch2Damage=false;
static ULONG g4FenceDrops=0, g4ErrorBits=0, g4Damage=0, g4BadGuards=0;
static std::vector<ULONG> g4Writes;
static bool allowG5Writes=false, g5Completes=true, g5Flush=true, g5Event=true, g5Fill=true, g5FenceForever=false;
static bool g5BadRptr=false;
static ULONG g5FenceDrops=0, g5ErrorBits=0, g5Color=0x78563412, g5BadPixel=0xFFFFFFFF, g5BadSentinel=0xFFFFFFFF;
static std::vector<ULONG> g5Writes;
static bool allowG5bWrites=false, g5bCompletes=true, g5bFill=true, g5bEvent=true, g5bFenceForever=false, g5bBadRptr=false;
static ULONG g5bFenceDrops=0, g5bErrorBits=0, g5bBadSentinel=0xFFFFFFFF;
static std::vector<ULONG> g5bWrites;
static bool allowZ1Writes=false, z1Completes=true, z1Fill=true, z1Event=true, z1Authorized=false;
static ULONG z1Reserved[0x1000/4]={};
static std::vector<ULONG> z1Writes;
static bool allowS1Writes=false,s1Completes=true,s1Store=true,s1Event=true,s1FenceForever=false,s1BadRptr=false;
static ULONG s1FenceDrops=0,s1ErrorBits=0,s1BadGuard=0xFFFFFFFF,s1Scatter=0xFFFFFFFF;
static std::vector<ULONG> s1Writes;
inline ULONG GmuStageEReg(ULONG i) {
    const ULONG prefix[]={0x8E08,0xB604,0xAE02,0xE01,0x534,0x84F};
    return i<6 ? prefix[i] : (i==46 ? 0x87F : 0x850+i-6);
}
static const ULONG cfgDwords[]={0x011,0x50B,0xE05,0xE06,0xE09,0xE0A,0xE07,0xE08,0xE18,0xE17,
    0x8C2,0x8C1,0xB34,0x8C3,0x9E00,0x500,0x8D0,0x01F,0xE19,0xB608,0xB609,0xB60A,0xB60B,0xB60C,
    0x23B0E,0x1F841,0x1F840,0x841,0x011,0x844};
inline ULONG* GmuCfgReg(ULONG reg) {
    if(reg>=0x23800) return gPages[2]+reg-0x23800;
    if(reg>=0x1F800) return gPages[1]+reg-0x1F800;
    if(reg<0x400) return gPages[8]+reg;
    if(reg<0x800) return hwcgIsdbPage+reg-0x400;
    for(ULONG i=0;i<RTL_NUMBER_OF(addrPageOffsets);++i)
        if((reg*4&~0xFFFUL)==addrPageOffsets[i]) return addrPages[i]+(reg&0x3FF);
    assert(false); return nullptr;
}
static ULONG failAddrWrite=0xFFFFFFFF, failAddrMap=0xFFFFFFFF;
inline ULONG* GmuAddrReg(ULONG i) {
    const ULONG byte=addrDwords[i]*4, page=byte&~0xFFFUL;
    if(page==0x3E000) return secvidPage+(byte&0xFFF)/4;
    for(ULONG j=0;j<RTL_NUMBER_OF(addrPageOffsets);++j)
        if(page==addrPageOffsets[j]) return addrPages[j]+(byte&0xFFF)/4;
    assert(false); return nullptr;
}
static bool bootCompletes=true, resetFails=false;
static bool smmuEnableFails=false, smmuDisableFails=false;
static ULONG gmuWrites=0;
static ULONG failGmuStep=0xFFFFFFFF;
static bool failCxHaltClear=false, failRbbmHaltClear=false;
static ULONG gxPageProtection=0;
static const ULONG gxRegExpected[]={1,0x213,0x201,0xAE,0x16,0x17};
inline void GmuSeedGxRegs() {
    gPages[8][0x210]=gxRegExpected[0]; gPages[8][0x213]=gxRegExpected[1];
    gPages[8][0x201]=gxRegExpected[2]; gPages[8][0x0AE]=gxRegExpected[3];
    gPages[8][0x016]=gxRegExpected[4]; gPages[8][0x017]=gxRegExpected[5];
}
static ULONG* hfiMemory=nullptr;
static std::map<std::wstring,ULONG> diagnostics;
inline void* GmuMap(PHYSICAL_ADDRESS a,SIZE_T size,ULONG protection) {
    if(a.QuadPart==0x8B110000) { assert(allowZ1Writes && size==0x1000 && protection==(PAGE_READWRITE|PAGE_NOCACHE)); return z1Reserved; }
    if(a.QuadPart==0x03D01000) return hwcgIsdbPage;
    if(a.QuadPart==0x03D6A000) return hwcgSpPage;
    for(ULONG i=0;i<RTL_NUMBER_OF(addrPageOffsets);++i) if(a.QuadPart==0x03D00000+addrPageOffsets[i]) {
        assert(protection==(PAGE_READWRITE|PAGE_NOCACHE));
        return failAddrMap==i ? nullptr : addrPages[i];
    }
    if(a.QuadPart==0x03D3E000) { assert(protection==(PAGE_READWRITE|PAGE_NOCACHE)); return secvidPage; }
    if(a.QuadPart==0x03D00000) gxPageProtection=protection;
    const ULONGLONG bases[]={0x03D7D000,0x03D7E000,0x03D8E000,0x03D6D000,0x03D71000,0x03DE0000,
                            0x03D0F000,0x00152000,0x03D00000};
    for(ULONG i=0;i<RTL_NUMBER_OF(bases);++i) if(static_cast<ULONGLONG>(a.QuadPart)==bases[i]) return gPages[i];
    assert(false); return nullptr;
}
inline ULONG GmuMmioRead(volatile ULONG* p) {
    if(g3cPolling && g3cPending) {
        if(p==gPages[2]+0x315 && g3cShowUnmasked && g3cSample>=g3cUnmaskedDelay) *p|=1UL<<20;
        if(p==GmuCfgReg(0x201) && g3cShowStatus && g3cSample>=g3cStatusDelay) *p|=1UL<<20;
    }
    const ULONG value=READ_REGISTER_ULONG(p);
    if(g3cPolling && p==GmuCfgReg(0x201)) ++g3cSample;
    return value;
}
inline void GmuMmioWrite(volatile ULONG* p,ULONG v) {
    ++gmuWrites;
    assert(p!=GmuCfgReg(0x38)); // MASK is never written, including in G-3c
    if(p==GmuCfgReg(0x807) && v==82) {
        assert(allowG2Writes && allowS1Writes && z1Authorized && *GmuCfgReg(0x38)==0);
        const ULONG packet[]={0x70BF8003,0x70016300,0,53,0x70460004,4,0x70017038,0,0xCAFE000C};
        const ULONG shader[32]={0x70017C00,0x204CC000,0,0x204CC001,0xC0DE5EED,0x204CC004,
                              0,0x500,0x01800008,0xC0C60100,0,0x03000000};
        assert(std::memcmp(g2Ring+73,packet,sizeof(packet))==0 && std::memcmp(g2Ring+0x6800/4,shader,sizeof(shader))==0);
        // Check key protocol fields without pretending to execute the shader ISA on the host.
        assert(g2Ring[0x6300/4+0]==0x40AB0001 && g2Ring[0x6300/4+29]==0x70348003);
        assert(g2Ring[0x6300/4+47]==0x70B30004 && g2Ring[0x6300/4+52]==0x70268000);
        g2Writes.push_back({0x807,v}); s1Writes.push_back(v);
        if(s1FenceForever || s1FenceDrops) {
            if(s1FenceDrops) --s1FenceDrops;
            gPages[2][0x313]=1; return;
        }
        gPages[2][0x313]=0;
        *GmuCfgReg(0x928)=0x70016300; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=53;
        if(s1Completes) {
            if(s1Store) g2Ring[0x7C00/4]=0xC0DE5EED;
            if(s1BadGuard<16) g2Ring[0x7C00/4+s1BadGuard]=0;
            if(s1Scatter<0x8000) g2Ring[s1Scatter/4]=0xC0DE5EED;
            if(s1Event) g2Ring[0x7038/4]=0xCAFE000C;
            *g2Shadow=82; *GmuCfgReg(0x806)=s1BadRptr ? 81 : 82; *GmuCfgReg(0x92A)=0;
        }
        gPages[8][0x210]=g2RbbmBusy ? 2 : 1; gPages[8][0x201]=s1ErrorBits|(g2Hang ? (1UL<<23) : 0);
        gPages[2][0x30C]=g2GmuBusy ? (1UL<<23) : 0; *p=v; return;
    }
    if(p==GmuCfgReg(0x807) && v==73) {
        assert(allowG2Writes && allowZ1Writes && z1Authorized && *GmuCfgReg(0x38)==0);
        const ULONG packet[]={0x70E60001,0,0x70268000,0x70BF8003,0x70016100,0,36,0x70460004,4,0x70017034,0,0xCAFE000B};
        assert(std::memcmp(g2Ring+61,packet,sizeof(packet))==0);
        g2Writes.push_back({0x807,v}); z1Writes.push_back(v); gPages[2][0x313]=0;
        if(z1Completes) {
            if(z1Fill) for(ULONG i=0;i<256;++i) g2Ring[0x7400/4+i]=0x78563412;
            if(z1Event) g2Ring[0x7034/4]=0xCAFE000B;
            *g2Shadow=73; *GmuCfgReg(0x806)=73; *GmuCfgReg(0x928)=0x70016100; *GmuCfgReg(0x92A)=0;
        }
        gPages[8][0x210]=g2RbbmBusy ? 2 : 1; gPages[8][0x201]=g2Hang ? (1UL<<23) : 0;
        gPages[2][0x30C]=g2GmuBusy ? (1UL<<23) : 0; *p=v; return;
    }
    if(p==GmuCfgReg(0x37)) {
        assert(allowG3cWrites && v==(1UL<<20) && !g3cWrites.empty());
        g3cClearWrites.push_back(v);
        if(!g3cIgnoreClear) {
            *GmuCfgReg(0x201)&=~(1UL<<20); gPages[2][0x315]&=~(1UL<<20); g3cPending=false;
        }
        *p=v; return;
    }
    if(p==GmuCfgReg(0x808) || p==GmuCfgReg(0x807)) {
        assert(allowG2Writes); const ULONG reg=p==GmuCfgReg(0x808) ? 0x808 : 0x807;
        g2Writes.push_back({reg,v});
        assert(reg==0x808 ? v==1 : (v==9 || (allowG3Writes && v==20) || (allowG3bWrites && v==25) ||
                                  (allowG3cWrites && v==30) || (allowG4Writes && v==39) || (allowG5Writes && v==48) || (allowG5bWrites && v==61)));
        if(reg==0x807) {
            const ULONG packet[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
            assert(g2Ring && g2Shadow && std::memcmp(g2Ring,packet,sizeof(packet))==0);
            if(v==20 || v==25 || v==30 || v==39 || v==48 || v==61) {
                const ULONG extra[]={0x48088501,0xCAFE0001,0x703D8003,0x70017000,0,0xCAFE0002,
                                     0x703E8003,0x00040885,0x70017008,0,0x70268000};
                assert(std::memcmp(g2Ring+9,extra,sizeof(extra))==0);
                if(v==20) g3Writes.push_back(v);
            }
            if(v==25 || v==30 || v==39 || v==48 || v==61) {
                const ULONG event[]={0x70460004,4,0x70017010,0,0xCAFE0003};
                assert(std::memcmp(g2Ring+20,event,sizeof(event))==0);
                if(v==25) g3bWrites.push_back(v);
            }
            if(v==30 || v==39 || v==48 || v==61) {
                const ULONG irqEvent[]={0x70460004,0x80000004,0x70017018,0,0xCAFE0004};
                assert(std::memcmp(g2Ring+25,irqEvent,sizeof(irqEvent))==0 && *GmuCfgReg(0x38)==0);
                if(v==30) g3cWrites.push_back(v);
            }
            if(v==39 || v==48 || v==61) {
                const ULONG ib[]={0x48088601,0xCAFE0005,0x703D8003,0x70017020,0,0xCAFE0006};
                const ULONG call[]={0x70BF8003,0x70016000,0,6,0x70460004,4,0x70017024,0,0xCAFE0007};
                assert(std::memcmp(g2Ring+0x6000/4,ib,sizeof(ib))==0 && std::memcmp(g2Ring+30,call,sizeof(call))==0);
                if(v==39) g4Writes.push_back(v);
            }
            if(v==48 || v==61) {
                const ULONG ib[]={0x70460001,25,0x70460001,24,0x70268000,0x408E0701,0x10000000,
                    0x488C0101,0,0x408C0001,0x10F03080,0x48840001,0x10F03080,0x48ACC001,0xF180,
                    0x408C1704,0x30,0x70017400,0,1,0x488C2C04,0x12,0x34,0x56,0x78,
                    0x48840502,0,0xF000F,0x702C0001,3,0x70460004,29,0x70017028,0,0xCAFE0008,0x70268000};
                const ULONG call[]={0x70BF8003,0x70016100,0,36,0x70460004,4,0x7001702C,0,0xCAFE0009};
                assert(std::memcmp(g2Ring+0x6100/4,ib,sizeof(ib))==0 && std::memcmp(g2Ring+39,call,sizeof(call))==0);
                if(v==48) g5Writes.push_back(v);
            }
            if(v==61) {
                const ULONG ib[]={0x70460001,49,0x70268000,0x408E0401,0x04100000,0x408E0101,0,0x400E1201,1,0x70268000};
                const ULONG call[]={0x70BF8003,0x70016200,0,10,0x70BF8003,0x70016100,0,36,0x70460004,4,0x70017030,0,0xCAFE000A};
                assert(std::memcmp(g2Ring+0x6200/4,ib,sizeof(ib))==0 && std::memcmp(g2Ring+48,call,sizeof(call))==0);
                g5bWrites.push_back(v);
            }
            ULONG& drops=v==61 ? g5bFenceDrops : (v==48 ? g5FenceDrops : (v==39 ? g4FenceDrops : (v==30 ? g3cFenceDrops : (v==25 ? g3bFenceDrops : (v==20 ? g3FenceDrops : g2FenceDrops)))));
            if((v==61 ? g5bFenceForever : (v==48 ? g5FenceForever : (v==39 ? g4FenceForever : (v==30 ? g3cFenceForever : (v==25 ? g3bFenceForever : (v==20 ? g3FenceForever : g2FenceForever)))))) || drops!=0) {
                if(drops!=0) --drops;
                gPages[2][0x313]=1; return;
            }
            gPages[2][0x313]=0;
            if(v==9 && g2Completes) { *g2Shadow=9; *GmuCfgReg(0x806)=9; }
            if(v==20 && g3Completes) {
                ULONG* data=g2Ring+0x7000/4;
                if(g3Scratch) *GmuCfgReg(0x885)=0xCAFE0001;
                if(g3Mem) data[0]=0xCAFE0002;
                if(g3Reg) data[2]=*GmuCfgReg(0x885);
                if(g3BadGuard1) data[1]=0;
                if(g3BadGuard3) data[3]=0;
                *g2Shadow=20; *GmuCfgReg(0x806)=20;
            }
            if(v==25 && g3bCompletes) {
                ULONG* data=g2Ring+0x7000/4;
                if(g3bEvent) data[4]=0xCAFE0003;
                if(g3bBadGuard) data[5]=0;
                for(ULONG i=0;i<4;++i) if(g3bDamage&(1UL<<i)) data[i]^=1;
                *g2Shadow=25; *GmuCfgReg(0x806)=25;
            }
            if(v==30 && g3cCompletes) {
                ULONG* data=g2Ring+0x7000/4;
                if(g3cEvent) data[6]=0xCAFE0004;
                if(g3cBadGuard) data[7]=0;
                for(ULONG i=0;i<6;++i) if(g3cDamage&(1UL<<i)) data[i]^=1;
                *g2Shadow=30; *GmuCfgReg(0x806)=30; g3cPending=true;
                if(g3cShowUnmasked && g3cUnmaskedDelay==0) gPages[2][0x315]|=1UL<<20;
            }
            if(v==39) {
                *GmuCfgReg(0x928)=0x70016000; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=6;
                if(g4Completes) {
                    ULONG* data=g2Ring+0x7000/4;
                    if(g4Scratch) *GmuCfgReg(0x886)=0xCAFE0005;
                    if(g4Mem) data[8]=0xCAFE0006;
                    if(g4Event) data[9]=0xCAFE0007;
                    for(ULONG i=0;i<8;++i) if(g4Damage&(1UL<<i)) data[i]^=1;
                    for(ULONG i=0;i<2;++i) if(g4BadGuards&(1UL<<i)) data[10+i]=0;
                    if(g4Scratch2Damage) *GmuCfgReg(0x885)^=1;
                    *g2Shadow=39; *GmuCfgReg(0x806)=39; *GmuCfgReg(0x92A)=0;
                }
            }
            if(v==48) {
                *GmuCfgReg(0x928)=0x70016100; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=36;
                if(g5Completes) {
                    *GmuCfgReg(0x8E07)=0x10000000;
                    if(g5Fill) for(ULONG i=0;i<256;++i) g2Ring[0x7400/4+i]=g5Color;
                    if(g5BadPixel<256) g2Ring[0x7400/4+g5BadPixel]=0;
                    if(g5BadSentinel<16) g2Ring[0x7800/4+g5BadSentinel]=0;
                    if(g5Flush) g2Ring[0x7028/4]=0xCAFE0008;
                    if(g5Event) g2Ring[0x702C/4]=0xCAFE0009;
                    *g2Shadow=48; *GmuCfgReg(0x806)=g5BadRptr ? 47 : 48; *GmuCfgReg(0x92A)=0;
                }
            }
            if(v==61) {
                *GmuCfgReg(0x928)=0x70016200; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=10;
                if(g5bCompletes) {
                    *GmuCfgReg(0x8E04)=0x04100000; *GmuCfgReg(0x8E01)=0; *GmuCfgReg(0xE12)=1;
                    *GmuCfgReg(0x928)=0x70016100; *GmuCfgReg(0x8E07)=0x10000000;
                    if(g5bFill) for(ULONG i=0;i<256;++i) g2Ring[0x7400/4+i]=0x78563412;
                    if(g5bBadSentinel<16) g2Ring[0x7800/4+g5bBadSentinel]=0;
                    g2Ring[0x7028/4]=0xCAFE0008;
                    if(g5bEvent) g2Ring[0x7030/4]=0xCAFE000A;
                    *g2Shadow=61; *GmuCfgReg(0x806)=g5bBadRptr ? 60 : 61; *GmuCfgReg(0x92A)=0;
                }
            }
            gPages[8][0x210]=g2RbbmBusy ? 2 : 1;
            const ULONG other=(v==30 || v==39 || v==48 || v==61) ? gPages[8][0x201] : 0;
            gPages[8][0x201]=other | (g2Hang ? (1UL<<23) : 0) |
                            (v==61 ? g5bErrorBits : (v==48 ? g5ErrorBits : (v==39 ? g4ErrorBits : (v==30 ? g3cErrorBits : (v==25 ? g3bErrorBits : (v==20 ? g3ErrorBits : 0))))));
            if(v==30 && g3cCompletes && g3cShowStatus && g3cStatusDelay==0) gPages[8][0x201]|=1UL<<20;
            gPages[2][0x30C]=g2GmuBusy ? (1UL<<23) : 0;
        }
    }
    assert(p!=GmuCfgReg(0x885)); // scratch must be written by the packet peer, never by the CPU driver
    assert(p!=GmuCfgReg(0x886)); // G-4 scratch[3] is also written only by CP
    assert(p!=GmuCfgReg(0x8E07)); // G-5 CCU configuration is written only by the IB, never by the CPU
    assert(p!=GmuCfgReg(0x8E04) && p!=GmuCfgReg(0x8E01) && p!=GmuCfgReg(0xE12)); // G-5b magic writes are CP-only
    for(ULONG i=0;i<7;++i) if(p==GmuCfgReg(g1Dwords[i])) {
        assert(allowG1Writes); g1Writes.push_back({g1Dwords[i],v});
        if(g1Dwords[i]==failG1Dword) return;
        break;
    }
    if(p==GmuCfgReg(0x98D)) {
        assert(allowStageFWrite); stageFWrites.push_back(v);
        if(ignoreStageFWrite) return;
    }
    for(ULONG reg=0x878;reg<=0x87E;++reg) assert(p!=GmuCfgReg(reg));
    for(ULONG i=0;i<47;++i) {
        const ULONG reg=GmuStageEReg(i);
        if(p==GmuCfgReg(reg)) {
            stageEWrites.push_back({reg,v});
            if(reg==failStageEDword) return;
            break;
        }
    }
    for(ULONG i=0;i<RTL_NUMBER_OF(cfgDwords);++i) if(p==GmuCfgReg(cfgDwords[i])) {
        cfgWrites.push_back({cfgDwords[i],v});
        if(cfgDwords[i]==failCfgDword) return;
        break; // QOS occurs twice in the table, but this is one write
    }
    const ULONG hw=HwcgDword(p);
    if(hw!=0xFFFFFFFF) {
        hwcgWrites.push_back({hw,v});
        if(hw==failHwcgDword || (hw==0x1A880 && (v&1) && failHwcgSpSet)) return;
    }
    for(ULONG i=0;i<RTL_NUMBER_OF(addrDwords);++i) if(p==GmuAddrReg(i)) {
        assert(v==1); addrWrites.push_back(addrDwords[i]);
        if(i!=failAddrWrite) *p=v;
        return;
    }
    for(ULONG i=0;i<4;++i) if(p==secvidPage+i) {
        secvidWrites.push_back(i);
        if(i!=failSecvidOffset) *p=v;
        return;
    }
    if(p==gPages[1] && v==1 && resetFails) return;
    if(v==0 && ((p==gPages[6]+0x45 && failCxHaltClear) || (p==gPages[8]+0x16 && failRbbmHaltClear))) return;
    *p=v;
    if ((p==cc+0x1120/4 || p==cc+0x117C/4) && (v&1)) { *p=v&~1UL; } // RCG UPDATE self-clears
    if(p==gPages[1] && v==0 && bootCompletes) {
        gPages[1][0x1C]=0x100; gPages[1][0xEC]=0xF;
    }
    // Minimal firmware peer for the existing host smoke tests; no hardware access.
    if(p==gPages[1]+0x186 && v==1) gPages[1][0x180]=1;
    if(p==gPages[1]+0x191) gPages[1][0x192]&=~v;
    if(p==gPages[1]+0x194 && v==0x40000000UL) {
        gPages[1][0x192]|=0x80000000UL;
        cc[0x100C/4]=(cc[0x100C/4]&~1UL)|0x80000000UL;
        gPages[1][0xD0]=0; gPages[1][0xEC]=0;
    }
    if(p==gPages[6]+0x45) gPages[6][0x46]=v; // CX GBIF HALT ACK, including clear
    if(p==gPages[8]+0x16) gPages[8][0x17]=v;
    if(p==gPages[2]+0x307 && v==1) {
        gPages[5][4]|=1UL<<16; gPages[5][0x101]=1; // sleeping RSCC remains busy after REQ clears
    }
    if(p==gPages[1]+0x194 && v==1 && hfiMemory) {
        ULONG* cmd=hfiMemory+6; ULONG* rsp=hfiMemory+18;
        const ULONG header=hfiMemory[0x400+cmd[10]], w=rsp[11];
        ULONG* data=hfiMemory+0x800;
        data[w]=(1UL<<16)|(3UL<<8)|126;
        data[(w+1)&0x3FF]=header; data[(w+2)&0x3FF]=0; data[(w+3)&0x3FF]=0xFAFAFAFA;
        cmd[10]=cmd[11]; rsp[11]=(w+4)&0x3FF; gPages[1][0x192]|=1;
        if((header&0xFF)==33 && !g2KeepGxOn) {
            cc[0x100C/4]=(cc[0x100C/4]&~0x80000000UL)|1;
            gPages[1][0xD0]=0xC0; gPages[1][0xEC]=0xF;
        }
    }
}
inline NTSTATUS GmuDword(HANDLE,const wchar_t* name,ULONG v) {
    if(std::wcscmp(name,L"EpiGpuProbeStep")==0) {
        if(v==g3cMaskChangeStep) *GmuCfgReg(0x38)=1;
        if(v==0x1009) { g3cPolling=true; g3cSample=0; }
        if(v==0x100A) g3cPolling=false;
    }
    if(std::wcscmp(name,L"EpiGpuProbeStep")==0 && v==dropGxAtStep) gPages[1][0xD0]=0xC0;
    if(std::wcscmp(name,L"EpiGpuProbeStep")==0 && v==failGmuStep) return STATUS_IO_DEVICE_ERROR;
    diagnostics[name]=v; return 0;
}
static std::vector<std::vector<UCHAR>> zapQuerySnapshots;
static ULONG failZapQuerySave=0xFFFFFFFF;
inline NTSTATUS EpiSetValue(HANDLE,const wchar_t* name,ULONG,const void* value,ULONG size) {
    if(std::wcscmp(name,L"GpuZapQuery")==0 && value && size) {
        if(zapQuerySnapshots.size()==failZapQuerySave) {failZapQuerySave=0xFFFFFFFF; return STATUS_IO_DEVICE_ERROR;}
        const UCHAR* p=static_cast<const UCHAR*>(value); zapQuerySnapshots.emplace_back(p,p+size);
    }
    return 0;
}
inline void* GmuAlloc(SIZE_T n,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,int) {
    if(FailAllocation()) return nullptr;
    void* p=_aligned_malloc(n,4096); if(p) {allocations[p]=nextPa; nextPa+=(n+4095)&~4095ULL;} return p;
}
inline void GmuFree(void* p,SIZE_T,int) { assert(allocations.erase(p)==1); _aligned_free(p); }
#define MmMapIoSpaceEx GmuMap
#define WRITE_REGISTER_ULONG GmuMmioWrite
#define READ_REGISTER_ULONG GmuMmioRead
#define EpiSetDword GmuDword
#define MmAllocateContiguousMemorySpecifyCache GmuAlloc
#define MmFreeContiguousMemorySpecifyCache GmuFree
