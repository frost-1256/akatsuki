// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <cassert>
#include <cstdio>
#include <malloc.h>
using LONG = std::int32_t;
using ULONG = std::uint32_t;
using ULONGLONG = std::uint64_t;
using SIZE_T = std::size_t;
using NTSTATUS = std::int32_t;
using BOOLEAN = bool;
using VOID = void;
using HANDLE = void*;
using PVOID = void*;
struct PHYSICAL_ADDRESS { std::int64_t QuadPart; };
#define TRUE true
#define FALSE false
#define PAGED_CODE() ((void)0)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0
#define STATUS_INSUFFICIENT_RESOURCES (-1)
#define STATUS_CONFLICTING_ADDRESSES (-2)
#define STATUS_INVALID_PARAMETER (-3)
#define STATUS_DEVICE_BUSY (-4)
#define STATUS_NOT_FOUND (-5)
#define STATUS_IO_DEVICE_ERROR (-6)
#define STATUS_IO_TIMEOUT (-7)
#define STATUS_INVALID_DEVICE_STATE (-8)
#define STATUS_NOT_SUPPORTED (-9)
#define STATUS_RETRY (-10)
#define POOL_FLAG_NON_PAGED 1
#define PAGE_READWRITE 1
#define PAGE_NOCACHE 2
static std::map<void*, ULONGLONG> allocations;
static ULONGLONG nextPa = 0x80000000;
static int failAfter = -1;
static ULONG mmio[0x17000/4], cc[0x9000/4], gmu[0x1000/4];
static ULONG dropOffset = 0xFFFFFFFF;
static bool dropAlways = false, syncTimeout = false;
static ULONG mmioWrites = 0, stalls = 0;
inline bool FailAllocation() { if (failAfter < 0) return false; return failAfter-- == 0; }
inline void* ExAllocatePool2(int, SIZE_T n, ULONG) {
    if (FailAllocation()) return nullptr;
    void* p = std::calloc(1, n); if (p) allocations[p] = 0; return p;
}
inline void ExFreePoolWithTag(void* p, ULONG) { assert(allocations.erase(p)==1); std::free(p); }
inline void* MmAllocateNonCachedMemory(SIZE_T n) {
    if (FailAllocation()) return nullptr;
    void* p = _aligned_malloc(n,4096);
    if (p) { allocations[p]=nextPa; nextPa+=4096; } return p;
}
inline void MmFreeNonCachedMemory(void* p, SIZE_T) { assert(allocations.erase(p)==1); _aligned_free(p); }
inline PHYSICAL_ADDRESS MmGetPhysicalAddress(void* p) { return {static_cast<std::int64_t>(allocations.at(p))}; }
inline void RtlZeroMemory(void* p, SIZE_T n) { std::memset(p,0,n); }
inline void KeMemoryBarrier() {}
inline void KeStallExecutionProcessor(int) { ++stalls; }
inline ULONG READ_REGISTER_ULONG(volatile ULONG* p) {
    if (p == mmio+(0x107F4/4) && syncTimeout) return 1;
    return *p;
}
inline void WRITE_REGISTER_ULONG(volatile ULONG* p, ULONG v) {
    ++mmioWrites;
    if (dropOffset != 0xFFFFFFFF && p==mmio+dropOffset/4) {
        if (!dropAlways) dropOffset=0xFFFFFFFF;
        return;
    }
    *p=v;
}
inline void* MmMapIoSpaceEx(PHYSICAL_ADDRESS, SIZE_T, ULONG) { return mmio; }
inline void MmUnmapIoSpace(void*, SIZE_T) {}
inline LONG InterlockedCompareExchange(volatile LONG* p,LONG v,LONG c) {
    const LONG old=*p; if(old==c) *p=v; return old;
}
inline LONG InterlockedExchange(volatile LONG* p,LONG v) {const LONG old=*p; *p=v; return old;}
static ULONG testRemainingStack=72*1024;
inline ULONG IoGetRemainingStackSize() {return testRemainingStack;}
#ifndef NTAPI
#define NTAPI
#endif
#ifndef _Function_class_
#define _Function_class_(x)
#endif
#define MAXIMUM_EXPANSION_SIZE (72*1024-2048)
using PEXPAND_STACK_CALLOUT=VOID (NTAPI *)(PVOID);
static NTSTATUS testExpandStatus=STATUS_SUCCESS;
static ULONG testExpandCalls=0;
inline NTSTATUS KeExpandKernelStackAndCalloutEx(PEXPAND_STACK_CALLOUT callout,PVOID parameter,SIZE_T size,BOOLEAN wait,PVOID context) {
    assert(size==MAXIMUM_EXPANSION_SIZE && wait && !context); ++testExpandCalls;
    if(!NT_SUCCESS(testExpandStatus)) return testExpandStatus;
    callout(parameter); return STATUS_SUCCESS;
}
inline void* InterlockedCompareExchangePointer(void* volatile* p, void* v, void* c) {
    void* old=*p; if (old==c) *p=v; return old;
}
inline void* InterlockedExchangePointer(void* volatile* p, void* v) { void* old=*p; *p=v; return old; }
inline NTSTATUS EpiSetDword(HANDLE, const wchar_t*, ULONG) { return 0; }
inline NTSTATUS ZwFlushKey(HANDLE) { return 0; }
