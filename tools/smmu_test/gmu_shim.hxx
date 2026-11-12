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
inline void* GmuMap(PHYSICAL_ADDRESS a,SIZE_T,ULONG protection) {
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
inline void GmuMmioWrite(volatile ULONG* p,ULONG v) {
    ++gmuWrites;
    assert(p!=GmuCfgReg(0x98D)); // stage D must never enable CP_AHB_CNTL
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
        if((header&0xFF)==33) {
            cc[0x100C/4]=(cc[0x100C/4]&~0x80000000UL)|1;
            gPages[1][0xD0]=0xC0; gPages[1][0xEC]=0xF;
        }
    }
}
inline NTSTATUS GmuDword(HANDLE,const wchar_t* name,ULONG v) {
    if(std::wcscmp(name,L"EpiGpuProbeStep")==0 && v==failGmuStep) return STATUS_IO_DEVICE_ERROR;
    diagnostics[name]=v; return 0;
}
inline NTSTATUS EpiSetValue(HANDLE,const wchar_t*,ULONG,const void*,ULONG) { return 0; }
inline void* GmuAlloc(SIZE_T n,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,int) {
    if(FailAllocation()) return nullptr;
    void* p=_aligned_malloc(n,4096); if(p) {allocations[p]=nextPa; nextPa+=(n+4095)&~4095ULL;} return p;
}
inline void GmuFree(void* p,SIZE_T,int) { assert(allocations.erase(p)==1); _aligned_free(p); }
#define MmMapIoSpaceEx GmuMap
#define WRITE_REGISTER_ULONG GmuMmioWrite
#define EpiSetDword GmuDword
#define MmAllocateContiguousMemorySpecifyCache GmuAlloc
#define MmFreeContiguousMemorySpecifyCache GmuFree
