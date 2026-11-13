// SPDX-License-Identifier: MIT
#define EPI_SMMU_HOST_TEST
// Compile the actual implementation against memory/MMIO mocks.
#include "../../src/epinephrine/epi_smmu.cxx"

static void Baseline() {
    std::memset(mmio,0,sizeof(mmio)); std::memset(cc,0,sizeof(cc)); std::memset(gmu,0,sizeof(gmu));
    mmio[0]=0x002C0406; mmio[0x20/4]=0x4C017E06; mmio[0x24/4]=0x30000007;
    mmio[0x28/4]=0x5511; mmio[0x3C/4]=0x24;
    cc[0x106C/4]=0x80000000; cc[0x100C/4]=1; gmu[0]=1; gmu[0x340/4]=0xC0;
    dropOffset=0xFFFFFFFF; dropAlways=false; syncTimeout=false; failAfter=-1;
}
static ULONGLONG Walk(EPI_SMMU* c, ULONGLONG a) {
    // Independent descriptor walk using physical addresses, not implementation lookup.
    ULONGLONG* t=c->Root.Va;
    const unsigned shifts[]={30,21};
    for (unsigned shift:shifts) {
        ULONGLONG d=t[(a>>shift)&511]; assert((d&3)==3);
        t=nullptr;
        for (const auto& p:allocations) if (p.second==(d&0xFFFFFF000ULL)) t=static_cast<ULONGLONG*>(p.first);
        assert(t);
    }
    return t[(a>>12)&511];
}
static EPI_SMMU* NewMapped() {
    EPI_SMMU* c=nullptr; assert(EpiSmmuCreate(nullptr,&c)==0);
    assert(EpiSmmuMap(c,0x4000,{0x90000000},0xE000,EPI_SMMU_READ|EPI_SMMU_PRIVILEGED)==0);
    return c;
}
int main() {
    Baseline();
    EPI_SMMU* c=NewMapped();
    ULONGLONG pte=Walk(c,0x11000);
    assert((pte&0xFFFFFF000ULL)==0x9000D000);
    assert((pte&0xCC)==0x80); // privileged RO, AttrIndx0
    assert((pte&(1ULL<<11))!=0); // nG: ASID invalidation includes every leaf
    assert(EpiSmmuMap(c,0x4001,{0x90000000},0x1000,1)==STATUS_INVALID_PARAMETER);
    assert(EpiSmmuMap(c,0x100000000ULL,{0x90000000},0x1000,1)==STATUS_INVALID_PARAMETER);
    assert(EpiSmmuMap(c,0x1000,{0x1000000000LL},0x1000,1)==STATUS_INVALID_PARAMETER);
    assert(EpiSmmuMap(c,0x1000,{0x90000000},0x1000,2)==STATUS_INVALID_PARAMETER);
    // Overlap halfway through request must leave its initial hole untouched.
    assert(EpiSmmuMap(c,0x3000,{0xA0000000},0x2000,3)==STATUS_CONFLICTING_ADDRESSES);
    assert(Walk(c,0x3000)==0);
    assert(EpiSmmuUnmap(c,0x3000,0x2000)==STATUS_NOT_FOUND);
    assert(Walk(c,0x4000)!=0);
    // Cross both a 2MB and a 1GB boundary; also test the last legal IOVA page.
    assert(EpiSmmuMap(c,0x3FFFF000,{0xB0000000},0x3000,3)==0);
    assert((Walk(c,0x40001000)&0xFFFFFF000ULL)==0xB0002000);
    assert((Walk(c,0x40001000)&0xCC)==0x40); // unprivileged RW
    assert(EpiSmmuMap(c,0xFFFFF000,{0xFFFFFF000LL},0x1000,3)==0);
    assert((Walk(c,0xFFFFF000)&0xFFFFFF000ULL)==0xFFFFFF000ULL);
    // Allocation failures at each stage roll back tables and preserve old mappings.
    const SIZE_T count=allocations.size();
    for(int n=0;n<5;++n) {
        failAfter=n;
        NTSTATUS st=EpiSmmuMap(c,0x80000000,{0xC0000000},0x201000,3);
        failAfter=-1;
        assert(st==STATUS_INSUFFICIENT_RESOURCES);
        assert(allocations.size()==count); assert(Walk(c,0x4000)!=0);
    }
    ULONG saved[RTL_NUMBER_OF(ConfigOffsets)];
    for(ULONG i=0;i<RTL_NUMBER_OF(ConfigOffsets);++i) {
        saved[i]=0x120+i; mmio[ConfigOffsets[i]/4]=saved[i];
    }
    ULONG fsr=0,syn=0; ULONGLONG far=0;
    // Occupied owner and unexpected global configuration refuse without MMIO writes.
    ULONG before=mmioWrites; mmio[0]|=1;
    assert(EpiSmmuEnable(c,cc,gmu)==STATUS_NOT_SUPPORTED); assert(mmioWrites==before); mmio[0]&=~1UL;
    mmio[0x804/4]=0x80000006;
    assert(EpiSmmuEnable(c,cc,gmu)==STATUS_DEVICE_BUSY); assert(mmioWrites==before); mmio[0x804/4]=0;
    assert(EpiSmmuEnable(c,cc,gmu)==0);
    assert(mmio[0]==0x002C0406 && mmio[0x800/4]==0x84000005);
    assert(mmio[0x10030/4]==0x802020 && mmio[0x10010/4]==0x38011);
    assert(mmio[Cb/4]==0x1127 && !(mmio[Cb/4]&0x40)); // reporting on, IRQ off
    assert(EpiSmmuMap(c,0x60000000,{0xD0000000},0x1000,3)==STATUS_DEVICE_BUSY);
    assert(EpiSmmuUnmap(c,0x4000,0x1000)==STATUS_DEVICE_BUSY);
    assert(EpiSmmuDestroy(c)==STATUS_DEVICE_BUSY);
    EPI_SMMU* second=NewMapped(); assert(EpiSmmuEnable(second,cc,gmu)==STATUS_DEVICE_BUSY);
    assert(EpiSmmuDestroy(second)==0);
    gmu[0]=0; mmio[0x10058/4]=2; mmio[0x10060/4]=0x60004000; mmio[0x10064/4]=1; mmio[0x10068/4]=0x1234;
    before=mmioWrites;
    assert(EpiSmmuQueryFault(c,&fsr,&far,&syn)==0);
    assert(fsr==2 && far==0x160004000ULL && syn==0x1234 && mmioWrites==before);
    ULONG global[4]={0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF};
    const ULONG globalOffsets[]={0x48,0x50,0x54,0x58};
    for(ULONG i=0;i<4;++i) mmio[globalOffsets[i]/4]=0xA0+i;
    assert(EpiSmmuQueryGlobalFault(c,global)==0 && mmioWrites==before);
    for(ULONG i=0;i<4;++i) assert(global[i]==0xA0+i && mmio[globalOffsets[i]/4]==0xA0+i);
    assert(EpiSmmuQueryGlobalFault(nullptr,global)==STATUS_INVALID_PARAMETER);
    assert(EpiSmmuQueryGlobalFault(c,nullptr)==STATUS_INVALID_PARAMETER);
    cc[0x106C/4]=0;
    assert(EpiSmmuQueryGlobalFault(c,global)==STATUS_INVALID_DEVICE_STATE && mmioWrites==before);
    cc[0x106C/4]=0x80000000;
    assert(EpiSmmuDisable(c)==STATUS_INVALID_DEVICE_STATE && mmioWrites==before); gmu[0]=1;
    syncTimeout=true;
    assert(EpiSmmuDisable(c)==STATUS_IO_TIMEOUT && stalls>=1000);
    assert(EpiSmmuDestroy(c)==STATUS_DEVICE_BUSY); syncTimeout=false;
    assert(EpiSmmuDisable(c)==0);
    for(ULONG i=0;i<RTL_NUMBER_OF(ConfigOffsets);++i) assert(mmio[ConfigOffsets[i]/4]==saved[i]);
    assert(mmio[0]==0x002C0406 && mmio[0x800/4]==0 && mmio[Cb/4]==0);
    assert(EpiSmmuDisable(c)==0);
    // Inject a dropped setup write: Enable must restore baseline and release ownership.
    dropOffset=0x10030;
    assert(EpiSmmuEnable(c,cc,gmu)==STATUS_IO_DEVICE_ERROR);
    assert(c->Regs==nullptr && Owner==nullptr);
    assert(EpiSmmuEnable(c,cc,gmu)==0);
    // Failed restoration keeps the tables pinned and allows a later retry.
    dropOffset=0x1800; dropAlways=true;
    assert(EpiSmmuDisable(c)==STATUS_IO_DEVICE_ERROR);
    assert(EpiSmmuDestroy(c)==STATUS_DEVICE_BUSY);
    dropOffset=0xFFFFFFFF; assert(EpiSmmuDisable(c)==0);
    assert(EpiSmmuUnmap(c,0x4000,0xE000)==0);
    assert(EpiSmmuDestroy(c)==0); assert(allocations.empty() && Owner==nullptr);
    // GPU IDs 0/1 use separate DT-compatible SMRs, sharing CB0 without matching each other or GMU ID5.
    Baseline(); c=NewMapped();
    assert(EpiSmmuConfigureGpuStreams(c)==0);
    assert(EpiSmmuMap(c,0x70000000,{0xA0000000},0xA000,EPI_SMMU_READ)==0);
    assert(EpiSmmuMap(c,0x70010000,{0xA0010000},0x8000,EPI_SMMU_READ|EPI_SMMU_WRITE)==0);
    assert(EpiSmmuMap(c,0x70020000,{0xA0020000},0x1000,EPI_SMMU_READ|EPI_SMMU_WRITE|EPI_SMMU_PRIVILEGED)==0);
    assert((Walk(c,0x70000000)&0xC0)==0xC0); // SQE RO, unprivileged reads allowed
    assert((Walk(c,0x70010000)&0xC0)==0x40); // ring RW
    assert((Walk(c,0x70020000)&0xC0)==0); // RPTR privileged RW
    mmio[0x804/4]=0x44; mmio[0xC04/4]=0x200;
    mmio[0x808/4]=0x88; mmio[0xC08/4]=0x300;
    assert(EpiSmmuEnable(c,cc,gmu)==0);
    assert(mmio[0x800/4]==0x84000005 && mmio[0x804/4]==0x84000000 && mmio[0xC04/4]==0);
    assert(mmio[0x808/4]==0x84000001 && mmio[0xC08/4]==0);
    for(ULONG i=0;i<2;++i) {
        const ULONG smr=mmio[(0x804+i*4)/4], mask=(smr>>16)&0x7FFF, id=smr&0x7FFF;
        assert(mask==0x400 && id==i && ((i^id)&~mask)==0 && (((i|0x400)^id)&~mask)==0);
        assert((((i^1)^id)&~mask)!=0 && ((5^id)&~mask)!=0);
    }
    assert(EpiSmmuConfigureGpuStreams(c)==STATUS_DEVICE_BUSY);
    // Failed invalidation must leave ownership/tables pinned, before disabling CB0.
    dropOffset=0x804; dropAlways=true;
    assert(EpiSmmuDisable(c)==STATUS_IO_DEVICE_ERROR && Owner==c && mmio[Cb/4]==0x1127);
    assert(mmio[0x808/4]==0x88); // the other GPU stream is still invalidated on this failure
    assert(EpiSmmuDestroy(c)==STATUS_DEVICE_BUSY);
    // Failed S2CR1 restore leaves both streams invalid and the CB stopped, and remains retryable.
    dropOffset=0xC04;
    assert(EpiSmmuDisable(c)==STATUS_IO_DEVICE_ERROR && Owner==c && mmio[Cb/4]==0);
    assert(!(mmio[0x800/4]&0x80000000) && !(mmio[0x804/4]&0x80000000) && !(mmio[0x808/4]&0x80000000));
    assert(mmio[0xC08/4]==0x300); // S2CR2 restore is attempted despite the S2CR1 mismatch
    dropOffset=0xFFFFFFFF;
    assert(EpiSmmuDisable(c)==0 && Owner==nullptr);
    assert(mmio[0x804/4]==0x44 && mmio[0xC04/4]==0x200);
    assert(mmio[0x808/4]==0x88 && mmio[0xC08/4]==0x300);
    EPI_SMMU_GPU_STREAMS_TRACE streams={};
    assert(EpiSmmuQueryGpuStreams(c,&streams)==0 && streams.RestoreStatus==0);
    for(ULONG i=0;i<2;++i) {
        assert(streams.SmrReadback[i]==(0x84000000|i) && streams.S2crReadback[i]==0);
        assert(streams.SmrRestored[i]==(i==0 ? 0x44UL : 0x88UL) && streams.S2crRestored[i]==(i==0 ? 0x200UL : 0x300UL));
    }
    // The second GPU stream has the same retention and retry rules.
    assert(EpiSmmuEnable(c,cc,gmu)==0); dropOffset=0x808; dropAlways=true;
    assert(EpiSmmuDisable(c)==STATUS_IO_DEVICE_ERROR && Owner==c && mmio[Cb/4]==0x1127);
    assert(mmio[0x804/4]==0x44 && EpiSmmuDestroy(c)==STATUS_DEVICE_BUSY);
    dropOffset=0xFFFFFFFF; assert(EpiSmmuDisable(c)==0);
    assert(EpiSmmuEnable(c,cc,gmu)==0); dropOffset=0xC08;
    assert(EpiSmmuDisable(c)==STATUS_IO_DEVICE_ERROR && Owner==c && mmio[Cb/4]==0);
    dropOffset=0xFFFFFFFF; assert(EpiSmmuDisable(c)==0);
    // Failure in either GPU S2CR/SMR setup rolls back both pairs and the GMU path.
    const ULONG setupFailures[]={0xC04,0xC08,0x804,0x808};
    for(ULONG off:setupFailures) {
        dropOffset=off; dropAlways=false;
        assert(EpiSmmuEnable(c,cc,gmu)==STATUS_IO_DEVICE_ERROR && c->Regs==nullptr && Owner==nullptr);
        assert(mmio[0x804/4]==0x44 && mmio[0xC04/4]==0x200);
        assert(mmio[0x808/4]==0x88 && mmio[0xC08/4]==0x300);
    }
    assert(EpiSmmuDestroy(c)==0 && allocations.empty());
    std::puts("SMMU tests passed: descriptors, permissions, boundaries, rollback, faults, TLB timeout, restoration.");
}
