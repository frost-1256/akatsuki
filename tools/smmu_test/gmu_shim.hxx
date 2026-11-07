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
#define MmNonCached 0
#define NT_ASSERT(x) assert(x)
#define REG_BINARY 3
inline void RtlCopyMemory(void* a,const void* b,SIZE_T n) { std::memcpy(a,b,n); }
template<class... T> inline void RtlStringCchPrintfW(wchar_t* p,SIZE_T n,const wchar_t* f,T... args) {
    _snwprintf_s(p,n,_TRUNCATE,f,args...);
}
inline void RtlStringCchCatW(wchar_t* p,SIZE_T n,const wchar_t* a) { wcsncat_s(p,n,a,_TRUNCATE); }
static ULONG gPages[5][4096];
static bool bootCompletes=true, resetFails=false;
static bool smmuEnableFails=false, smmuDisableFails=false;
static ULONG gmuWrites=0;
static std::map<std::wstring,ULONG> diagnostics;
inline void* GmuMap(PHYSICAL_ADDRESS a,SIZE_T,ULONG) {
    const ULONGLONG bases[]={0x03D7D000,0x03D7E000,0x03D8E000,0x03D6D000,0x03D71000};
    for(ULONG i=0;i<5;++i) if(static_cast<ULONGLONG>(a.QuadPart)==bases[i]) return gPages[i];
    assert(false); return nullptr;
}
inline void GmuMmioWrite(volatile ULONG* p,ULONG v) {
    ++gmuWrites;
    if(p==gPages[1] && v==1 && resetFails) return;
    *p=v;
    if(p==gPages[1] && v==0 && bootCompletes) gPages[1][0x1C]=0x100;
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
