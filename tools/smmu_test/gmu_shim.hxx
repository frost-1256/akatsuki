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
static void* t1CopyTrigger=nullptr;
static ULONG* t1CorruptTarget=nullptr;
static void* r1CopyTarget=nullptr;
static ULONG r1CopyBadDword=0xFFFFFFFF;
inline void RtlCopyMemory(void* a,const void* b,SIZE_T n) {
    std::memcpy(a,b,n);
    if(a==t1CopyTrigger && t1CorruptTarget) *t1CorruptTarget^=1;
    if(a==r1CopyTarget && r1CopyBadDword<n/4) static_cast<ULONG*>(a)[r1CopyBadDword]^=1;
}
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
static bool allowR1Writes=false,r1Completes=true,r1Draw=true,r1Ccu=true,r1Event=true,r1FenceForever=false,r1BadRptr=false;
static ULONG r1FenceDrops=0,r1ErrorBits=0,r1BadSentinel=0xFFFFFFFF,r1BadPixel=0xFFFFFFFF,r1Color=0x000033FF,r1Scatter=0xFFFFFFFF;
static ULONG r1RectEnd=12;
static std::vector<ULONG> r1Writes;
// D-1 framebuffer peer: emulate two draws, not shader/graphics instruction execution.
static bool allowD1Writes=false,d1Completes=true,d1Odd=true,d1Fences=true,d1Outside=false,d1FenceForever=false;
static bool d1MapFails=false,d1Restoring=false;
static ULONG d1RestoreDrop=0xFFFFFFFF,d1ErrorBits=0,d1FenceDrops=0,d1Maps=0,d1Unmaps=0;
static std::vector<ULONG> d1Band(0x120000/4),d1Writes;
static bool allowV1Writes=false,v1Completes=true,v1Odd=true,v1BadAlpha=false,v1Outside=false,v1CoreOut=false,v1EdgeOnly=false;
static bool v1FenceForever=false,v1BadRptr=false,v1SaveFails=false;
static LONG v1Delta[3]={};
static ULONG v1Fences=7,v1FenceDrops=0,v1ErrorBits=0,v1Scatter=0xFFFFFFFF,v1BadCore=0xFFFFFFFF;
static std::vector<ULONG> v1Writes;
// Independent barycentric reference for the peer: generic cross products, not the driver's simplified formula.
inline LONG V1PeerEdge(LONG ax,LONG ay,LONG bx,LONG by,LONG x,LONG y) {
    return (bx-ax)*(y-ay)-(by-ay)*(x-ax);
}
inline bool V1PeerColor(ULONG col,ULONG row,ULONG* value) {
    const LONG x=static_cast<LONG>(2*col+1),y=static_cast<LONG>(2*row+1);
    const LONG w[]={-V1PeerEdge(824,256,1336,256,x,y),-V1PeerEdge(1336,256,1080,0,x,y),-V1PeerEdge(1080,0,824,256,x,y)};
    if(w[0]<0 || w[1]<0 || w[2]<0) return false;
    *value=0xFF000000;
    for(ULONG channel=0;channel<3;++channel) {
        LONG c=(510*w[2-channel]+131072)/262144+v1Delta[channel];
        if(c<0) c=0; if(c>255) c=255;
        *value|=static_cast<ULONG>(c)<<(channel*8);
    }
    if(v1BadAlpha) *value&=0x00FFFFFF;
    return true;
}
static bool allowA1Writes=false,a1FenceForever=false,a1BadSq=false,a1BadBg=false,a1Outside=false,a1Odd=true,a1SaveFails=false;
static ULONG a1StopFrame=0xFFFFFFFF,a1WrongFrame=0xFFFFFFFF,a1ErrorFrame=0xFFFFFFFF,a1FenceDrops=0,a1DelayUs=0,a1Fences=3;
static ULONG a1Pending=0xFFFFFFFF;
static ULONGLONG a1Due=0;
static std::vector<ULONG> a1Writes,a1Frames,a1Progress;
static std::vector<std::vector<ULONG>> a1Vertices;
inline ULONG* GmuCfgReg(ULONG reg);
inline ULONG A1PeerColor(ULONG col,ULONG row) {
    const LONG pt[4][2]={{920,48},{1240,48},{1240,208},{920,208}};
    const ULONG tris[2][3]={{0,1,2},{0,2,3}};
    const LONG colors[4][3]={{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    const LONG x=static_cast<LONG>(col*2+1),y=static_cast<LONG>(row*2+1);
    for(const auto& tri:tris) {
        LONG w[3];
        for(ULONG j=0;j<3;++j) {const auto a=tri[(j+1)%3],b=tri[(j+2)%3]; w[j]=V1PeerEdge(pt[a][0],pt[a][1],pt[b][0],pt[b][1],x,y);}
        if(w[0]<0 || w[1]<0 || w[2]<0) continue;
        ULONG result=0xFF000000;
        for(ULONG channel=0;channel<3;++channel) {
            LONG num=0; for(ULONG j=0;j<3;++j) num+=w[j]*colors[tri[j]][channel];
            result|=static_cast<ULONG>((510*num+51200)/102400)<<((2-channel)*8);
        }
        return result;
    }
    return 0xFF202020;
}
inline void A1PeerComplete(ULONG f) {
    a1Pending=0xFFFFFFFF;
    if(f==240) {
        for(ULONG y=1072;y<1328;++y) if(a1Odd || !(y&1)) for(ULONG x=412;x<668;++x)
            d1Band[(y*4320+x*4-0x462000)/4]=A1PeerColor(x,(y-1072)/2);
        if(a1BadSq) d1Band[(1200*4320+540*4-0x462000)/4]=0;
        if(a1BadBg) d1Band[(1076*4320+414*4-0x462000)/4]=0;
        if(a1Outside) d1Band[(1072*4320+411*4-0x462000)/4]^=1;
    }
    if(a1Fences&1) g2Ring[0x705C/4]=0xCAFE0015;
    if(a1Fences&2) g2Ring[0x7060/4]=0xCAFE0016;
    g2Ring[0x7064/4]=0xA1000000|(f==a1WrongFrame ? 0xFFFF : f);
    *g2Shadow=109+9*(f+1); *GmuCfgReg(0x806)=*g2Shadow; *GmuCfgReg(0x92A)=0;
}
static bool allowT1Writes=false,t1Completes=true,t1Zero=false,t1Black=false,t1Pad=false,t1Transpose=false,t1FlipX=false,t1FlipY=false;
static bool t1Outside=false,t1BoxDamage=false,t1Odd=true,t1FenceForever=false,t1BadRemaining=false,t1SaveFails=false,t1Pending=false;
static ULONG t1Fences=7,t1FenceDrops=0,t1ErrorBits=0,t1DelayUs=0,t1BadTexel=0xFFFFFFFF;
static ULONGLONG t1Due=0;
static std::vector<ULONG> t1Writes;
inline void T1PeerComplete() {
    t1Pending=false;
    for(ULONG y=1120;y<1280;++y) if(t1Odd || !(y&1)) for(ULONG x=460;x<620;++x) {
        ULONG i=(x-460)/20,j=((y-1072)/2-24)/10;
        if(t1FlipX) i=7-i; if(t1FlipY) j=7-j; if(t1Transpose) {const ULONG temp=i; i=j; j=temp;}
        ULONG color=g2Ring[0x6C00/4+j*16+i];
        if(j*8+i==t1BadTexel) color^=1;
        if(t1Zero) color=0; if(t1Black) color=0xFF000000; if(t1Pad) color=0xFFFF00FF;
        d1Band[(y*4320+x*4-0x462000)/4]=color;
    }
    if(t1BoxDamage) d1Band[(1076*4320+414*4-0x462000)/4]^=1;
    if(t1Outside) d1Band[(1072*4320+411*4-0x462000)/4]^=1;
    if(t1Fences&1) g2Ring[0x7068/4]=0xCAFE0017;
    if(t1Fences&2) g2Ring[0x706C/4]=0xCAFE0018;
    if(t1Fences&4) g2Ring[0x7070/4]=0xCAFE0019;
    *g2Shadow=2287; *GmuCfgReg(0x806)=2287; *GmuCfgReg(0x92A)=t1BadRemaining ? 1 : 0;
}
static bool allowW1Writes=false,w1Active=false,w1FenceForever=false,w1Pending=false,w1ShadowOnly=false,w1SaveFails=false;
static ULONG w1FenceDrops=0,w1StopPub=0xFFFFFFFF,w1BadSizePub=0xFFFFFFFF,w1WrongPub=0xFFFFFFFF,w1ErrorPub=0xFFFFFFFF;
static ULONG w1DamageRing=0xFFFFFFFF,w1DamageOutside=0xFFFFFFFF,w1Cursor=2287,w1NextPacket=0,w1DelayUs=0;
static ULONG w1PendingTarget=0,w1PendingLast=0,w1PendingPub=0;
static ULONGLONG w1Due=0;
static std::vector<ULONG> w1Writes,w1Pubs,w1ReadPackets,w1Progress;
inline void W1PeerComplete(ULONG pub,ULONG target,ULONG last) {
    w1Pending=false;
    if(pub==649) T1PeerComplete(); // only the final image is rasterized by this protocol peer
    g2Ring[0x7074/4]=0xB1000000|(pub==w1WrongPub ? 0xFFFF : last);
    *g2Shadow=target; *GmuCfgReg(0x806)=target; *GmuCfgReg(0x92A)=0; w1Cursor=target;
    if(pub==649) {
        if(w1DamageRing<4096) g2Ring[w1DamageRing]^=1;
        if(w1DamageOutside>=0x4000 && w1DamageOutside<0x8000) g2Ring[w1DamageOutside/4]^=1;
    }
}
inline bool D1BandPointer(volatile ULONG* p) {
    const auto address=reinterpret_cast<std::uintptr_t>(p),base=reinterpret_cast<std::uintptr_t>(d1Band.data());
    return address>=base && address<base+0x120000;
}
inline void GmuUnmap(void* p,SIZE_T n) {
    if(p==d1Band.data()) {assert(n==0x120000); ++d1Unmaps;}
    MmUnmapIoSpace(p,n);
}
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
    if(a.QuadPart==0xE1462000) {
        assert((allowD1Writes || allowV1Writes || allowA1Writes || allowT1Writes || allowW1Writes) && size==0x120000 && protection==(PAGE_READWRITE|PAGE_NOCACHE));
        if(d1MapFails) return nullptr;
        ++d1Maps; d1Restoring=false; return d1Band.data();
    }
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
    if(D1BandPointer(p)) return *p;
    if(a1Pending!=0xFFFFFFFF && testInterruptTime>=a1Due) A1PeerComplete(a1Pending);
    if(t1Pending && testInterruptTime>=t1Due) T1PeerComplete();
    if(w1Pending && testInterruptTime>=w1Due) W1PeerComplete(w1PendingPub,w1PendingTarget,w1PendingLast);
    if(g3cPolling && g3cPending) {
        if(p==gPages[2]+0x315 && g3cShowUnmasked && g3cSample>=g3cUnmaskedDelay) *p|=1UL<<20;
        if(p==GmuCfgReg(0x201) && g3cShowStatus && g3cSample>=g3cStatusDelay) *p|=1UL<<20;
    }
    const ULONG value=READ_REGISTER_ULONG(p);
    if(g3cPolling && p==GmuCfgReg(0x201)) ++g3cSample;
    return value;
}
inline void GmuMmioWrite(volatile ULONG* p,ULONG v) {
    if(D1BandPointer(p)) {
        assert(d1Restoring);
        if(static_cast<ULONG>(p-d1Band.data())!=d1RestoreDrop) *p=v;
        return;
    }
    ++gmuWrites;
    assert(p!=GmuCfgReg(0x38)); // MASK is never written, including in G-3c
    if(p==GmuCfgReg(0x807) && allowW1Writes && w1Active) {
        assert(*GmuCfgReg(0x802)==0x20B && v<4096 && *GmuCfgReg(0x38)==0);
        w1Writes.push_back(v); g2Writes.push_back({0x807,v});
        const ULONG pub=static_cast<ULONG>(w1Pubs.size()); assert(pub<650);
        if(pub==w1BadSizePub) {
            w1Pubs.push_back(v); *p=v;
            *GmuCfgReg(0x806)=w1ShadowOnly ? w1Cursor : 4096; *g2Shadow=4096;
            gPages[2][0x313]=w1FenceForever ? 1 : 0; return;
        }
        if(w1FenceForever || w1FenceDrops) {if(w1FenceDrops) --w1FenceDrops; gPages[2][0x313]=1; return;}
        gPages[2][0x313]=0;
        const ULONG count=pub<600 ? 1 : 8,first=w1NextPacket,last=first+count-1;
        assert(v==((w1Cursor+count*9)%4096) && first==(pub<600 ? pub : 600+(pub-600)*8));
        for(ULONG i=first;i<=last;++i) {
            const ULONG expected[]={0x70BF8003,0x70016400,0,107,0x70460004,4,0x70017074,0,0xB1000000|i};
            for(ULONG j=0;j<9;++j) assert(g2Ring[(w1Cursor+(i-first)*9+j)%4096]==expected[j]);
            w1ReadPackets.push_back(i);
        }
        w1NextPacket+=count; w1Pubs.push_back(v); *p=v;
        *GmuCfgReg(0x928)=0x70016400; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=107;
        gPages[8][0x210]=1; gPages[8][0x201]=pub==w1ErrorPub ? (1UL<<9) : 0; gPages[2][0x30C]=0;
        if(pub!=w1StopPub) {
            if(w1DelayUs) {w1Pending=true; w1Due=testInterruptTime+w1DelayUs*10; w1PendingTarget=v; w1PendingLast=last; w1PendingPub=pub;}
            else W1PeerComplete(pub,v,last);
        }
        return;
    }
    if(p==GmuCfgReg(0x807) && v==2287) {
        assert(allowG2Writes && allowT1Writes && z1Authorized && *GmuCfgReg(0x38)==0);
        const ULONG packet[]={0x70BF8003,0x70016400,0,107,0x70460004,4,0x70017070,0,0xCAFE0019};
        const ULONG desc[16]={0x0C006880,0x00040008,0x20002000,0,0x70016C00,0};
        const ULONG sampler[]={0x60920,0x60,0,0};
        assert(std::memcmp(g2Ring+2278,packet,sizeof(packet))==0 && std::memcmp(g2Ring+0x6A80/4,desc,sizeof(desc))==0);
        assert(std::memcmp(g2Ring+0x6AC0/4,sampler,sizeof(sampler))==0 && g2Ring[0x6884/4]==0x13000000);
        assert(g2Ring[0x6400/4+46]==0x23C40000 && g2Ring[0x6400/4+9]==0x85400100);
        for(ULONG i=0;i<32;++i) assert(g2Ring[0x6B00/4+i]==0);
        g2Writes.push_back({0x807,v}); t1Writes.push_back(v);
        if(t1FenceForever || t1FenceDrops) {if(t1FenceDrops) --t1FenceDrops; gPages[2][0x313]=1; return;}
        gPages[2][0x313]=0; *p=v;
        *GmuCfgReg(0x928)=0x70016400; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=107;
        gPages[8][0x210]=1; gPages[8][0x201]=t1ErrorBits; gPages[2][0x30C]=0;
        if(t1Completes) {
            if(t1DelayUs) {t1Pending=true; t1Due=testInterruptTime+t1DelayUs*10;}
            else T1PeerComplete();
        }
        return;
    }
    if(p==GmuCfgReg(0x807) && v>=118 && v<=2278 && (v-109)%9==0) {
        assert(allowG2Writes && allowA1Writes && z1Authorized && *GmuCfgReg(0x38)==0);
        const ULONG f=(v-109)/9-1;
        const ULONG packet[]={0x70BF8003,0x70015600,0,61,0x70460004,4,0x70017064,0,0xA1000000|f};
        assert(std::memcmp(g2Ring+109+9*f,packet,sizeof(packet))==0 && v<4096);
        assert(g2Ring[0x5600/4]==0x70460001 && g2Ring[0x5600/4+1]==0x31 && g2Ring[0x5600/4+25]==12);
        g2Writes.push_back({0x807,v}); a1Writes.push_back(v);
        if(a1FenceForever || a1FenceDrops) {if(a1FenceDrops) --a1FenceDrops; gPages[2][0x313]=1; return;}
        gPages[2][0x313]=0;
        assert(f==a1Frames.size()); a1Frames.push_back(f);
        a1Vertices.emplace_back(g2Ring+0x58C0/4,g2Ring+0x5980/4);
        a1Vertices.back().insert(a1Vertices.back().end(),g2Ring+0x5AC0/4,g2Ring+0x5B80/4);
        *GmuCfgReg(0x928)=0x70015600; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=61; *p=v;
        gPages[8][0x210]=1; gPages[8][0x201]=f==a1ErrorFrame ? (1UL<<9) : 0; gPages[2][0x30C]=0;
        if(f!=a1StopFrame) {
            if(a1DelayUs) {a1Pending=f; a1Due=testInterruptTime+a1DelayUs*10;}
            else A1PeerComplete(f);
        }
        return;
    }
    if(p==GmuCfgReg(0x807) && v==109) {
        assert(allowG2Writes && allowV1Writes && z1Authorized && *GmuCfgReg(0x38)==0);
        const ULONG packet[]={0x70BF8003,0x70015100,0,115,0x70460004,4,0x70017058,0,0xCAFE0014};
        const ULONG fs[32]={0x2000,0x47308B04,0,0x500,0,0x03000000};
        assert(std::memcmp(g2Ring+100,packet,sizeof(packet))==0 && std::memcmp(g2Ring+0x5400/4,fs,sizeof(fs))==0);
        assert(g2Ring[0x5100/4+72]==135 && g2Ring[0x5100/4+74]==0x71008A00 && g2Ring[0x5100/4+103]==0x71009AC0);
        assert(g2Ring[0x5100/4+77]==0x84 && g2Ring[0x5100/4+79]==3 && g2Ring[0x5100/4+106]==0x84);
        assert(g2Ring[0x5480/4]==0x44070000 && g2Ring[0x5500/4]==0x44090000);
        g2Writes.push_back({0x807,v}); v1Writes.push_back(v);
        if(v1FenceForever || v1FenceDrops) {
            if(v1FenceDrops) --v1FenceDrops;
            gPages[2][0x313]=1; return;
        }
        gPages[2][0x313]=0;
        *GmuCfgReg(0x928)=0x70015100; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=115;
        if(v1Completes) {
            for(ULONG y=1072;y<1328;++y) if(v1Odd || !(y&1)) for(ULONG x=412;x<668;++x) {
                ULONG color=0; if(V1PeerColor(x,(y-1072)/2,&color)) d1Band[(y*4320+x*4-0x462000)/4]=color;
            }
            if(v1BadCore<d1Band.size()) d1Band[v1BadCore]=0;
            if(v1Outside) d1Band[(1072*4320+411*4-0x462000)/4]^=1;
            if(v1CoreOut) d1Band[(1072*4320+412*4-0x462000)/4]^=1;
            if(v1EdgeOnly) d1Band[(1072*4320+540*4-0x462000)/4]=0x12345678;
            if(v1Scatter<0x8000) g2Ring[v1Scatter/4]=0xFF123456;
            if(v1Fences&1) g2Ring[0x7050/4]=0xCAFE0012;
            if(v1Fences&2) g2Ring[0x7054/4]=0xCAFE0013;
            if(v1Fences&4) g2Ring[0x7058/4]=0xCAFE0014;
            *g2Shadow=109; *GmuCfgReg(0x806)=v1BadRptr ? 108 : 109; *GmuCfgReg(0x92A)=0;
        }
        gPages[8][0x210]=g2RbbmBusy ? 2 : 1; gPages[8][0x201]=v1ErrorBits|(g2Hang ? (1UL<<23) : 0);
        gPages[2][0x30C]=g2GmuBusy ? (1UL<<23) : 0; *p=v; return;
    }
    if(p==GmuCfgReg(0x807) && v==100) {
        assert(allowG2Writes && allowD1Writes && z1Authorized && *GmuCfgReg(0x38)==0);
        const ULONG packet[]={0x70BF8003,0x70014500,0,72,0x70460004,4,0x7001704C,0,0xCAFE0011};
        const ULONG fs[32]={0,0x204CC000,0x3E4CCCCD,0x204CC001,0x3F800000,0x204CC002,
                            0x3F800000,0x204CC003,0,0x500,0,0x03000000};
        assert(std::memcmp(g2Ring+91,packet,sizeof(packet))==0 && std::memcmp(g2Ring+0x4A00/4,fs,sizeof(fs))==0);
        assert(g2Ring[0x4500/4+29]==135 && g2Ring[0x4500/4+31]==0x71008A00 && g2Ring[0x4500/4+60]==0x71009AC0);
        assert(g2Ring[0x4500/4+33]==0x70388003 && g2Ring[0x4500/4+62]==0x70388003);
        g2Writes.push_back({0x807,v}); d1Writes.push_back(v);
        if(d1FenceForever || d1FenceDrops) {
            if(d1FenceDrops) --d1FenceDrops;
            gPages[2][0x313]=1; return;
        }
        gPages[2][0x313]=0;
        *GmuCfgReg(0x928)=0x70014500; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=72;
        if(d1Completes) {
            for(ULONG y=1072;y<1328;++y) if(d1Odd || !(y&1)) for(ULONG x=412;x<668;++x)
                d1Band[(y*4320+x*4-0x462000)/4]=0xFFFF3300;
            if(d1Outside) d1Band[(1072*4320+411*4-0x462000)/4]^=1;
            if(d1Fences) {g2Ring[0x7044/4]=0xCAFE000F; g2Ring[0x7048/4]=0xCAFE0010; g2Ring[0x704C/4]=0xCAFE0011;}
            *g2Shadow=100; *GmuCfgReg(0x806)=100; *GmuCfgReg(0x92A)=0;
        }
        gPages[8][0x210]=g2RbbmBusy ? 2 : 1; gPages[8][0x201]=d1ErrorBits|(g2Hang ? (1UL<<23) : 0);
        gPages[2][0x30C]=g2GmuBusy ? (1UL<<23) : 0; *p=v; return;
    }
    if(p==GmuCfgReg(0x807) && v==91) {
        assert(allowG2Writes && allowR1Writes && z1Authorized && *GmuCfgReg(0x38)==0);
        const ULONG packet[]={0x70BF8003,0x70014000,0,314,0x70460004,4,0x70017040,0,0xCAFE000E};
        const ULONG vs[32]={0,0,0,0x03000000};
        const ULONG fs[32]={0x3F800000,0x204CC000,0x3E4CCCCD,0x204CC001,0,0x204CC002,
                            0,0x204CC003,0,0x500,0,0x03000000};
        const ULONG vb[]={0x40800000,0x40800000,0,0x3F800000,0x41400000,0x41400000,0,0x3F800000};
        assert(std::memcmp(g2Ring+82,packet,sizeof(packet))==0 && std::memcmp(g2Ring+0x4800/4,vs,sizeof(vs))==0);
        assert(std::memcmp(g2Ring+0x4880/4,fs,sizeof(fs))==0 && std::memcmp(g2Ring+0x4900/4,vb,sizeof(vb))==0);
        assert(g2Ring[0x4000/4+304]==0x70388003 && g2Ring[0x4000/4+308]==0x70460004 && g2Ring[0x4000/4+313]==0x70268000);
        g2Writes.push_back({0x807,v}); r1Writes.push_back(v);
        if(r1FenceForever || r1FenceDrops) {
            if(r1FenceDrops) --r1FenceDrops;
            gPages[2][0x313]=1; return;
        }
        gPages[2][0x313]=0;
        *GmuCfgReg(0x928)=0x70014000; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=314;
        if(r1Completes) {
            if(r1Draw) for(ULONG y=4;y<r1RectEnd;++y) for(ULONG x=4;x<r1RectEnd;++x) g2Ring[0x4C00/4+y*16+x]=r1Color;
            if(r1BadPixel<256) g2Ring[0x4C00/4+r1BadPixel]=0;
            if(r1BadSentinel<16) g2Ring[0x5000/4+r1BadSentinel]=0;
            if(r1Scatter<0x8000) g2Ring[r1Scatter/4]=r1Color;
            if(r1Ccu) g2Ring[0x703C/4]=0xCAFE000D;
            if(r1Event) g2Ring[0x7040/4]=0xCAFE000E;
            *g2Shadow=91; *GmuCfgReg(0x806)=r1BadRptr ? 90 : 91; *GmuCfgReg(0x92A)=0;
        }
        gPages[8][0x210]=g2RbbmBusy ? 2 : 1; gPages[8][0x201]=r1ErrorBits|(g2Hang ? (1UL<<23) : 0);
        gPages[2][0x30C]=g2GmuBusy ? (1UL<<23) : 0; *p=v; return;
    }
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
        if(v==0x1D03 && allowW1Writes) w1Active=true;
        if(v>=0x1D40 && v<=0x1D4A) w1Progress.push_back(v);
        if(v>=0x1B40 && v<=0x1B4F) a1Progress.push_back(v);
        if(v==0x1931 || v==0x1A31 || v==0x1B31 || v==0x1C31 || v==0x1D31) d1Restoring=true;
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
    if(std::wcscmp(name,L"GpuGmuW1")==0 && value && size && w1SaveFails) return STATUS_IO_DEVICE_ERROR;
    if(std::wcscmp(name,L"GpuGmuT1")==0 && value && size && t1SaveFails) return STATUS_IO_DEVICE_ERROR;
    if(std::wcscmp(name,L"GpuGmuA1")==0 && value && size && a1SaveFails) return STATUS_IO_DEVICE_ERROR;
    if(std::wcscmp(name,L"GpuGmuV1")==0 && value && size && v1SaveFails) return STATUS_IO_DEVICE_ERROR;
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
#define MmUnmapIoSpace GmuUnmap
#define WRITE_REGISTER_ULONG GmuMmioWrite
#define READ_REGISTER_ULONG GmuMmioRead
#define EpiSetDword GmuDword
#define MmAllocateContiguousMemorySpecifyCache GmuAlloc
#define MmFreeContiguousMemorySpecifyCache GmuFree
