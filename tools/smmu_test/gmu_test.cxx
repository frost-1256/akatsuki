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
// Host reset simulation only. Production deliberately retains these allocations on teardown failure.
static void ClearQuarantine() {
    GMU_TEST* s=g_KeptGmu; assert(s);
    if(s->Smmu) {s->Smmu->enabled=false; EpiSmmuDestroy(s->Smmu);}
    for(ULONG i=0;i<s->Count;++i) if(s->Regions[i].Va) GmuFree(s->Regions[i].Va,s->Regions[i].Size,0);
    ExFreePoolWithTag(s,EPI_GMU_TAG); g_KeptGmu=nullptr;
}
int main(int argc,char** argv) {
    std::vector<UCHAR> f=Image(); BOOLEAN keep=false;
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
    std::puts("GMU tests passed: firmware regions, synthetic/real image load, allocation failures, boot timeout, reset and SMMU teardown quarantine.");
}
