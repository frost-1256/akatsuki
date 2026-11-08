// SPDX-License-Identifier: MIT
#define EPI_GMU_HOST_TEST
#include "../../src/epinephrine/epi_gmu.cxx"
#include <fstream>
#include <iterator>
struct EPI_SMMU { bool enabled; };
NTSTATUS EpiSmmuCreate(HANDLE,EPI_SMMU** c) {
    *c=static_cast<EPI_SMMU*>(ExAllocatePool2(0,sizeof(EPI_SMMU),0)); return *c ? 0 : STATUS_INSUFFICIENT_RESOURCES;
}
NTSTATUS EpiSmmuMap(EPI_SMMU*,ULONGLONG iova,PHYSICAL_ADDRESS pa,SIZE_T,ULONG) {
    if(iova==0x60008000) {
        for(const auto& a:allocations) if(a.second==static_cast<ULONGLONG>(pa.QuadPart))
            hfiMemory=static_cast<ULONG*>(a.first);
        assert(hfiMemory);
    }
    return 0;
}
NTSTATUS EpiSmmuUnmap(EPI_SMMU*,ULONGLONG,SIZE_T) {return 0;}
NTSTATUS EpiSmmuEnable(EPI_SMMU* c,volatile ULONG*,volatile ULONG*) {
    if(smmuEnableFails) return STATUS_IO_DEVICE_ERROR; c->enabled=true; return 0;
}
NTSTATUS EpiSmmuDisable(EPI_SMMU* c) {
    if(smmuDisableFails) return STATUS_IO_TIMEOUT; c->enabled=false; return 0;
}
NTSTATUS EpiSmmuDestroy(EPI_SMMU* c) {assert(!c->enabled); ExFreePoolWithTag(c,0); return 0;}
NTSTATUS EpiSmmuQueryFault(EPI_SMMU*,ULONG* fsr,ULONGLONG* addr,ULONG* syn) {*fsr=2;*addr=0x4000;*syn=0;return 0;}
NTSTATUS EpiSmmuQueryLastError(EPI_SMMU*,ULONG*,ULONG*,ULONG*,ULONG*) {return STATUS_NOT_FOUND;}
NTSTATUS EpiSmmuQueryCbar(EPI_SMMU* c,ULONG* value) {
    if(!c || !value) return STATUS_INVALID_PARAMETER;
    if(!c->enabled) return STATUS_INVALID_DEVICE_STATE;
    *value=0x1F300; return STATUS_SUCCESS;
}
NTSTATUS EpiSmmuQueryDisableWrites(EPI_SMMU*,EPI_SMMU_DISABLE_WRITE*,ULONG,ULONG* count) {
    if(!count) return STATUS_INVALID_PARAMETER;
    *count=0; return STATUS_NOT_FOUND;
}
static void Block(std::vector<UCHAR>& fw,ULONG a,ULONG s,ULONG t,ULONG v,const void* data=nullptr) {
    ULONG h[]={a,s,t,v}; SIZE_T o=fw.size(); fw.resize(o+16+s);
    std::memcpy(fw.data()+o,h,16); if(s && data) std::memcpy(fw.data()+o+16,data,s);
}
static std::vector<UCHAR> Image() {
    std::vector<UCHAR> f;
    Block(f,0,0,1,0x1DC); Block(f,0x4000,0,1,0xD4CC);
    Block(f,0x100000,0,1,0x85B4); Block(f,0x200000,0,1,0x12A80); Block(f,0x300000,0,1,0x7060);
    Block(f,0x60000000,0,7,0x1000); Block(f,0x60001000,0,7,0x1000);
    ULONG vec[]={0x10007F80,0x4001}; Block(f,0,sizeof(vec),0,0,vec);
    std::vector<UCHAR> code(0xD4CC,0xA5); Block(f,0x4000,static_cast<ULONG>(code.size()),0,0,code.data());
    ULONG version=0x30010008; Block(f,0x10007FE0,4,0,0,&version); return f;
}
static void Reset() {
    assert(allocations.empty() && !g_KeptGmu);
    std::memset(gPages,0,sizeof(gPages)); std::memset(cc,0,sizeof(cc));
    cc[0x106C/4]=0x80000000; cc[0x100C/4]=1; gPages[1][0]=1; gPages[1][0x340/4]=0xC0;
    bootCompletes=true; resetFails=false; smmuEnableFails=false; smmuDisableFails=false;
    failAfter=-1; diagnostics.clear(); gmuWrites=0; hfiMemory=nullptr;
}
static ULONG Run(const std::vector<UCHAR>& f,BOOLEAN* keep) {
    WCHAR text[3072]={};
    return EpiGmuBootTest(reinterpret_cast<HANDLE>(1),reinterpret_cast<HANDLE>(2),cc,f.data(),static_cast<ULONG>(f.size()),text,3072,keep);
}
// Host reset simulation only. Production deliberately retains these allocations on teardown failure.
static void ClearQuarantine() {
    GMU_TEST* s=g_KeptGmu; assert(s);
    if(s->Smmu) {s->Smmu->enabled=false; EpiSmmuDestroy(s->Smmu);}
    for(ULONG i=0;i<s->Count;++i) if(s->Regions[i].Va) GmuFree(s->Regions[i].Va,s->Regions[i].Size,0);
    ExFreePoolWithTag(s,EPI_GMU_TAG); g_KeptGmu=nullptr;
}
int main(int argc,char** argv) {
    std::vector<UCHAR> f=Image(); BOOLEAN keep=false;
    Reset(); GMU_TEST plan={}; assert(GmuPlan(&plan,f.data(),static_cast<ULONG>(f.size())));
    assert(plan.Count==8 && GmuFind(&plan,0x300000,0x7060));
    assert(Run(f,&keep)==0 && !keep && allocations.empty() && gPages[1][0]==1);
    assert(diagnostics[L"GpuGmuFaultStatus"]==0);
    // Complete real vendor block-list validation without embedding or distributing firmware.
    if(argc>1) {
        std::ifstream file(argv[1],std::ios::binary); assert(file);
        std::vector<UCHAR> real((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        Reset(); assert(Run(real,&keep)==0 && !keep && allocations.empty());
    }
    Reset(); auto bad=f; bad.pop_back(); assert(Run(bad,&keep)==EPI_GMU_E_FW && gmuWrites==0 && allocations.empty());
    // Fail each region/context allocation; all already acquired buffers must be released.
    for(int i=0;i<10;++i) {
        Reset(); failAfter=i; ULONG st=Run(f,&keep); failAfter=-1;
        assert((st==EPI_GMU_E_NOMEM || st==EPI_GMU_E_SMMU_MAP) && !keep && allocations.empty());
    }
    Reset(); bootCompletes=false; assert(Run(f,&keep)==EPI_GMU_E_TIMEOUT && !keep && gPages[1][0]==1 && allocations.empty());
    Reset(); smmuEnableFails=true; assert(Run(f,&keep)==EPI_GMU_E_SMMU_ENABLE && !keep && allocations.empty());
    Reset(); smmuDisableFails=true; assert(Run(f,&keep)==EPI_GMU_E_TEARDOWN && keep && !allocations.empty());
    ClearQuarantine();
    Reset(); resetFails=true; assert(Run(f,&keep)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu); ClearQuarantine();
    Reset();
    std::puts("GMU tests passed: firmware regions, synthetic/real image load, allocation failures, boot timeout, reset and SMMU teardown quarantine.");
}
