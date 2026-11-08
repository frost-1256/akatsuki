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
template<class... T> inline void RtlStringCchPrintfW(wchar_t* p,SIZE_T n,const wchar_t* f,T... args) {
    _snwprintf_s(p,n,_TRUNCATE,f,args...);
}
inline void RtlStringCchCatW(wchar_t* p,SIZE_T n,const wchar_t* a) { wcsncat_s(p,n,a,_TRUNCATE); }
static ULONG gPages[8][4096];
static bool bootCompletes=true, resetFails=false;
static bool smmuEnableFails=false, smmuDisableFails=false;
static ULONG gmuWrites=0;
static ULONG* hfiMemory=nullptr;
static std::map<std::wstring,ULONG> diagnostics;
inline void* GmuMap(PHYSICAL_ADDRESS a,SIZE_T,ULONG) {
    const ULONGLONG bases[]={0x03D7D000,0x03D7E000,0x03D8E000,0x03D6D000,0x03D71000,0x03DE0000,
                            0x03D0F000,0x00152000};
    for(ULONG i=0;i<RTL_NUMBER_OF(bases);++i) if(static_cast<ULONGLONG>(a.QuadPart)==bases[i]) return gPages[i];
    assert(false); return nullptr;
}
inline void GmuMmioWrite(volatile ULONG* p,ULONG v) {
    ++gmuWrites;
    if(p==gPages[1] && v==1 && resetFails) return;
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
    if(p==gPages[2]+0x307 && v==1) gPages[5][4]|=1UL<<16; // RSCC sleep completion
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
inline NTSTATUS GmuDword(HANDLE,const wchar_t* name,ULONG v) { diagnostics[name]=v; return 0; }
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
