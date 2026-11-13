// SPDX-License-Identifier: MIT
#define EPI_GMU_HOST_TEST
#include "../../src/epinephrine/epi_gmu.cxx"
#include <fstream>
#include <iterator>
struct EPI_SMMU { bool enabled, gpuStreams; };
struct TEST_MAP { ULONGLONG Iova; SIZE_T Size; ULONG Flags; };
static std::vector<TEST_MAP> testMaps;
static ULONGLONG failMapIova=~0ULL;
NTSTATUS EpiSmmuConfigureGpuStreams(EPI_SMMU* c) { assert(!c->enabled); c->gpuStreams=true; return 0; }
NTSTATUS EpiSmmuQueryGpuStreams(EPI_SMMU* c,EPI_SMMU_GPU_STREAMS_TRACE* t) {
    if(!c->gpuStreams) return STATUS_NOT_FOUND;
    *t={}; t->Configured=1; return 0;
}
NTSTATUS EpiSmmuCreate(HANDLE,EPI_SMMU** c) {
    *c=static_cast<EPI_SMMU*>(ExAllocatePool2(0,sizeof(EPI_SMMU),0)); return *c ? 0 : STATUS_INSUFFICIENT_RESOURCES;
}
NTSTATUS EpiSmmuMap(EPI_SMMU*,ULONGLONG iova,PHYSICAL_ADDRESS pa,SIZE_T size,ULONG flags) {
    if(iova==failMapIova) return STATUS_INSUFFICIENT_RESOURCES;
    testMaps.push_back({iova,size,flags});
    if(iova==0x70010000 || iova==0x70020000) {
        for(const auto& a:allocations) if(a.second==static_cast<ULONGLONG>(pa.QuadPart)) {
            if(iova==0x70010000) g2Ring=static_cast<ULONG*>(a.first);
            else g2Shadow=static_cast<ULONG*>(a.first);
        }
    }
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
static std::vector<UCHAR> SqeImage() {
    std::vector<UCHAR> sqe(EPI_GMU_SQE_FILE_SIZE,0xA5);
    const ULONG head[]={0,0x016DC094}, tail=0x12345678;
    std::memcpy(sqe.data(),head,sizeof(head)); std::memcpy(sqe.data()+sqe.size()-4,&tail,4);
    return sqe;
}
static void Reset() {
    allowG2Writes=false; g2Completes=true; g2FenceForever=false; g2Hang=false; g2RbbmBusy=false; g2GmuBusy=false;
    g2FenceDrops=0; g2Ring=nullptr; g2Shadow=nullptr; g2Writes.clear(); g2KeepGxOn=false;
    testMaps.clear(); failMapIova=~0ULL; g1Writes.clear(); allowG1Writes=false; failG1Dword=0xFFFFFFFF;
    stageFWrites.clear(); allowStageFWrite=false; ignoreStageFWrite=false; dropGxAtStep=0xFFFFFFFF;
    stageEWrites.clear(); failStageEDword=0xFFFFFFFF;
    cfgWrites.clear(); failCfgDword=0xFFFFFFFF;
    std::memset(hwcgIsdbPage,0,sizeof(hwcgIsdbPage)); std::memset(hwcgSpPage,0,sizeof(hwcgSpPage));
    hwcgWrites.clear(); failHwcgDword=0xFFFFFFFF; failHwcgSpSet=false;
    std::memset(secvidPage,0,sizeof(secvidPage)); secvidWrites.clear(); failSecvidOffset=0xFFFFFFFF;
    std::memset(addrPages,0,sizeof(addrPages)); addrWrites.clear();
    failAddrWrite=0xFFFFFFFF; failAddrMap=0xFFFFFFFF;
    assert(allocations.empty() && !g_KeptGmu);
    std::memset(gPages,0,sizeof(gPages)); std::memset(cc,0,sizeof(cc));
    cc[0x106C/4]=0x80000000; cc[0x100C/4]=1; gPages[1][0]=1; gPages[1][0x340/4]=0xC0;
    bootCompletes=true; resetFails=false; smmuEnableFails=false; smmuDisableFails=false;
    failAfter=-1; diagnostics.clear(); gmuWrites=0; hfiMemory=nullptr; failGmuStep=0xFFFFFFFF;
    failCxHaltClear=false; failRbbmHaltClear=false; gxPageProtection=0;
    GmuSeedGxRegs();
}
static ULONG Run(const std::vector<UCHAR>& f,BOOLEAN* keep) {
    WCHAR text[3072]={};
    return EpiGmuBootTest(reinterpret_cast<HANDLE>(1),reinterpret_cast<HANDLE>(2),cc,f.data(),static_cast<ULONG>(f.size()),text,3072,keep);
}
static ULONG RunG1(const std::vector<UCHAR>& f,const std::vector<UCHAR>& sqe,BOOLEAN* keep) {
    WCHAR text[3072]={};
    return EpiGmuBootTest(reinterpret_cast<HANDLE>(1),reinterpret_cast<HANDLE>(2),cc,f.data(),static_cast<ULONG>(f.size()),
                          text,3072,keep,true,true,true,true,true,true,true,true,true,true,true,true,true,
                          sqe.data(),static_cast<ULONG>(sqe.size()));
}
static ULONG RunG2(const std::vector<UCHAR>& f,const std::vector<UCHAR>& sqe,BOOLEAN* keep) {
    WCHAR text[3072]={}; allowStageFWrite=true; allowG1Writes=true; allowG2Writes=true;
    return EpiGmuBootTest(reinterpret_cast<HANDLE>(1),reinterpret_cast<HANDLE>(2),cc,f.data(),static_cast<ULONG>(f.size()),
                          text,3072,keep,true,true,true,true,true,true,true,true,true,true,true,true,true,
                          sqe.data(),static_cast<ULONG>(sqe.size()),true);
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
    // G-1 planning/copy uses the real region machinery without programming CP or starting CM3.
    {
        Reset(); auto sqe=SqeImage(); GMU_TEST mem={};
        assert(GmuPlan(&mem,f.data(),static_cast<ULONG>(f.size())) && mem.Count==8);
        assert(GmuG1Plan(&mem,sqe.data(),static_cast<ULONG>(sqe.size())-1)==STATUS_INVALID_PARAMETER && mem.Count==8);
        auto invalid=sqe; invalid[4]=0;
        assert(GmuG1Plan(&mem,invalid.data(),static_cast<ULONG>(invalid.size()))==STATUS_INVALID_PARAMETER && mem.Count==8);
        GMU_TEST overlap={}; assert(GmuAdd(&overlap,0x70000000,0x10000,EPI_SMMU_READ));
        assert(GmuG1Plan(&overlap,sqe.data(),static_cast<ULONG>(sqe.size()))==STATUS_CONFLICTING_ADDRESSES && overlap.Count==1);
        assert(GmuG1Plan(&mem,sqe.data(),static_cast<ULONG>(sqe.size()))==0 && mem.Count==11);
        for(ULONG i=0;i<3;++i) {
            GMU_REGION* r=GmuFind(&mem,GmuG1Iova[i],GmuG1Size[i]); assert(r && r->Flags==GmuG1Flags[i]);
            r->Va=GmuAlloc(r->Size,{0},{0xFFFFFFFFFLL},{0},0); assert(r->Va);
            std::memset(r->Va,0,r->Size);
        }
        GMU_G1_TRACE trace=GmuG1Trace();
        assert(GmuG1Copy(&mem,reinterpret_cast<HANDLE>(1),sqe.data(),&trace)==0);
        assert(trace.SqeBytes==40492 && trace.SqeFirst==0x016DC094 && trace.SqeLast==0x12345678);
        for(ULONG i=0;i<3;++i) {
            GMU_REGION* r=GmuFind(&mem,GmuG1Iova[i],GmuG1Size[i]);
            if(i==0) assert(std::memcmp(r->Va,sqe.data()+4,40492)==0);
            const ULONG start=i==0 ? 40492 : 0;
            for(ULONG j=start;j<r->Size;++j) assert(static_cast<UCHAR*>(r->Va)[j]==0);
            GmuFree(r->Va,r->Size,0); r->Va=nullptr;
        }
        assert(allocations.empty());
    }
    // Read-only GX probe: independent fixture order, complete count, and stopping before an unpersisted read.
    Reset();
    {
        GMU_TEST probe={}; probe.Maps[1]=gPages[1]; probe.Maps[2]=gPages[2]; probe.Maps[6]=gPages[6];
        GMU_GX_TRACE oob={}; oob.OobStatus=STATUS_SUCCESS;
        cc[0x100C/4]=0x80000000; gPages[1][0xD0]=0;
        GMU_GX_REG_TRACE regs={0,GX_REG_NOT_REACHED,0xFFFFFFFF,0xFFFFFFFF,0,
                              {0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF}};
        GmuGxReadReg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&regs);
        assert(regs.Attempted==1 && regs.Reason==GX_REG_READ && regs.Count==6 && gmuWrites==0);
        assert(gxPageProtection==(PAGE_READONLY|PAGE_NOCACHE));
        for(ULONG i=0;i<6;++i) assert(regs.Value[i]==gxRegExpected[i]);
        regs={0,GX_REG_NOT_REACHED,0xFFFFFFFF,0xFFFFFFFF,0,
              {0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF}};
        failGmuStep=0x7CA;
        GmuGxReadReg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&regs);
        assert(regs.Attempted==1 && regs.Reason==GX_REG_BREADCRUMB_FAILED && regs.Count==2 && gmuWrites==0);
        assert(regs.Value[0]==gxRegExpected[0] && regs.Value[1]==gxRegExpected[1]);
        for(ULONG i=2;i<6;++i) assert(regs.Value[i]==0xFFFFFFFF);
        // New opt-in path: both original halt pairs are recorded, and only the two HALT registers are cleared.
        failGmuStep=0xFFFFFFFF;
        regs={};
        GMU_GX_HALT_TRACE halt={}; halt.Status=STATUS_NOT_FOUND;
        gPages[6][0x45]=3; gPages[6][0x46]=3; gPages[8][0x16]=1; gPages[8][0x17]=1;
        GmuGxReadReg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&regs,&halt);
        assert(halt.Status==0 && halt.Reason==0 && halt.Count==2 && gmuWrites==2);
        assert(halt.Before[0]==3 && halt.Before[1]==3 && halt.Before[2]==1 && halt.Before[3]==1);
        for(ULONG i=0;i<4;++i) assert(halt.After[i]==0);
        assert(gxPageProtection==(PAGE_READWRITE|PAGE_NOCACHE));
        assert(GmuGxCxHaltIdle(&probe,reinterpret_cast<HANDLE>(1),&halt)==0);
        // v50: ACK can stay at 3 while HALT is clear; record it without blocking teardown.
        gPages[6][0x46]=3;
        assert(GmuGxCxHaltIdle(&probe,reinterpret_cast<HANDLE>(1),&halt)==0);
        assert(halt.FinalCx[0]==0 && halt.FinalCx[1]==3 && halt.CxPolls==0);
        // Stage A: all originals precede ordered writes; stage B slots stay unread.
        for(ULONG i=0;i<4;++i) secvidPage[i]=0x10+i;
        GMU_GX_INIT_TRACE init=GmuGxInitTrace();
        assert(GmuGxSecvid(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==0);
        assert(init.Reason==0 && init.Attempted==1 && init.Count==4);
        const ULONG order[]={3,0,1,2};
        assert(secvidWrites.size()==4);
        for(ULONG i=0;i<4;++i) {
            assert(secvidWrites[i]==order[i] && init.Before[i]==0x10+order[i] && init.After[i]==0);
        }
        for(ULONG i=4;i<16;++i) assert(init.Before[i]==0xFFFFFFFF && init.After[i]==0xFFFFFFFF);
        assert(addrWrites.empty()); // stage A alone never writes an ADDR_MODE register
        // Failure at the second write breadcrumb leaves the remaining registers untouched.
        secvidWrites.clear(); for(ULONG i=0;i<4;++i) secvidPage[i]=0x10+i;
        init=GmuGxInitTrace(); failGmuStep=0x811;
        assert(GmuGxSecvid(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
        assert(init.Reason==6 && init.Count==1 && secvidWrites.size()==1 && secvidPage[0]==0x10);
        // A failed original-read breadcrumb prevents every write.
        secvidWrites.clear(); init=GmuGxInitTrace(); failGmuStep=0x802;
        assert(GmuGxSecvid(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
        assert(init.Reason==6 && init.Count==0 && init.Attempted==0 && secvidWrites.empty());
        // Readback mismatch at BASE low stops before BASE high and SIZE.
        failGmuStep=0xFFFFFFFF; failSecvidOffset=0;
        init=GmuGxInitTrace();
        assert(GmuGxSecvid(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
        assert(init.Reason==7 && init.Count==2 && secvidWrites.size()==2);
        assert(init.After[1]==0x10 && init.After[2]==0xFFFFFFFF && secvidPage[1]==0x11);
        failSecvidOffset=0xFFFFFFFF;
        // Stage B follows A, saves all originals, and writes exactly the documented order.
        init=GmuGxInitTrace();
        assert(GmuGxSecvid(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==0);
        for(ULONG i=0;i<12;++i) *GmuAddrReg(i)=0x100+i;
        assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==0);
        assert(init.Reason==0 && init.Count==16 && addrWrites.size()==12);
        for(ULONG i=0;i<12;++i)
            assert(addrWrites[i]==addrDwords[i] && init.Before[4+i]==0x100+i && init.After[4+i]==1);
        // No stage B accesses after a failed/incomplete stage A.
        addrWrites.clear(); init=GmuGxInitTrace();
        assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_INVALID_DEVICE_STATE);
        assert(init.Reason==8 && addrWrites.empty());
        // Fail each stage B write breadcrumb: only earlier writes may complete.
        for(ULONG fail=0;fail<12;++fail) {
            init=GmuGxInitTrace(); init.Reason=0; init.Count=4; init.Attempted=1;
            addrWrites.clear(); for(ULONG i=0;i<12;++i) *GmuAddrReg(i)=0x100+i;
            failGmuStep=0x830+fail;
            assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
            assert(init.Reason==6 && init.Count==4+fail && addrWrites.size()==fail);
            for(ULONG i=0;i<12;++i) assert(init.Before[4+i]==0x100+i);
            for(ULONG i=fail;i<12;++i) assert(init.After[4+i]==0xFFFFFFFF && *GmuAddrReg(i)==0x100+i);
        }
        // A failed original read or map must prevent every stage B write.
        init=GmuGxInitTrace(); init.Reason=0; init.Count=4;
        addrWrites.clear(); failGmuStep=0x825;
        assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
        assert(init.Reason==6 && init.Count==4 && addrWrites.empty());
        init=GmuGxInitTrace(); init.Reason=0; init.Count=4;
        failGmuStep=0xFFFFFFFF; failAddrMap=3;
        assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_INSUFFICIENT_RESOURCES);
        assert(init.Reason==5 && init.Count==4 && addrWrites.empty()); failAddrMap=0xFFFFFFFF;
        // A readback mismatch includes that write in Count and stops the remainder.
        for(ULONG fail=0;fail<12;++fail) {
            init=GmuGxInitTrace(); init.Reason=0; init.Count=4; init.Attempted=1;
            addrWrites.clear(); for(ULONG i=0;i<12;++i) *GmuAddrReg(i)=0x100+i;
            failAddrWrite=fail;
            assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
            assert(init.Reason==7 && init.Count==5+fail && addrWrites.size()==1+fail);
            assert(init.After[4+fail]==0x100+fail);
            for(ULONG i=fail+1;i<12;++i) assert(init.After[4+i]==0xFFFFFFFF && *GmuAddrReg(i)==0x100+i);
        }
        failAddrWrite=0xFFFFFFFF;
        // HWCG: seed distinct originals and verify ordered writes, skip, recovery and AO restoration.
        const auto seedHwcg=[&]() {
            failGmuStep=0xFFFFFFFF; failHwcgDword=0xFFFFFFFF; failHwcgSpSet=false;
            hwcgWrites.clear(); gPages[8][0xAE]=0; hwcgSpPage[0x80]=0xA5;
            for(ULONG i=0;i<3;++i) gPages[2][0x309+i]=0x10+i;
            for(ULONG i=0;i<49;++i) {
                const ULONG off=GmuHwcgTable[i][0];
                (off>=0x400 ? hwcgIsdbPage : gPages[8])[off&0x3FF]=0xA000+i;
            }
        };
        seedHwcg(); GMU_GX_HWCG_TRACE hw=GmuGxHwcgTrace();
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
        assert(hw.Attempted==1 && hw.Reason==0 && hw.Count==53 && hw.Skipped==0);
        assert(hwcgWrites.size()==55 && hw.SptpracBefore==0xA5 && hw.SptpracAfterClear==0xA4 && hw.SptpracAfterSet==0xA5);
        for(ULONG i=0;i<3;++i) assert(hwcgWrites[i].first==0x23B09+i && hwcgWrites[i].second==GmuHwcgCgc[i]);
        assert(hwcgWrites[3].first==0x1A880 && hwcgWrites[3].second==0xA4);
        for(ULONG i=0;i<49;++i) {
            assert(hw.Before[i]==0xA000+i && hw.After[i]==GmuHwcgTable[i][1]);
            assert(hwcgWrites[4+i].first==GmuHwcgTable[i][0] && hwcgWrites[4+i].second==GmuHwcgTable[i][1]);
        }
        assert(hwcgWrites[53].first==0x1A880 && hwcgWrites[53].second==0xA5);
        assert(hwcgWrites[54].first==0xAE && hwcgWrites[54].second==0x8AA8AA82);
        assert(GmuGxHwcgRestore(&probe,reinterpret_cast<HANDLE>(1),&hw)==0);
        for(ULONG i=0;i<3;++i) assert(hw.CgcRestored[i]==0x10+i && gPages[2][0x309+i]==0x10+i);
        seedHwcg(); hw=GmuGxHwcgTrace(); gPages[8][0xAE]=0x8AA8AA82;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
        assert(hw.Skipped==1 && hw.Count==0 && hw.Attempted==0 && hwcgWrites.empty());
        // Every table breadcrumb failure must still re-enable SP, and must skip CLOCK_CNTL.
        for(ULONG fail=0;fail<49;++fail) {
            seedHwcg(); hw=GmuGxHwcgTrace(); failGmuStep=0x890+fail;
            assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
            assert(hw.Reason==6 && hw.Count==3+fail && hw.SptpracAfterSet==0xA5 && gPages[8][0xAE]==0);
            for(ULONG i=fail;i<49;++i) assert(hw.After[i]==0xFFFFFFFF);
        }
        // v55: an ignored write at any table position is recorded, but all later writes complete.
        for(ULONG fail=0;fail<49;++fail) {
            seedHwcg(); hw=GmuGxHwcgTrace(); failHwcgDword=GmuHwcgTable[fail][0];
            assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
            assert(hw.Reason==0 && hw.Count==53 && hw.After[fail]==0xA000+fail && hw.SptpracAfterSet==0xA5);
            for(ULONG i=0;i<49;++i) assert(hw.After[i]==(i==fail ? 0xA000+i : GmuHwcgTable[i][1]));
            assert(gPages[8][0xAE]==0x8AA8AA82 && hwcgWrites.size()==55);
        }
        // Even a failed re-enable breadcrumb must not suppress the actual recovery write.
        seedHwcg(); hw=GmuGxHwcgTrace(); failGmuStep=0x8C1;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Reason==6 && hw.SptpracAfterSet==0xA5 && gPages[8][0xAE]==0);
        seedHwcg(); hw=GmuGxHwcgTrace(); failHwcgSpSet=true;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
        assert(hw.Reason==0 && hw.Count==53 && hw.SptpracAfterSet==0xA4 && gPages[8][0xAE]==0x8AA8AA82);
        // Both SP writes ignored: diagnostic values differ, but the table and CLOCK_CNTL still complete.
        seedHwcg(); hw=GmuGxHwcgTrace(); failHwcgDword=0x1A880;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
        assert(hw.Reason==0 && hw.Count==53 && hw.SptpracAfterClear==0xA5 && hw.SptpracAfterSet==0xA5);
        // A later SP readback mismatch cannot replace an earlier breadcrumb failure.
        seedHwcg(); hw=GmuGxHwcgTrace(); failGmuStep=0x895; failHwcgSpSet=true;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Reason==6 && hw.Count==8 && hw.SptpracAfterSet==0xA4 && gPages[8][0xAE]==0);
        // CGC and final CLOCK_CNTL comparisons remain strict.
        seedHwcg(); hw=GmuGxHwcgTrace(); failHwcgDword=0x23B09;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Reason==7 && hw.Count==1 && hw.SptpracAfterClear==0xFFFFFFFF);
        seedHwcg(); hw=GmuGxHwcgTrace(); failHwcgDword=0xAE;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Reason==7 && hw.Count==53 && hw.ClockCntlAfter==0);
        seedHwcg(); hw=GmuGxHwcgTrace(); failGmuStep=0x845;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Count==0 && hw.Attempted==0 && hwcgWrites.empty());
        seedHwcg(); hw=GmuGxHwcgTrace();
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
        failHwcgDword=0x23B09;
        assert(GmuGxHwcgRestore(&probe,reinterpret_cast<HANDLE>(1),&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.CgcRestored[0]==GmuHwcgCgc[0] && hw.CgcRestored[1]==0x11 && hw.CgcRestored[2]==0x12);
        failHwcgDword=0xFFFFFFFF;
        // Stage D: complete originals, lo/high ordering, QOS twice, SELECT RMW, no readback comparisons.
        const auto seedCfg=[&]() {
            failGmuStep=0xFFFFFFFF; failCfgDword=0xFFFFFFFF; failAddrMap=0xFFFFFFFF;
            cfgWrites.clear();
            for(ULONG i=0;i<30;++i) *GmuCfgReg(cfgDwords[i])=0x100+i;
            *GmuCfgReg(0x1F841)=0xABCDEFAA; *GmuCfgReg(0x1F840)=0;
        };
        seedCfg(); GMU_GX_CFG_TRACE cfg=GmuGxCfgTrace();
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==0);
        assert(cfg.Reason==0 && cfg.Count==30 && cfg.Attempted==1 && cfg.CxWritten==7 && cfgWrites.size()==30);
        assert(cfg.Writes[0].Before==0x11C && cfg.Writes[28].Before==0x11C);
        assert(cfg.Writes[0].After==3 && cfg.Writes[28].After==0);
        assert(cfg.Writes[25].Value==0xABCDEF20 && cfg.Writes[25].After==0xABCDEF20);
        for(ULONG i=0;i<30;++i) {
            assert(cfg.Writes[i].Reg==cfgDwords[i] && cfgWrites[i].first==cfgDwords[i]);
            assert(cfgWrites[i].second==cfg.Writes[i].Value && cfg.Writes[i].After==cfg.Writes[i].Value);
        }
        // Slumber clears ENABLE; teardown must not duplicate that write or restore stage D's temporary 1.
        *GmuCfgReg(0x1F840)=0; cfgWrites.clear();
        assert(GmuGxCfgRestore(&probe,reinterpret_cast<HANDLE>(1),&cfg,&oob)==0);
        assert(cfgWrites.size()==2 && cfg.CxRestored[2]==0);
        for(ULONG i=0;i<3;++i) assert(cfg.CxRestored[i]==cfg.CxBefore[i]);
        seedCfg(); cfg=GmuGxCfgTrace(); *GmuCfgReg(0x1F840)=5;
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==0);
        *GmuCfgReg(0x1F840)=0;
        assert(GmuGxCfgRestore(&probe,reinterpret_cast<HANDLE>(1),&cfg,&oob)==0);
        assert(cfg.CxBefore[2]==5 && cfg.CxRestored[2]==5 && *GmuCfgReg(0x1F840)==5);
        for(ULONG fail=0;fail<30;++fail) {
            seedCfg(); cfg=GmuGxCfgTrace(); failCfgDword=cfgDwords[fail];
            assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==0);
            assert(cfg.Reason==0 && cfg.Count==30 && cfgWrites.size()==30);
        }
        // Every write breadcrumb failure stops later writes, leaving unread After fields untouched.
        for(ULONG fail=0;fail<30;++fail) {
            seedCfg(); cfg=GmuGxCfgTrace(); failGmuStep=0x940+fail;
            assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==STATUS_IO_DEVICE_ERROR);
            assert(cfg.Reason==6 && cfg.Count==fail && cfgWrites.size()==fail);
            for(ULONG i=fail;i<30;++i) assert(cfg.Writes[i].After==0xFFFFFFFF);
            assert(cfg.CxWritten==static_cast<ULONG>((fail>24 ? 1 : 0)|(fail>25 ? 2 : 0)|(fail>26 ? 4 : 0)));
        }
        seedCfg(); cfg=GmuGxCfgTrace(); failGmuStep=0x919;
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==STATUS_IO_DEVICE_ERROR);
        assert(cfg.Count==0 && cfg.CxWritten==0 && cfgWrites.empty());
        seedCfg(); cfg=GmuGxCfgTrace(); failAddrMap=0;
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==STATUS_INSUFFICIENT_RESOURCES);
        assert(cfg.Reason==5 && cfgWrites.empty());
        seedCfg(); cfg=GmuGxCfgTrace(); gPages[1][0xD0]=0xC0;
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==STATUS_INVALID_DEVICE_STATE);
        assert(cfg.Reason==GX_REG_HM_OFF && cfg.Count==0 && cfgWrites.empty()); gPages[1][0xD0]=0;
        seedCfg(); cfg=GmuGxCfgTrace();
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==0);
        failCfgDword=0x23B0E;
        assert(GmuGxCfgRestore(&probe,reinterpret_cast<HANDLE>(1),&cfg,&oob)==STATUS_IO_DEVICE_ERROR);
        assert(cfg.CxRestored[0]!=cfg.CxBefore[0] && cfg.CxRestored[1]==cfg.CxBefore[1] && cfg.CxRestored[2]==0);
        failCfgDword=0xFFFFFFFF;
        // Stage E: UBWC then control and protect 0..39,47, with every original saved before writing.
        const auto seedStageE=[&]() {
            failGmuStep=0xFFFFFFFF; failStageEDword=0xFFFFFFFF; failAddrMap=0xFFFFFFFF;
            stageEWrites.clear();
            for(ULONG i=0;i<47;++i) *GmuCfgReg(GmuStageEReg(i))=0x200+i;
            for(ULONG reg=0x878;reg<=0x87E;++reg) *GmuCfgReg(reg)=0xCAFE;
        };
        seedStageE(); GMU_GX_STAGE_E_TRACE stageE=GmuGxStageETrace();
        assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==0);
        assert(stageE.Reason==0 && stageE.Count==47 && stageE.Attempted==1 && stageEWrites.size()==47);
        for(ULONG i=0;i<47;++i) {
            assert(stageE.Writes[i].Reg==GmuStageEReg(i) && stageE.Writes[i].Before==0x200+i);
            assert(stageEWrites[i].first==GmuStageEReg(i) && stageEWrites[i].second==stageE.Writes[i].Value);
            assert(stageE.Writes[i].After==stageE.Writes[i].Value);
        }
        assert(stageE.Writes[46].Reg==0x87F && stageE.Writes[46].Value==0x8001F8C0);
        for(ULONG reg=0x878;reg<=0x87E;++reg) assert(*GmuCfgReg(reg)==0xCAFE);
        // Any single ignored write must be recorded without suppressing later writes.
        for(ULONG fail=0;fail<47;++fail) {
            seedStageE(); stageE=GmuGxStageETrace(); failStageEDword=GmuStageEReg(fail);
            assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==0);
            assert(stageE.Reason==0 && stageE.Count==47 && stageEWrites.size()==47);
            assert(stageE.Writes[fail].After==0x200+fail);
        }
        for(ULONG fail=0;fail<47;++fail) {
            seedStageE(); stageE=GmuGxStageETrace(); failGmuStep=0xA40+fail;
            assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==STATUS_IO_DEVICE_ERROR);
            assert(stageE.Reason==6 && stageE.Count==fail && stageEWrites.size()==fail);
            for(ULONG i=0;i<47;++i) assert(stageE.Writes[i].Before==0x200+i);
            for(ULONG i=fail;i<47;++i) assert(stageE.Writes[i].After==0xFFFFFFFF);
        }
        seedStageE(); stageE=GmuGxStageETrace(); failGmuStep=0xA10;
        assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==STATUS_IO_DEVICE_ERROR);
        assert(stageE.Count==0 && stageE.Attempted==0 && stageEWrites.empty());
        seedStageE(); stageE=GmuGxStageETrace(); failAddrMap=0;
        assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==STATUS_INSUFFICIENT_RESOURCES);
        assert(stageE.Reason==5 && stageEWrites.empty());
        seedStageE(); stageE=GmuGxStageETrace(); gPages[1][0xD0]=0xC0;
        assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==STATUS_INVALID_DEVICE_STATE);
        assert(stageE.Reason==GX_REG_HM_OFF && stageEWrites.empty()); gPages[1][0xD0]=0;
        // Stage F: one opt-in write, then four ordered diagnostic reads without comparisons.
        const ULONG checkRegs[]={0x011,0x500,0xE17,0x84F};
        const ULONG checkValues[]={0,1,4,0xB};
        const auto seedStageF=[&]() {
            failGmuStep=0xFFFFFFFF; failAddrMap=0xFFFFFFFF; dropGxAtStep=0xFFFFFFFF;
            gPages[1][0xD0]=0; stageFWrites.clear(); allowStageFWrite=true; ignoreStageFWrite=false;
            *GmuCfgReg(0x98D)=7;
            for(ULONG i=0;i<4;++i) *GmuCfgReg(checkRegs[i])=checkValues[i];
        };
        seedStageF(); GMU_GX_STAGE_F_TRACE stageF=GmuGxStageFTrace();
        assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==0);
        assert(stageF.Attempted==1 && stageF.Reason==0 && stageF.Before==7 && stageF.After==1 && stageF.Count==4);
        assert(stageFWrites.size()==1 && stageFWrites[0]==1);
        for(ULONG i=0;i<4;++i) assert(stageF.Check[i]==checkValues[i]);
        seedStageF(); stageF=GmuGxStageFTrace(); ignoreStageFWrite=true;
        for(ULONG i=0;i<4;++i) *GmuCfgReg(checkRegs[i])=0x100+i;
        assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==0);
        assert(stageF.Reason==0 && stageF.After==7 && stageF.Count==4);
        for(ULONG i=0;i<4;++i) assert(stageF.Check[i]==0x100+i);
        // Original/write breadcrumb failures must prevent CP_AHB_CNTL publication.
        for(ULONG step=0xB00;step<=0xB02;++step) {
            seedStageF(); stageF=GmuGxStageFTrace(); failGmuStep=step;
            assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==STATUS_IO_DEVICE_ERROR);
            assert(stageF.Reason==6 && stageF.Attempted==0 && stageF.Count==0 && stageFWrites.empty());
            assert(stageF.After==0xFFFFFFFF);
        }
        for(ULONG fail=0;fail<4;++fail) {
            seedStageF(); stageF=GmuGxStageFTrace(); failGmuStep=0xB10+fail;
            assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==STATUS_IO_DEVICE_ERROR);
            assert(stageF.Reason==6 && stageF.Attempted==1 && stageF.Count==fail && stageFWrites.size()==1);
            for(ULONG i=0;i<fail;++i) assert(stageF.Check[i]==checkValues[i]);
            for(ULONG i=fail;i<4;++i) assert(stageF.Check[i]==0xFFFFFFFF);
        }
        seedStageF(); stageF=GmuGxStageFTrace(); failAddrMap=0;
        assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==STATUS_INSUFFICIENT_RESOURCES);
        assert(stageF.Reason==5 && stageFWrites.empty());
        seedStageF(); stageF=GmuGxStageFTrace(); dropGxAtStep=0xB12;
        assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==STATUS_INVALID_DEVICE_STATE);
        assert(stageF.Reason==GX_REG_HM_OFF && stageF.Count==2 && stageF.Check[2]==0xFFFFFFFF && stageF.Check[3]==0xFFFFFFFF);
        dropGxAtStep=0xFFFFFFFF; gPages[1][0xD0]=0; allowStageFWrite=false;
        // G-1 register setup: seven writes only, lo/hi order, diagnostic-only readbacks.
        allowG1Writes=true; GMU_G1_TRACE g1=GmuG1Trace();
        g1Writes.clear(); failGmuStep=0xFFFFFFFF;
        for(ULONG i=0;i<7;++i) *GmuCfgReg(g1Dwords[i])=0x300+i;
        assert(GmuG1Registers(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g1)==0 && g1.Count==7 && g1Writes.size()==7);
        for(ULONG i=0;i<7;++i) {
            assert(g1.Before[i]==0x300+i && g1.After[i]==g1.Value[i]);
            assert(g1Writes[i].first==g1Dwords[i] && g1Writes[i].second==g1.Value[i]);
        }
        for(ULONG fail=0;fail<7;++fail) {
            g1=GmuG1Trace(); g1Writes.clear(); failG1Dword=g1Dwords[fail];
            for(ULONG i=0;i<7;++i) *GmuCfgReg(g1Dwords[i])=0x300+i;
            assert(GmuG1Registers(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g1)==0 && g1.Count==7);
            assert(g1.After[fail]==0x300+fail);
        }
        failG1Dword=0xFFFFFFFF;
        for(ULONG fail=0;fail<7;++fail) {
            g1=GmuG1Trace(); g1Writes.clear(); failGmuStep=0xC20+fail;
            assert(GmuG1Registers(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g1)==STATUS_IO_DEVICE_ERROR);
            assert(g1.Reason==6 && g1.Count==fail && g1Writes.size()==fail);
            for(ULONG i=fail;i<7;++i) assert(g1.After[i]==0xFFFFFFFF);
        }
        allowG1Writes=false; failGmuStep=0xFFFFFFFF;
        // G-2: independent memory fixture, one ME_INIT packet, bounded fence retries and all idle predicates.
        {
            std::vector<ULONG> cpSqe(0xA000/4), cpRing(0x8000/4), cpShadow(0x1000/4);
            EPI_SMMU cpSmmu={true,true}; probe.Smmu=&cpSmmu;
            for(ULONG i=0;i<3;++i) assert(GmuAdd(&probe,GmuG1Iova[i],GmuG1Size[i],GmuG1Flags[i]));
            probe.Regions[0].Va=cpSqe.data(); probe.Regions[1].Va=cpRing.data(); probe.Regions[2].Va=cpShadow.data();
            const auto seedG2=[&]() {
                allowG2Writes=true; g2Writes.clear(); g2Completes=true; g2FenceForever=false; g2FenceDrops=0;
                g2Hang=false; g2RbbmBusy=false; g2GmuBusy=false; failGmuStep=0xFFFFFFFF; failAddrMap=0xFFFFFFFF;
                dropGxAtStep=0xFFFFFFFF; gPages[1][0xD0]=0; gPages[2][0x313]=0; gPages[2][0x30C]=0;
                cc[0x100C/4]=0x80000000; g2Ring=cpRing.data(); g2Shadow=cpShadow.data();
                std::memset(cpRing.data(),0,0x8000); std::memset(cpShadow.data(),0,0x1000);
                for(ULONG i=0;i<3;++i) probe.Regions[i].Mapped=true;
                *GmuCfgReg(0x808)=0; *GmuCfgReg(0x807)=0; *GmuCfgReg(0x806)=0; *GmuCfgReg(0x821)=0;
                gPages[8][0x210]=1; gPages[8][0x201]=0;
            };
            const ULONG expectedPacket[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
            seedG2(); GMU_G2_TRACE cp=GmuG2Trace();
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==0);
            assert(cp.Reason==0 && cp.Attempted==1 && cp.SqeWritten==1 && cp.WptrWrites==1 && cp.Polls==0);
            assert(cp.ShadowBefore==0 && cp.ShadowAfter==9 && cp.Rptr==9 && cp.WptrAfter==9 && cp.FenceStatus==0);
            assert(cp.RbbmStatus==1 && cp.CxBusy==0 && cp.IntStatus==0 && cp.CpHwFault==0 && cp.FaultStatus==0);
            assert(g2Writes.size()==2 && g2Writes[0].first==0x808 && g2Writes[0].second==1);
            assert(g2Writes[1].first==0x807 && g2Writes[1].second==9);
            assert(std::memcmp(cpRing.data(),expectedPacket,sizeof(expectedPacket))==0);
            assert(std::memcmp(cp.Packet,expectedPacket,sizeof(expectedPacket))==0);
            seedG2(); cp=GmuG2Trace(); g2FenceDrops=3;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==0 && cp.WptrWrites==4 && cp.ShadowAfter==9);
            seedG2(); cp=GmuG2Trace(); g2FenceForever=true;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_TIMEOUT);
            assert(cp.Reason==10 && cp.WptrWrites==201 && cp.FenceStatus==1 && cp.Polls==0xFFFFFFFF);
            for(ULONG predicate=0;predicate<3;++predicate) {
                seedG2(); cp=GmuG2Trace();
                if(predicate==0) g2Completes=false;
                if(predicate==1) g2RbbmBusy=true;
                if(predicate==2) g2GmuBusy=true;
                *GmuCfgReg(0x821)=0x55;
                assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_TIMEOUT);
                assert(cp.Reason==9 && cp.Polls==10000 && cp.CpHwFault==0x55 && cp.FaultStatus==0 && cp.FarLow==0x4000);
            }
            seedG2(); cp=GmuG2Trace(); g2Hang=true;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_DEVICE_ERROR);
            assert(cp.Reason==11 && cp.Polls==0 && (cp.IntStatus&(1UL<<23)));
            const ULONG steps[]={0xD00,0xD01,0xD02,0xD03,0xD04,0xD05,0xD06,0xD07};
            for(ULONG step:steps) {
                seedG2(); cp=GmuG2Trace(); failGmuStep=step;
                assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_DEVICE_ERROR && cp.Reason==6);
                assert(cp.Attempted==(step<=0xD02 ? 0UL : 1UL) && cp.WptrWrites==(step<=0xD04 ? 0UL : 1UL));
            }
            seedG2(); cp=GmuG2Trace(); g2Completes=false; failGmuStep=0xD07;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_TIMEOUT && cp.Reason==9);
            seedG2(); cp=GmuG2Trace(); dropGxAtStep=0xD06;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_INVALID_DEVICE_STATE);
            assert(cp.Reason==GX_REG_HM_OFF && cp.WptrAfter==0xFFFFFFFF && cp.CpHwFault==0xFFFFFFFF);
            seedG2(); cp=GmuG2Trace(); failAddrMap=0;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_INSUFFICIENT_RESOURCES && g2Writes.empty());
            for(ULONG bad=0;bad<4;++bad) {
                seedG2(); cp=GmuG2Trace();
                if(bad==0) cpShadow[0]=1;
                if(bad==1) cpRing[0]=1;
                if(bad==2) *GmuCfgReg(0x807)=1;
                if(bad==3) probe.Regions[2].Mapped=false;
                assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_INVALID_DEVICE_STATE);
                assert(cp.Reason==12 && cp.Attempted==0 && g2Writes.empty());
            }
            allowG2Writes=false; g2Ring=nullptr; g2Shadow=nullptr; probe.Count=0; probe.Smmu=nullptr;
            failGmuStep=0xFFFFFFFF; failAddrMap=0xFFFFFFFF; dropGxAtStep=0xFFFFFFFF; gPages[1][0xD0]=0;
        }
        // A stuck CX clear must stop before the RBBM write and must block final CX teardown.
        regs={}; halt={}; halt.Status=STATUS_NOT_FOUND; gmuWrites=0;
        gPages[6][0x45]=3; gPages[6][0x46]=3; gPages[8][0x16]=1; failCxHaltClear=true;
        GmuGxReadReg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&regs,&halt);
        assert(halt.Status==STATUS_IO_DEVICE_ERROR && halt.Reason==7 && halt.Count==1 && gmuWrites==1);
        assert(gPages[8][0x16]==1);
        assert(GmuGxCxHaltIdle(&probe,reinterpret_cast<HANDLE>(1),&halt)==STATUS_IO_TIMEOUT);
        // A stuck RBBM clear is recorded too, but a cleared CX halt remains independently observable.
        regs={}; halt={}; halt.Status=STATUS_NOT_FOUND; gmuWrites=0;
        failCxHaltClear=false; failRbbmHaltClear=true;
        gPages[6][0x45]=3; gPages[6][0x46]=3;
        GmuGxReadReg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&regs,&halt);
        assert(halt.Status==STATUS_IO_DEVICE_ERROR && halt.Reason==7 && halt.Count==2 && gmuWrites==2);
        assert(halt.After[0]==0 && halt.After[2]==1);
        assert(GmuGxCxHaltIdle(&probe,reinterpret_cast<HANDLE>(1),&halt)==0);
    }
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
    // G-1 failures before CM3 release release every GPU allocation, unless Disable itself fails.
    {
        const auto sqe=SqeImage();
        for(int fail=0;fail<13;++fail) {
            Reset(); failAfter=fail;
            const ULONG status=RunG1(f,sqe,&keep); failAfter=-1;
            assert((status==EPI_GMU_E_NOMEM || status==EPI_GMU_E_SMMU_MAP) && !keep && allocations.empty());
        }
        for(ULONG i=0;i<3;++i) {
            Reset(); failMapIova=GmuG1Iova[i];
            assert(RunG1(f,sqe,&keep)==EPI_GMU_E_SMMU_MAP && !keep && allocations.empty());
        }
        Reset(); smmuEnableFails=true;
        assert(RunG1(f,sqe,&keep)==EPI_GMU_E_SMMU_ENABLE && !keep && allocations.empty());
        assert(testMaps.size()==11 && g1Writes.empty());
        for(ULONG i=0;i<3;++i) {
            assert(testMaps[8+i].Iova==GmuG1Iova[i] && testMaps[8+i].Size==GmuG1Size[i] && testMaps[8+i].Flags==GmuG1Flags[i]);
        }
        Reset(); smmuEnableFails=true; smmuDisableFails=true;
        assert(RunG1(f,sqe,&keep)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu && !allocations.empty());
        assert(g_KeptGmu->Smmu->gpuStreams);
        for(ULONG i=0;i<3;++i) assert(GmuFind(g_KeptGmu,GmuG1Iova[i],GmuG1Size[i])->Va!=nullptr);
        ClearQuarantine(); Reset();
    }
    // Full G-2 path: normal completion frees memory, CP errors preserve cleanup, GX/Disable failures retain data.
    {
        const auto sqe=SqeImage();
        Reset(); assert(RunG2(f,sqe,&keep)==0 && !keep && allocations.empty());
        assert(g2Writes.size()==2 && g2Writes[0].first==0x808 && g2Writes[1].first==0x807);
        Reset(); g2Completes=false;
        assert(RunG2(f,sqe,&keep)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); failG1Dword=0x830;
        assert(RunG2(f,sqe,&keep)==EPI_GMU_E_CP && g2Writes.empty() && !keep && allocations.empty());
        Reset(); g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu && g_KeptGmu->Smmu->enabled);
        for(ULONG i=0;i<3;++i) assert(GmuFind(g_KeptGmu,GmuG1Iova[i],GmuG1Size[i])->Va!=nullptr);
        ClearQuarantine();
        Reset(); g2Completes=false; smmuDisableFails=true;
        assert(RunG2(f,sqe,&keep)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        for(ULONG i=0;i<3;++i) assert(GmuFind(g_KeptGmu,GmuG1Iova[i],GmuG1Size[i])->Va!=nullptr);
        ClearQuarantine(); Reset();
    }
    std::puts("GMU tests passed: firmware regions, synthetic/real image load, allocation failures, boot timeout, reset and SMMU teardown quarantine.");
}
