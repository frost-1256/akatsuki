// SPDX-License-Identifier: MIT
#define EPI_GMU_HOST_TEST
#include "../../src/epinephrine/epi_zap.cxx"
#include "../../src/epinephrine/epi_gmu.cxx"
#include <fstream>
#include <iterator>
struct EPI_SMMU { bool enabled, gpuStreams; };
struct TEST_MAP { ULONGLONG Iova; SIZE_T Size; ULONG Flags; ULONGLONG Pa; };
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
    testMaps.push_back({iova,size,flags,static_cast<ULONGLONG>(pa.QuadPart)});
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
static ULONG testFaultFsr=2;
NTSTATUS EpiSmmuQueryFault(EPI_SMMU*,ULONG* fsr,ULONGLONG* addr,ULONG* syn) {*fsr=testFaultFsr;*addr=0x4000;*syn=0;return 0;}
static ULONG testGlobalCalls=0;
static NTSTATUS testGlobalStatus=STATUS_SUCCESS;
NTSTATUS EpiSmmuQueryGlobalFault(EPI_SMMU* c,ULONG* values) {
    if(!c || !values) return STATUS_INVALID_PARAMETER;
    ++testGlobalCalls; if(!NT_SUCCESS(testGlobalStatus)) return testGlobalStatus;
    for(ULONG i=0;i<4;++i) values[i]=0x100*testGlobalCalls+i;
    return STATUS_SUCCESS;
}
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
static std::vector<UCHAR> ZapImage() {
    std::vector<UCHAR> zap(EPI_ZAP_FILE_SIZE,0xA5);
    const UCHAR ident[]={0x7F,'E','L','F',1,1,1,0,0,0,0,0,0,0,0,0};
    const ULONG header[]={0x00A40002,1,0x5000,0x34,0,3,0x00200034,3,0};
    const ULONG ph[3][8]={{0,0,0,0,0x94,0,0x07000000,0},
        {0,0x1000,0x6000,0x6000,0x19A0,0x2000,0x02200000,0x1000},
        {1,0x3000,0x5000,0x5000,0x7B8,0x7B8,0x08000007,0x100000}};
    std::memcpy(zap.data(),ident,sizeof(ident)); std::memcpy(zap.data()+16,header,sizeof(header));
    std::memcpy(zap.data()+52,ph,sizeof(ph)); return zap;
}
static std::vector<EPI_SMC_REGS> smcInputs;
static ULONG smcInterrupts=0,smcBusy=0,smcFailCmd=0xFFFFFFFF,smcWaitCmd=0xFFFFFFFF;
static ULONGLONG smcResumeFunction=0;
static ULONG smcResultFailureCmd=0xFFFFFFFF;
static ULONGLONG smcSupportedResult=1;
static bool smcQueryPeer=false;
static ULONG smcQueryNegativeRow=0xFFFFFFFF,smcQueryUnexpectedRow=0xFFFFFFFF;
extern "C" VOID EpiSmcInvoke(EPI_SMC_REGS* r) {
    smcInputs.push_back(*r);
    const ULONGLONG function=r->X[0]==1 ? smcResumeFunction : r->X[0];
    if(r->X[0]==1) assert(r->X[6]==0x1234);
    else smcResumeFunction=r->X[0];
    if(smcInterrupts) {--smcInterrupts; r->X[0]=1; r->X[6]=0x1234; return;}
    if(smcBusy) {--smcBusy; r->X[0]=static_cast<ULONGLONG>(-12LL); return;}
    if(smcQueryPeer) {
        const ULONG row=diagnostics[L"EpiGpuProbeStep"]-0x1600;
        const ULONGLONG funcs[]={0x42000601,0xC2000601,0x42000601,0x42000601,0x42000601,0x42000601,
                                0x42000207,0x42000207,0x42000207,0x42000207,0x42000207,0x42000207};
        const ULONGLONG args[]={0x02000601,0x02000601,0x02000207,0x02000201,0x02000202,0x02000205,1,18,6,4,9,13};
        assert(row<12 && function==funcs[row] && r->X[1]==1 && r->X[2]==args[row]);
        for(ULONG i=3;i<=5;++i) assert(r->X[i]==0);
        assert(r->X[7]==0);
        r->X[0]=(row==smcQueryNegativeRow ? static_cast<ULONGLONG>(-3LL) : (row==smcQueryUnexpectedRow ? 2 : 0));
        r->X[1]=(row<6 ? 1 : 0); r->X[2]=0xABC002+row; r->X[3]=0xABC003+row; r->X[6]=0xABC006+row;
        return;
    }
    const ULONG cmd=static_cast<ULONG>(function)&0xFF;
    assert(function==(0x42000200ULL|cmd) && r->X[2]==13 && r->X[5]==0 && r->X[7]==0);
    if(cmd==smcWaitCmd) {r->X[0]=2; return;}
    if(cmd==smcFailCmd) {r->X[0]=static_cast<ULONGLONG>(-3LL); return;}
    if(cmd==smcResultFailureCmd) {r->X[0]=0; r->X[1]=2; return;}
    if(cmd==1) {assert(r->X[1]==0x82 && (r->X[3]&0xFFF)==0);}
    if(cmd==2) {assert(r->X[1]==3 && r->X[3]==0x8B110000 && r->X[4]==0x1000);}
    r->X[0]=0; r->X[1]=(cmd==7 ? smcSupportedResult : 0); r->X[2]=0; r->X[3]=0;
    if(cmd==5) z1Authorized=true;
}
static void Reset() {
    allowQ1Writes=false; q1Active=false; q1Pending=false; q1FenceForever=false; q1ShadowOnly=false; q1SaveFails=false;
    q1Zero=false; q1Pad=false; q1Bg=false; q1Outside=false; q1BackgroundBad=false;
    q1StopFrame=0xFFFFFFFF; q1WrongFrame=0xFFFFFFFF; q1ErrorFrame=0xFFFFFFFF; q1BadSizeFrame=0xFFFFFFFF;
    q1FenceDrops=0; q1DelayUs=0; q1PendingFrame=0; q1DamageRing=0xFFFFFFFF; q1DamageOutside=0xFFFFFFFF; q1Due=0;
    q1Writes.clear(); q1Frames.clear(); q1Progress.clear(); q1Vertices.clear();

    allowM1Writes=false; m1Active=false; m1Pending=false; m1Completes=true; m1SaveFails=false;
    m1NoClear=false; m1NoLoad=false; m1NoStore=false; m1FenceForever=false; m1BadSize=false; m1ShadowOnly=false;
    m1DelayUs=0; m1StopTile=0xFFFFFFFF; m1ErrorBits=0; m1FenceDrops=0; m1WrongTile=0xFFFFFFFF;
    m1DamageRing=0xFFFFFFFF; m1DamageOutside=0xFFFFFFFF; m1DamagePixel=0xFFFFFFFF; m1Due=0;
    m1Writes.clear(); m1Tiles.clear(); std::fill(m1TileMemory.begin(),m1TileMemory.end(),0xEEEEEEEE);

    allowW1Writes=false; w1Active=false; w1FenceForever=false; w1Pending=false; w1ShadowOnly=false; w1SaveFails=false;
    w1FenceDrops=0; w1StopPub=0xFFFFFFFF; w1BadSizePub=0xFFFFFFFF; w1WrongPub=0xFFFFFFFF; w1ErrorPub=0xFFFFFFFF;
    w1DamageRing=0xFFFFFFFF; w1DamageOutside=0xFFFFFFFF; w1Cursor=2287; w1NextPacket=0; w1DelayUs=0;
    w1Writes.clear(); w1Pubs.clear(); w1ReadPackets.clear(); w1Progress.clear();
    t1CopyTrigger=nullptr; t1CorruptTarget=nullptr;
    allowT1Writes=false; t1Completes=true; t1Zero=false; t1Black=false; t1Pad=false; t1Transpose=false; t1FlipX=false; t1FlipY=false;
    t1Outside=false; t1BoxDamage=false; t1Odd=true; t1FenceForever=false; t1BadRemaining=false; t1SaveFails=false; t1Pending=false;
    t1Fences=7; t1FenceDrops=0; t1ErrorBits=0; t1DelayUs=0; t1BadTexel=0xFFFFFFFF; t1Due=0; t1Writes.clear();
    testInterruptTime=100000; allowA1Writes=false; a1FenceForever=false; a1BadSq=false; a1BadBg=false; a1Outside=false; a1Odd=true; a1SaveFails=false;
    a1StopFrame=0xFFFFFFFF; a1WrongFrame=0xFFFFFFFF; a1ErrorFrame=0xFFFFFFFF; a1FenceDrops=0; a1DelayUs=0; a1Fences=3;
    a1Pending=0xFFFFFFFF; a1Due=0; a1Writes.clear(); a1Frames.clear(); a1Progress.clear(); a1Vertices.clear();
    EpiGmuStackReset(); testRemainingStack=72*1024; testExpandStatus=STATUS_SUCCESS; testExpandCalls=0;
    allowV1Writes=false; v1Completes=true; v1Odd=true; v1BadAlpha=false; v1Outside=false; v1CoreOut=false; v1EdgeOnly=false;
    v1FenceForever=false; v1BadRptr=false; v1SaveFails=false; v1Fences=7; v1FenceDrops=0; v1ErrorBits=0; v1Scatter=0xFFFFFFFF; v1BadCore=0xFFFFFFFF;
    v1Delta[0]=0; v1Delta[1]=0; v1Delta[2]=0; v1Writes.clear();
    allowD1Writes=false; d1Completes=true; d1Odd=true; d1Fences=true; d1Outside=false; d1FenceForever=false;
    d1MapFails=false; d1Restoring=false; d1RestoreDrop=0xFFFFFFFF; d1ErrorBits=0; d1FenceDrops=0;
    d1Maps=0; d1Unmaps=0; d1Writes.clear();
    for(ULONG i=0;i<d1Band.size();++i) d1Band[i]=0xAB000000+i;

    r1CopyTarget=nullptr; r1CopyBadDword=0xFFFFFFFF;
    r1RectEnd=12;
    allowR1Writes=false; r1Completes=true; r1Draw=true; r1Ccu=true; r1Event=true; r1FenceForever=false; r1BadRptr=false;
    r1FenceDrops=0; r1ErrorBits=0; r1BadSentinel=0xFFFFFFFF; r1BadPixel=0xFFFFFFFF; r1Color=0x000033FF; r1Scatter=0xFFFFFFFF; r1Writes.clear();
    allowS1Writes=false; s1Completes=true; s1Store=true; s1Event=true; s1FenceForever=false; s1BadRptr=false;
    s1FenceDrops=0; s1ErrorBits=0; s1BadGuard=0xFFFFFFFF; s1Scatter=0xFFFFFFFF; s1Writes.clear();
    smcQueryPeer=false; smcQueryNegativeRow=0xFFFFFFFF; smcQueryUnexpectedRow=0xFFFFFFFF;
    zapQuerySnapshots.clear(); failZapQuerySave=0xFFFFFFFF;
    if(ZapRetainedMetadata) {GmuFree(ZapRetainedMetadata,0x2000,0); ZapRetainedMetadata=nullptr;}
    ZapOwner=nullptr; // host-only reboot simulation; production never resets a consumed PAS guard
    ZapBootClaim=FALSE;
    allowZ1Writes=false; z1Completes=true; z1Fill=true; z1Event=true; z1Authorized=false;
    z1Writes.clear(); std::memset(z1Reserved,0xCC,sizeof(z1Reserved));
    smcInputs.clear(); smcInterrupts=0; smcBusy=0; smcFailCmd=0xFFFFFFFF; smcWaitCmd=0xFFFFFFFF;
    smcResumeFunction=0;
    smcResultFailureCmd=0xFFFFFFFF;
    smcSupportedResult=1;
    allowG5bWrites=false; g5bCompletes=true; g5bFill=true; g5bEvent=true; g5bFenceForever=false; g5bBadRptr=false;
    g5bFenceDrops=0; g5bErrorBits=0; g5bBadSentinel=0xFFFFFFFF; g5bWrites.clear(); testGlobalCalls=0; testGlobalStatus=STATUS_SUCCESS;
    allowG5Writes=false; g5Completes=true; g5Flush=true; g5Event=true; g5Fill=true; g5FenceForever=false; g5BadRptr=false;
    g5FenceDrops=0; g5ErrorBits=0; g5Color=0x78563412; g5BadPixel=0xFFFFFFFF; g5BadSentinel=0xFFFFFFFF; g5Writes.clear();
    allowG4Writes=false; g4Completes=true; g4Scratch=true; g4Mem=true; g4Event=true; g4FenceForever=false;
    g4Scratch2Damage=false; g4FenceDrops=0; g4ErrorBits=0; g4Damage=0; g4BadGuards=0; g4Writes.clear();
    allowG3cWrites=false; g3cCompletes=true; g3cEvent=true; g3cBadGuard=false; g3cFenceForever=false;
    g3cShowStatus=true; g3cShowUnmasked=true; g3cIgnoreClear=false; g3cPending=false; g3cPolling=false;
    g3cFenceDrops=0; g3cErrorBits=0; g3cDamage=0; g3cStatusDelay=0; g3cUnmaskedDelay=0; g3cSample=0;
    g3cMaskChangeStep=0xFFFFFFFF; g3cWrites.clear(); g3cClearWrites.clear();
    allowG3bWrites=false; g3bCompletes=true; g3bEvent=true; g3bBadGuard=false; g3bFenceForever=false;
    g3bFenceDrops=0; g3bErrorBits=0; g3bDamage=0; g3bWrites.clear();
    allowG3Writes=false; g3Completes=true; g3Scratch=true; g3Mem=true; g3Reg=true;
    g3BadGuard1=false; g3BadGuard3=false; g3FenceForever=false; g3FenceDrops=0; g3ErrorBits=0;
    g3Writes.clear(); testFaultFsr=2;
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
static ULONG RunG2(const std::vector<UCHAR>& f,const std::vector<UCHAR>& sqe,BOOLEAN* keep,BOOLEAN g3=false,BOOLEAN g3b=false,BOOLEAN g3c=false,BOOLEAN g4=false,BOOLEAN g5=false,BOOLEAN g5b=false,BOOLEAN z1=false,BOOLEAN s1=false,BOOLEAN r1=false,BOOLEAN d1=false,BOOLEAN v1=false,BOOLEAN a1=false,BOOLEAN t1=false,BOOLEAN w1=false,BOOLEAN m1=false,BOOLEAN q1=false) {
    WCHAR text[3072]={}; allowStageFWrite=true; allowG1Writes=true; allowG2Writes=true; allowG3Writes=g3; allowG3bWrites=g3b;
    allowG3cWrites=g3c;
    allowG4Writes=g4;
    allowG5Writes=g5;
    allowG5bWrites=g5b;
    allowZ1Writes=z1; const auto zap=ZapImage();
    allowS1Writes=s1;
    allowR1Writes=r1; allowD1Writes=d1; allowV1Writes=v1; allowA1Writes=a1; allowT1Writes=t1; allowW1Writes=w1; allowM1Writes=m1; allowQ1Writes=q1;
    const ULONG disp[]={0xE1000000,0,1080,2400,4320,21};
    return EpiGmuBootTest(reinterpret_cast<HANDLE>(1),reinterpret_cast<HANDLE>(2),cc,f.data(),static_cast<ULONG>(f.size()),
                          text,3072,keep,true,true,true,true,true,true,true,true,true,true,true,true,true,
                          sqe.data(),static_cast<ULONG>(sqe.size()),true,g3,g3b,g3c,g4,g5,g5b,z1,zap.data(),static_cast<ULONG>(zap.size()),s1,r1,d1,d1 ? disp : nullptr,v1,a1,t1,w1,m1,q1);
}
// Host reset simulation only. Production deliberately retains these allocations on teardown failure.
static void ClearQuarantine() {
    GMU_TEST* s=g_KeptGmu; assert(s);
    if(s->Smmu) {s->Smmu->enabled=false; EpiSmmuDestroy(s->Smmu);}
    for(ULONG i=0;i<s->Count;++i) if(!s->Regions[i].External && s->Regions[i].Va) GmuFree(s->Regions[i].Va,s->Regions[i].Size,0);
    ExFreePoolWithTag(s,EPI_GMU_TAG); g_KeptGmu=nullptr;
}
int main(int argc,char** argv) {
    std::vector<UCHAR> f=Image(); BOOLEAN keep=false;
    // Probe2 is optional, but R-1 must have exactly the full S-1 value and no unknown bits.
    assert(EpiGmuProbe2Valid(0,0) && EpiGmuProbe2Valid(0x80,0) && EpiGmuProbe2Valid(0xBFFFFE80,1));
    for(ULONG invalid:{0UL,0x3FFFFE80UL,0x80000000UL,0xFFFFFF80UL}) assert(!EpiGmuProbe2Valid(invalid,1));
    for(ULONG invalid:{2UL,4UL,5UL,6UL,8UL,9UL,14UL,16UL,30UL,32UL,62UL,64UL,0x80000000UL,0xFFFFFFFFUL}) assert(!EpiGmuProbe2Valid(0xBFFFFE80,invalid));
    assert(EpiGmuProbe2Valid(0xBFFFFE80,3) && !EpiGmuProbe2Valid(0x3FFFFE80,3));
    {
        ULONG disp[]={0xE1000000,0,1080,2400,4320,21};
        assert(EpiGmuD1DisplayMatches(disp)); disp[5]=22; assert(EpiGmuD1DisplayMatches(disp));
        for(ULONG i=0;i<6;++i) {const ULONG v=disp[i]; disp[i]=0xFFFFFFFF; assert(!EpiGmuD1DisplayMatches(disp)); disp[i]=v;}
        assert(!EpiGmuD1DisplayMatches(nullptr));
    }
    assert(EpiGmuProbe2Valid(0xBFFFFE80,7) && !EpiGmuProbe2Valid(0x3FFFFE80,7));
    // Integer edge classification and six documented sample expectations, independent of the GPU peer.
    {
        ULONG count[3]={};
        for(ULONG row=0;row<128;++row) for(ULONG col=412;col<668;++col) ++count[GmuV1Classify(col,row)];
        assert(count[V1_CORE_IN]==15500 && count[V1_CORE_OUT]==15500 && count[V1_EDGE]==1768);
        const ULONG sample[][3]={{540,85,0xFF555556},{540,6,0xFFF20607},{420,124,0xFF07F305},
            {660,124,0xFF0704F4},{540,124,0xFF077C7D},{476,100,0xFF37A325}};
        for(const auto& pt:sample) {
            assert(GmuV1Classify(pt[0],pt[1])==V1_CORE_IN && GmuV1Expected(pt[0],pt[1])==pt[2]);
        }
        ULONG maxErr[3]={};
        assert(GmuV1WithinTolerance(0xFF535754,0xFF555556,maxErr) && maxErr[0]==2 && maxErr[1]==2 && maxErr[2]==2);
        assert(!GmuV1WithinTolerance(0xFF555559,0xFF555556,maxErr) && maxErr[0]==3);
        assert(!GmuV1WithinTolerance(0xFE555556,0xFF555556,maxErr));
    }
    assert(EpiGmuProbe2Valid(0xBFFFFE80,15) && !EpiGmuProbe2Valid(0x3FFFFE80,15));
    {
        ULONG sq=0,bg=0;
        for(ULONG v=0;v<128;++v) for(ULONG c=412;c<668;++c) {
            if(c>=461 && c<=618 && v>=25 && v<=102) {++sq; assert(GmuA1Expected(c,v)==A1PeerColor(c,v));}
            else if(c<=458 || c>=621 || v<=22 || v>=105) ++bg;
        }
        assert(sq==12324 && bg==19484 && GmuA1WptrEnd==2278);
        const ULONG samples[][3]={{540,64,0xFF7F0181},{463,27,0xFFF9060B},{616,27,0xFF06EE0B},{616,100,0xFF0606F4},
            {463,100,0xFFF9EEF4},{500,44,0xFFBE0141},{580,84,0xFF3F01C1}};
        for(const auto& pt:samples) assert(GmuA1Expected(pt[0],pt[1])==pt[2]);
    }
    assert(EpiGmuProbe2Valid(0xBFFFFE80,31) && !EpiGmuProbe2Valid(0x3FFFFE80,31));
    {
        const ULONG samples[][3]={{0,0,0xFF101040},{7,0,0xFFF010C0},{7,7,0xFFF0F040},{0,7,0xFF10F0C0},
            {3,3,0xFF707040},{4,5,0xFF90B0C0},{1,1,0xFF303040}};
        for(const auto& pt:samples) assert(GmuT1Texel(pt[0],pt[1])==pt[2]);
        for(ULONG j=0;j<8;++j) for(ULONG i=8;i<16;++i) assert(GmuT1Word(0x6C00+(j*16+i)*4)==0xFFFF00FF);
    }
    assert(EpiGmuProbe2Valid(0xBFFFFE80,63) && !EpiGmuProbe2Valid(0x3FFFFE80,63));
    {
        GMU_G1_TRACE g1; GmuG1TraceInit(&g1); assert(g1.Value[4]==0x20C);
        GmuG1TraceInit(&g1,TRUE); assert(g1.Value[4]==0x20B);
        ULONG model[4096]={}; // host-only reference; the driver computes each expected word without this array
        for(ULONG i=0;i<1000;++i) for(ULONG j=0;j<9;++j) model[(2287+9*i+j)%4096]=GmuW1Word(i,j);
        for(ULONG i=0;i<4096;++i) assert(model[i]==GmuW1Expected(i));
        assert(model[0]==0x70016400 && model[4095]==0x70BF8003 && model[3095]==0xB1000220);
    }
    assert(EpiGmuProbe2Valid(0xBFFFFE80,127) && !EpiGmuProbe2Valid(0x3FFFFE80,127));
    for(ULONG invalid:{65UL,95UL,126UL,128UL,254UL,256UL,511UL}) assert(!EpiGmuProbe2Valid(0xBFFFFE80,invalid));
    assert(sizeof(GMU_M1_TRACE)==600 && RTL_NUMBER_OF(GmuM1Packet)==177 && RTL_NUMBER_OF(GmuM1Ibs)==18);
    assert(GmuM1Packet[0]==0x70BF8003 && GmuM1Packet[1]==0x70015B80 && GmuM1Packet[176]==0xCAFE001A);
    assert(EpiGmuProbe2Valid(0xBFFFFE80,255) && !EpiGmuProbe2Valid(0x3FFFFE80,255));
    assert(sizeof(GMU_Q1_TRACE)==552 && RTL_NUMBER_OF(GmuQ1Ib)==107);
    {
        ULONG model[4096];
        for(ULONG i=0;i<4096;++i) model[i]=i>=3095 && i<3272 ? GmuM1Packet[i-3095] : GmuW1Expected(i);
        for(ULONG fr=0;fr<121;++fr) for(ULONG j=0;j<9;++j) model[(3272+9*fr+j)%4096]=GmuQ1Word(fr,j);
        for(ULONG i=0;i<4096;++i) assert(model[i]==GmuQ1Expected(i));
        assert(model[4091]==0x70BF8003 && model[0]==4 && model[3]==0xA200005B && model[264]==0xA2000078);
        for(ULONG pass=0;pass<2;++pass) for(ULONG i=0;i<48;++i) assert(GmuQ1VertexWord(0,pass!=0,48+i)==GmuT1VertexWord(pass!=0,i));
        assert(GmuQ1VertexWord(0,false,4)==0x41080000 && GmuQ1VertexWord(0,false,5)==0x3F000000);
    }
    // Z-1b: only read-only SMCs; independent of ZapOnce and every GPU power/memory prerequisite.
    {
        EPI_ZAP_QUERY_TRACE q;
        Reset(); smcQueryPeer=true; ZapBootClaim=TRUE; ZapOwner=reinterpret_cast<PVOID>(1);
        assert(EpiZapQuery(nullptr,nullptr,&q)==0 && q.Count==12 && q.Status==0);
        assert(smcInputs.size()==12 && zapQuerySnapshots.size()==14 && diagnostics[L"EpiGpuProbeStep"]==0xFFFF);
        assert(ZapBootClaim && ZapOwner==reinterpret_cast<PVOID>(1) && allocations.empty() && testMaps.empty() && gmuWrites==0);
        assert(q.Row[0].Function==0x42000601 && q.Row[1].Function==0xC2000601);
        assert(q.Row[2].Arg==0x02000207 && q.Row[3].Arg==0x02000201 && q.Row[4].Arg==0x02000202 && q.Row[5].Arg==0x02000205);
        const ULONGLONG ids[]={1,18,6,4,9,13};
        for(ULONG i=0;i<6;++i) assert(q.Row[6+i].Arg==ids[i]);
        for(ULONG i=0;i<12;++i) {
            assert(q.Row[i].Calls==1 && q.Row[i].Flags==0 && q.Row[i].X0==0);
            assert(q.Row[i].X2==0xABC002+i && q.Row[i].X3==0xABC003+i && q.Row[i].X6==0xABC006+i);
            for(ULONG j=3;j<8;++j) assert(smcInputs[i].X[j]==0);
            EPI_ZAP_QUERY_TRACE saved; std::memcpy(&saved,zapQuerySnapshots[i+1].data(),sizeof(saved));
            assert(saved.Count==i+1 && saved.Row[i].Calls==1 && saved.Row[i].X6==q.Row[i].X6);
            if(i<11) assert(saved.Row[i+1].X0==~0ULL && saved.Row[i+1].Calls==0);
        }
        Reset(); smcQueryPeer=true; smcQueryNegativeRow=2; smcQueryUnexpectedRow=4;
        assert(EpiZapQuery(nullptr,nullptr,&q)==0 && q.Count==12 && q.Row[2].X0==static_cast<ULONGLONG>(-3LL) && q.Row[4].X0==2);
        Reset(); smcQueryPeer=true; smcInterrupts=2;
        assert(EpiZapQuery(nullptr,nullptr,&q)==0 && q.Row[0].Calls==3 && q.Row[0].Flags==1);
        assert(smcInputs[1].X[0]==1 && smcInputs[1].X[6]==0x1234 && smcInputs[1].X[2]==0x02000601);
        EPI_ZAP_QUERY_TRACE saved; std::memcpy(&saved,zapQuerySnapshots[1].data(),sizeof(saved));
        assert(saved.Count==1 && saved.Row[0].X0==1 && saved.Row[0].Flags==1 && saved.Row[0].Calls==1);
        Reset(); smcQueryPeer=true; smcBusy=2;
        assert(EpiZapQuery(nullptr,nullptr,&q)==0 && q.Row[0].Calls==3 && q.Row[0].Flags==2);
        assert(smcInputs[1].X[0]==0x42000601 && smcInputs[1].X[6]==0);
        Reset(); smcQueryPeer=true; smcBusy=21;
        assert(EpiZapQuery(nullptr,nullptr,&q)==STATUS_DEVICE_BUSY && q.Count==12);
        assert(q.Row[0].Calls==21 && q.Row[0].Flags==6 && q.Row[11].Calls==1 && smcInputs.size()==32);
        Reset(); smcQueryPeer=true; smcInterrupts=1024;
        assert(EpiZapQuery(nullptr,nullptr,&q)==STATUS_IO_TIMEOUT && q.Count==12);
        assert(q.Row[0].Calls==1024 && q.Row[0].Flags==5 && q.Row[11].Calls==1);
        Reset(); smcQueryPeer=true; failGmuStep=0x1603;
        assert(EpiZapQuery(nullptr,nullptr,&q)==STATUS_IO_DEVICE_ERROR && q.Count==3 && smcInputs.size()==3);
        Reset(); smcQueryPeer=true; failZapQuerySave=1;
        assert(EpiZapQuery(nullptr,nullptr,&q)==STATUS_IO_DEVICE_ERROR && q.Count==1 && smcInputs.size()==1);
        Reset(); smcQueryPeer=true; failZapQuerySave=0;
        assert(EpiZapQuery(nullptr,nullptr,&q)==STATUS_IO_DEVICE_ERROR && q.Count==0 && smcInputs.empty());
        assert(EpiZapQuery(nullptr,nullptr,nullptr)==STATUS_INVALID_PARAMETER);
        Reset();
    }
    // PAS/SMC is a pure host peer here: no real SMC, no physical reserved-memory access.
    {
        auto zap=ZapImage(); EPI_ZAP_TRACE pas={};
        const auto checkpoint=[](PVOID,ULONG step)->NTSTATUS {return GmuDword(nullptr,L"EpiGpuProbeStep",step);};
        Reset(); allowZ1Writes=true;
        assert(EpiZapValidate(zap.data(),static_cast<ULONG>(zap.size())));
        for(ULONGLONG reply:{0ULL,2ULL,~0ULL}) {
            Reset(); allowZ1Writes=true; smcSupportedResult=reply;
            assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==0);
            assert(pas.Call[0].Result[0]==0 && pas.Call[0].Result[1]==reply && pas.SupportX1Ignored==1);
            assert(smcInputs.size()==4 && pas.InitSucceeded==1 && GmuZ1PasSucceeded(&pas));
            for(ULONG index=0;index<4;++index) {
                auto bad=pas; bad.Call[index].Completed=0; assert(!GmuZ1PasSucceeded(&bad));
                bad=pas; bad.Call[index].Result[0]=2; assert(!GmuZ1PasSucceeded(&bad));
                if(index!=0) {bad=pas; bad.Call[index].Result[1]=1; assert(!GmuZ1PasSucceeded(&bad));}
            }
        }
        Reset(); allowZ1Writes=true; // restore the original fixture for the pre-existing Z-1 cases
        for(ULONG off:{0UL,16UL,28UL,44UL,52UL,84UL,116UL}) {
            auto bad=zap; bad[off]^=1;
            assert(!EpiZapValidate(bad.data(),static_cast<ULONG>(bad.size())));
        }
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==0);
        assert(smcInputs.size()==4 && pas.InitSucceeded==1 && pas.Consumed==1 && pas.MetadataKept==0 && allocations.empty());
        assert(pas.SupportX1Ignored==1);
        assert(pas.Header[0]==2 && pas.Header[1]==164 && pas.Header[9]==3 && pas.MetadataBytes==6708);
        assert(std::memcmp(z1Reserved,zap.data()+0x3000,0x7B8)==0);
        for(ULONG off=0x7B8;off<0x1000;++off) assert(reinterpret_cast<UCHAR*>(z1Reserved)[off]==0);
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_DEVICE_BUSY);
        assert(smcInputs.size()==4);
        ZapOwner=nullptr; // Simulate driver reload without a machine reboot: the volatile claim still blocks INIT.
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_DEVICE_BUSY);
        assert(smcInputs.size()==4 && pas.InitSucceeded==0 && pas.Consumed==1);
        for(ULONG cmd:{7UL,1UL,2UL,5UL}) {
            Reset(); allowZ1Writes=true; smcFailCmd=cmd;
            assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_IO_DEVICE_ERROR);
            assert(allocations.empty() && z1Writes.empty());
            assert(pas.InitSucceeded==(cmd==2 || cmd==5 ? 1UL : 0UL));
            if(cmd==7 || cmd==1) assert(ZapOwner==nullptr);
            if(cmd==7) assert(pas.SupportX1Ignored==0 && smcInputs.size()==1 && pas.InitSucceeded==0);
        }
        Reset(); allowZ1Writes=true; smcInterrupts=2;
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==0);
        assert(pas.Call[0].Interrupted==2 && smcInputs[1].X[0]==1 && smcInputs[1].X[6]==0x1234);
        Reset(); allowZ1Writes=true; smcBusy=2;
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==0);
        assert(pas.Call[0].Busy==2 && pas.Call[0].Calls==3 && smcInputs[1].X[0]==0x42000207 && smcInputs[1].X[6]==0);
        Reset(); allowZ1Writes=true; smcBusy=21;
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_DEVICE_BUSY);
        assert(pas.Call[0].Busy==20 && smcInputs.size()==21 && allocations.empty());
        assert(pas.SupportX1Ignored==0 && pas.InitSucceeded==0);
        Reset(); allowZ1Writes=true; smcInterrupts=1024;
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_IO_TIMEOUT);
        assert(pas.Call[0].Interrupted==1024 && pas.Consumed==1 && smcInputs.size()==1024 && allocations.empty());
        assert(pas.SupportX1Ignored==0 && pas.InitSucceeded==0);
        Reset(); allowZ1Writes=true; smcWaitCmd=7;
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_IO_DEVICE_ERROR);
        assert(pas.Call[0].Result[0]==2 && pas.Call[0].Completed==0 && pas.SupportX1Ignored==0 && smcInputs.size()==1);
        Reset(); allowZ1Writes=true; smcSupportedResult=0; smcFailCmd=5;
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_IO_DEVICE_ERROR);
        assert(pas.SupportX1Ignored==1 && pas.InitSucceeded==1 && pas.Consumed==1 && allocations.empty());
        const SIZE_T authFailureCalls=smcInputs.size();
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_DEVICE_BUSY);
        assert(smcInputs.size()==authFailureCalls);
        Reset(); allowZ1Writes=true; smcResultFailureCmd=1;
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_IO_DEVICE_ERROR);
        assert(pas.Call[1].Result[0]==0 && pas.Call[1].Result[1]==2 && pas.InitSucceeded==0 && allocations.empty());
        Reset(); allowZ1Writes=true; smcWaitCmd=1;
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_IO_DEVICE_ERROR);
        assert(pas.MetadataKept==1 && ZapRetainedMetadata && ZapOwner && allocations.size()==1);
        Reset(); allowZ1Writes=true; failGmuStep=0x1403;
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_IO_DEVICE_ERROR);
        assert(pas.InitSucceeded==1 && !z1Authorized && z1Reserved[0]==0xCCCCCCCC && allocations.empty());
        Reset(); allowZ1Writes=true; failGmuStep=0x1426; // INIT returned success; its post-call persistence failed.
        assert(EpiZapLoad(nullptr,nullptr,zap.data(),static_cast<ULONG>(zap.size()),&pas,checkpoint,nullptr)==STATUS_IO_DEVICE_ERROR);
        assert(pas.InitSucceeded==1 && pas.Consumed==1 && ZapOwner && smcInputs.size()==2);
        Reset();
    }
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
        GMU_G1_TRACE trace; GmuG1TraceInit(&trace);
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
        GMU_GX_INIT_TRACE init; GmuGxInitTraceInit(&init);
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
        GmuGxInitTraceInit(&init); failGmuStep=0x811;
        assert(GmuGxSecvid(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
        assert(init.Reason==6 && init.Count==1 && secvidWrites.size()==1 && secvidPage[0]==0x10);
        // A failed original-read breadcrumb prevents every write.
        secvidWrites.clear(); GmuGxInitTraceInit(&init); failGmuStep=0x802;
        assert(GmuGxSecvid(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
        assert(init.Reason==6 && init.Count==0 && init.Attempted==0 && secvidWrites.empty());
        // Readback mismatch at BASE low stops before BASE high and SIZE.
        failGmuStep=0xFFFFFFFF; failSecvidOffset=0;
        GmuGxInitTraceInit(&init);
        assert(GmuGxSecvid(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
        assert(init.Reason==7 && init.Count==2 && secvidWrites.size()==2);
        assert(init.After[1]==0x10 && init.After[2]==0xFFFFFFFF && secvidPage[1]==0x11);
        failSecvidOffset=0xFFFFFFFF;
        // Stage B follows A, saves all originals, and writes exactly the documented order.
        GmuGxInitTraceInit(&init);
        assert(GmuGxSecvid(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==0);
        for(ULONG i=0;i<12;++i) *GmuAddrReg(i)=0x100+i;
        assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==0);
        assert(init.Reason==0 && init.Count==16 && addrWrites.size()==12);
        for(ULONG i=0;i<12;++i)
            assert(addrWrites[i]==addrDwords[i] && init.Before[4+i]==0x100+i && init.After[4+i]==1);
        // No stage B accesses after a failed/incomplete stage A.
        addrWrites.clear(); GmuGxInitTraceInit(&init);
        assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_INVALID_DEVICE_STATE);
        assert(init.Reason==8 && addrWrites.empty());
        // Fail each stage B write breadcrumb: only earlier writes may complete.
        for(ULONG fail=0;fail<12;++fail) {
            GmuGxInitTraceInit(&init); init.Reason=0; init.Count=4; init.Attempted=1;
            addrWrites.clear(); for(ULONG i=0;i<12;++i) *GmuAddrReg(i)=0x100+i;
            failGmuStep=0x830+fail;
            assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
            assert(init.Reason==6 && init.Count==4+fail && addrWrites.size()==fail);
            for(ULONG i=0;i<12;++i) assert(init.Before[4+i]==0x100+i);
            for(ULONG i=fail;i<12;++i) assert(init.After[4+i]==0xFFFFFFFF && *GmuAddrReg(i)==0x100+i);
        }
        // A failed original read or map must prevent every stage B write.
        GmuGxInitTraceInit(&init); init.Reason=0; init.Count=4;
        addrWrites.clear(); failGmuStep=0x825;
        assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_IO_DEVICE_ERROR);
        assert(init.Reason==6 && init.Count==4 && addrWrites.empty());
        GmuGxInitTraceInit(&init); init.Reason=0; init.Count=4;
        failGmuStep=0xFFFFFFFF; failAddrMap=3;
        assert(GmuGxAddrMode(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&init)==STATUS_INSUFFICIENT_RESOURCES);
        assert(init.Reason==5 && init.Count==4 && addrWrites.empty()); failAddrMap=0xFFFFFFFF;
        // A readback mismatch includes that write in Count and stops the remainder.
        for(ULONG fail=0;fail<12;++fail) {
            GmuGxInitTraceInit(&init); init.Reason=0; init.Count=4; init.Attempted=1;
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
        seedHwcg(); GMU_GX_HWCG_TRACE hw; GmuGxHwcgTraceInit(&hw);
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
        seedHwcg(); GmuGxHwcgTraceInit(&hw); gPages[8][0xAE]=0x8AA8AA82;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
        assert(hw.Skipped==1 && hw.Count==0 && hw.Attempted==0 && hwcgWrites.empty());
        // Every table breadcrumb failure must still re-enable SP, and must skip CLOCK_CNTL.
        for(ULONG fail=0;fail<49;++fail) {
            seedHwcg(); GmuGxHwcgTraceInit(&hw); failGmuStep=0x890+fail;
            assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
            assert(hw.Reason==6 && hw.Count==3+fail && hw.SptpracAfterSet==0xA5 && gPages[8][0xAE]==0);
            for(ULONG i=fail;i<49;++i) assert(hw.After[i]==0xFFFFFFFF);
        }
        // v55: an ignored write at any table position is recorded, but all later writes complete.
        for(ULONG fail=0;fail<49;++fail) {
            seedHwcg(); GmuGxHwcgTraceInit(&hw); failHwcgDword=GmuHwcgTable[fail][0];
            assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
            assert(hw.Reason==0 && hw.Count==53 && hw.After[fail]==0xA000+fail && hw.SptpracAfterSet==0xA5);
            for(ULONG i=0;i<49;++i) assert(hw.After[i]==(i==fail ? 0xA000+i : GmuHwcgTable[i][1]));
            assert(gPages[8][0xAE]==0x8AA8AA82 && hwcgWrites.size()==55);
        }
        // Even a failed re-enable breadcrumb must not suppress the actual recovery write.
        seedHwcg(); GmuGxHwcgTraceInit(&hw); failGmuStep=0x8C1;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Reason==6 && hw.SptpracAfterSet==0xA5 && gPages[8][0xAE]==0);
        seedHwcg(); GmuGxHwcgTraceInit(&hw); failHwcgSpSet=true;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
        assert(hw.Reason==0 && hw.Count==53 && hw.SptpracAfterSet==0xA4 && gPages[8][0xAE]==0x8AA8AA82);
        // Both SP writes ignored: diagnostic values differ, but the table and CLOCK_CNTL still complete.
        seedHwcg(); GmuGxHwcgTraceInit(&hw); failHwcgDword=0x1A880;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==0);
        assert(hw.Reason==0 && hw.Count==53 && hw.SptpracAfterClear==0xA5 && hw.SptpracAfterSet==0xA5);
        // A later SP readback mismatch cannot replace an earlier breadcrumb failure.
        seedHwcg(); GmuGxHwcgTraceInit(&hw); failGmuStep=0x895; failHwcgSpSet=true;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Reason==6 && hw.Count==8 && hw.SptpracAfterSet==0xA4 && gPages[8][0xAE]==0);
        // CGC and final CLOCK_CNTL comparisons remain strict.
        seedHwcg(); GmuGxHwcgTraceInit(&hw); failHwcgDword=0x23B09;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Reason==7 && hw.Count==1 && hw.SptpracAfterClear==0xFFFFFFFF);
        seedHwcg(); GmuGxHwcgTraceInit(&hw); failHwcgDword=0xAE;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Reason==7 && hw.Count==53 && hw.ClockCntlAfter==0);
        seedHwcg(); GmuGxHwcgTraceInit(&hw); failGmuStep=0x845;
        assert(GmuGxHwcg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&hw)==STATUS_IO_DEVICE_ERROR);
        assert(hw.Count==0 && hw.Attempted==0 && hwcgWrites.empty());
        seedHwcg(); GmuGxHwcgTraceInit(&hw);
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
        seedCfg(); GMU_GX_CFG_TRACE cfg; GmuGxCfgTraceInit(&cfg);
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
        seedCfg(); GmuGxCfgTraceInit(&cfg); *GmuCfgReg(0x1F840)=5;
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==0);
        *GmuCfgReg(0x1F840)=0;
        assert(GmuGxCfgRestore(&probe,reinterpret_cast<HANDLE>(1),&cfg,&oob)==0);
        assert(cfg.CxBefore[2]==5 && cfg.CxRestored[2]==5 && *GmuCfgReg(0x1F840)==5);
        for(ULONG fail=0;fail<30;++fail) {
            seedCfg(); GmuGxCfgTraceInit(&cfg); failCfgDword=cfgDwords[fail];
            assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==0);
            assert(cfg.Reason==0 && cfg.Count==30 && cfgWrites.size()==30);
        }
        // Every write breadcrumb failure stops later writes, leaving unread After fields untouched.
        for(ULONG fail=0;fail<30;++fail) {
            seedCfg(); GmuGxCfgTraceInit(&cfg); failGmuStep=0x940+fail;
            assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==STATUS_IO_DEVICE_ERROR);
            assert(cfg.Reason==6 && cfg.Count==fail && cfgWrites.size()==fail);
            for(ULONG i=fail;i<30;++i) assert(cfg.Writes[i].After==0xFFFFFFFF);
            assert(cfg.CxWritten==static_cast<ULONG>((fail>24 ? 1 : 0)|(fail>25 ? 2 : 0)|(fail>26 ? 4 : 0)));
        }
        seedCfg(); GmuGxCfgTraceInit(&cfg); failGmuStep=0x919;
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==STATUS_IO_DEVICE_ERROR);
        assert(cfg.Count==0 && cfg.CxWritten==0 && cfgWrites.empty());
        seedCfg(); GmuGxCfgTraceInit(&cfg); failAddrMap=0;
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==STATUS_INSUFFICIENT_RESOURCES);
        assert(cfg.Reason==5 && cfgWrites.empty());
        seedCfg(); GmuGxCfgTraceInit(&cfg); gPages[1][0xD0]=0xC0;
        assert(GmuGxCfg(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cfg)==STATUS_INVALID_DEVICE_STATE);
        assert(cfg.Reason==GX_REG_HM_OFF && cfg.Count==0 && cfgWrites.empty()); gPages[1][0xD0]=0;
        seedCfg(); GmuGxCfgTraceInit(&cfg);
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
        seedStageE(); GMU_GX_STAGE_E_TRACE stageE; GmuGxStageETraceInit(&stageE);
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
            seedStageE(); GmuGxStageETraceInit(&stageE); failStageEDword=GmuStageEReg(fail);
            assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==0);
            assert(stageE.Reason==0 && stageE.Count==47 && stageEWrites.size()==47);
            assert(stageE.Writes[fail].After==0x200+fail);
        }
        for(ULONG fail=0;fail<47;++fail) {
            seedStageE(); GmuGxStageETraceInit(&stageE); failGmuStep=0xA40+fail;
            assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==STATUS_IO_DEVICE_ERROR);
            assert(stageE.Reason==6 && stageE.Count==fail && stageEWrites.size()==fail);
            for(ULONG i=0;i<47;++i) assert(stageE.Writes[i].Before==0x200+i);
            for(ULONG i=fail;i<47;++i) assert(stageE.Writes[i].After==0xFFFFFFFF);
        }
        seedStageE(); GmuGxStageETraceInit(&stageE); failGmuStep=0xA10;
        assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==STATUS_IO_DEVICE_ERROR);
        assert(stageE.Count==0 && stageE.Attempted==0 && stageEWrites.empty());
        seedStageE(); GmuGxStageETraceInit(&stageE); failAddrMap=0;
        assert(GmuGxStageE(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageE)==STATUS_INSUFFICIENT_RESOURCES);
        assert(stageE.Reason==5 && stageEWrites.empty());
        seedStageE(); GmuGxStageETraceInit(&stageE); gPages[1][0xD0]=0xC0;
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
        seedStageF(); GMU_GX_STAGE_F_TRACE stageF; GmuGxStageFTraceInit(&stageF);
        assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==0);
        assert(stageF.Attempted==1 && stageF.Reason==0 && stageF.Before==7 && stageF.After==1 && stageF.Count==4);
        assert(stageFWrites.size()==1 && stageFWrites[0]==1);
        for(ULONG i=0;i<4;++i) assert(stageF.Check[i]==checkValues[i]);
        seedStageF(); GmuGxStageFTraceInit(&stageF); ignoreStageFWrite=true;
        for(ULONG i=0;i<4;++i) *GmuCfgReg(checkRegs[i])=0x100+i;
        assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==0);
        assert(stageF.Reason==0 && stageF.After==7 && stageF.Count==4);
        for(ULONG i=0;i<4;++i) assert(stageF.Check[i]==0x100+i);
        // Original/write breadcrumb failures must prevent CP_AHB_CNTL publication.
        for(ULONG step=0xB00;step<=0xB02;++step) {
            seedStageF(); GmuGxStageFTraceInit(&stageF); failGmuStep=step;
            assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==STATUS_IO_DEVICE_ERROR);
            assert(stageF.Reason==6 && stageF.Attempted==0 && stageF.Count==0 && stageFWrites.empty());
            assert(stageF.After==0xFFFFFFFF);
        }
        for(ULONG fail=0;fail<4;++fail) {
            seedStageF(); GmuGxStageFTraceInit(&stageF); failGmuStep=0xB10+fail;
            assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==STATUS_IO_DEVICE_ERROR);
            assert(stageF.Reason==6 && stageF.Attempted==1 && stageF.Count==fail && stageFWrites.size()==1);
            for(ULONG i=0;i<fail;++i) assert(stageF.Check[i]==checkValues[i]);
            for(ULONG i=fail;i<4;++i) assert(stageF.Check[i]==0xFFFFFFFF);
        }
        seedStageF(); GmuGxStageFTraceInit(&stageF); failAddrMap=0;
        assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==STATUS_INSUFFICIENT_RESOURCES);
        assert(stageF.Reason==5 && stageFWrites.empty());
        seedStageF(); GmuGxStageFTraceInit(&stageF); dropGxAtStep=0xB12;
        assert(GmuGxStageF(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&stageF)==STATUS_INVALID_DEVICE_STATE);
        assert(stageF.Reason==GX_REG_HM_OFF && stageF.Count==2 && stageF.Check[2]==0xFFFFFFFF && stageF.Check[3]==0xFFFFFFFF);
        dropGxAtStep=0xFFFFFFFF; gPages[1][0xD0]=0; allowStageFWrite=false;
        // G-1 register setup: seven writes only, lo/hi order, diagnostic-only readbacks.
        allowG1Writes=true; GMU_G1_TRACE g1; GmuG1TraceInit(&g1);
        g1Writes.clear(); failGmuStep=0xFFFFFFFF;
        for(ULONG i=0;i<7;++i) *GmuCfgReg(g1Dwords[i])=0x300+i;
        assert(GmuG1Registers(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g1)==0 && g1.Count==7 && g1Writes.size()==7);
        for(ULONG i=0;i<7;++i) {
            assert(g1.Before[i]==0x300+i && g1.After[i]==g1.Value[i]);
            assert(g1Writes[i].first==g1Dwords[i] && g1Writes[i].second==g1.Value[i]);
        }
        for(ULONG fail=0;fail<7;++fail) {
            GmuG1TraceInit(&g1); g1Writes.clear(); failG1Dword=g1Dwords[fail];
            for(ULONG i=0;i<7;++i) *GmuCfgReg(g1Dwords[i])=0x300+i;
            assert(GmuG1Registers(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g1)==0 && g1.Count==7);
            assert(g1.After[fail]==0x300+fail);
        }
        failG1Dword=0xFFFFFFFF;
        for(ULONG fail=0;fail<7;++fail) {
            GmuG1TraceInit(&g1); g1Writes.clear(); failGmuStep=0xC20+fail;
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
            seedG2(); GMU_G2_TRACE cp; GmuG2TraceInit(&cp);
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==0);
            assert(cp.Reason==0 && cp.Attempted==1 && cp.SqeWritten==1 && cp.WptrWrites==1 && cp.Polls==0);
            assert(cp.ShadowBefore==0 && cp.ShadowAfter==9 && cp.Rptr==9 && cp.WptrAfter==9 && cp.FenceStatus==0);
            assert(cp.RbbmStatus==1 && cp.CxBusy==0 && cp.IntStatus==0 && cp.CpHwFault==0 && cp.FaultStatus==0);
            assert(g2Writes.size()==2 && g2Writes[0].first==0x808 && g2Writes[0].second==1);
            assert(g2Writes[1].first==0x807 && g2Writes[1].second==9);
            assert(std::memcmp(cpRing.data(),expectedPacket,sizeof(expectedPacket))==0);
            assert(std::memcmp(cp.Packet,expectedPacket,sizeof(expectedPacket))==0);
            seedG2(); GmuG2TraceInit(&cp); g2FenceDrops=3;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==0 && cp.WptrWrites==4 && cp.ShadowAfter==9);
            seedG2(); GmuG2TraceInit(&cp); g2FenceForever=true;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_TIMEOUT);
            assert(cp.Reason==10 && cp.WptrWrites==201 && cp.FenceStatus==1 && cp.Polls==0xFFFFFFFF);
            for(ULONG predicate=0;predicate<3;++predicate) {
                seedG2(); GmuG2TraceInit(&cp);
                if(predicate==0) g2Completes=false;
                if(predicate==1) g2RbbmBusy=true;
                if(predicate==2) g2GmuBusy=true;
                *GmuCfgReg(0x821)=0x55;
                assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_TIMEOUT);
                assert(cp.Reason==9 && cp.Polls==10000 && cp.CpHwFault==0x55 && cp.FaultStatus==0 && cp.FarLow==0x4000);
            }
            seedG2(); GmuG2TraceInit(&cp); g2Hang=true;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_DEVICE_ERROR);
            assert(cp.Reason==11 && cp.Polls==0 && (cp.IntStatus&(1UL<<23)));
            const ULONG steps[]={0xD00,0xD01,0xD02,0xD03,0xD04,0xD05,0xD06,0xD07};
            for(ULONG step:steps) {
                seedG2(); GmuG2TraceInit(&cp); failGmuStep=step;
                assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_DEVICE_ERROR && cp.Reason==6);
                assert(cp.Attempted==(step<=0xD02 ? 0UL : 1UL) && cp.WptrWrites==(step<=0xD04 ? 0UL : 1UL));
            }
            seedG2(); GmuG2TraceInit(&cp); g2Completes=false; failGmuStep=0xD07;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_IO_TIMEOUT && cp.Reason==9);
            seedG2(); GmuG2TraceInit(&cp); dropGxAtStep=0xD06;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_INVALID_DEVICE_STATE);
            assert(cp.Reason==GX_REG_HM_OFF && cp.WptrAfter==0xFFFFFFFF && cp.CpHwFault==0xFFFFFFFF);
            seedG2(); GmuG2TraceInit(&cp); failAddrMap=0;
            assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_INSUFFICIENT_RESOURCES && g2Writes.empty());
            for(ULONG bad=0;bad<4;++bad) {
                seedG2(); GmuG2TraceInit(&cp);
                if(bad==0) cpShadow[0]=1;
                if(bad==1) cpRing[0]=1;
                if(bad==2) *GmuCfgReg(0x807)=1;
                if(bad==3) probe.Regions[2].Mapped=false;
                assert(GmuG2Start(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&cp)==STATUS_INVALID_DEVICE_STATE);
                assert(cp.Reason==12 && cp.Attempted==0 && g2Writes.empty());
            }
            // G-3 reuses the same allocations after a completed ME_INIT; no SQE restart and no new map requests.
            const auto seedG3=[&]() {
                seedG2(); allowG3Writes=true; g3Writes.clear(); g3Completes=true;
                g3Scratch=true; g3Mem=true; g3Reg=true; g3BadGuard1=false; g3BadGuard3=false;
                g3FenceForever=false; g3FenceDrops=0; g3ErrorBits=0; testFaultFsr=0x400;
                std::memcpy(cpRing.data(),expectedPacket,sizeof(expectedPacket));
                cpShadow[0]=9; *GmuCfgReg(0x806)=9; *GmuCfgReg(0x807)=9; *GmuCfgReg(0x808)=1;
                *GmuCfgReg(0x885)=0; *GmuCfgReg(0x823)=0x123; *GmuCfgReg(0x824)=0x456; *GmuCfgReg(0x825)=0x789;
            };
            const ULONG extraPacket[]={0x48088501,0xCAFE0001,0x703D8003,0x70017000,0,0xCAFE0002,
                                        0x703E8003,0x00040885,0x70017008,0,0x70268000};
            seedG3(); GMU_G3_TRACE g3; GmuG3TraceInit(&g3); const SIZE_T originalMaps=testMaps.size();
            assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==0);
            assert(g3.Cp.Reason==0 && g3.Cp.Attempted==1 && g3.Cp.SqeWritten==0 && g3.Prepared==1 && g3.Checks==0x1F);
            assert(g3.Cp.ShadowBefore==9 && g3.Cp.ShadowAfter==20 && g3.Cp.WptrBefore==9 && g3.Cp.WptrAfter==20);
            assert(g3.ScratchBefore==0 && g3.ScratchAfter==0xCAFE0001 && g3.Cp.WptrWrites==1);
            assert(g3.DataAfter[0]==0xCAFE0002 && g3.DataAfter[2]==0xCAFE0001);
            for(ULONG i=0;i<4;++i) assert(g3.DataBefore[i]==0xFFFFFFFF);
            assert(g3.DataAfter[1]==0xFFFFFFFF && g3.DataAfter[3]==0xFFFFFFFF && cpRing[20]==0 && cpRing[0x7010/4]==0);
            assert(g3.CpInterrupt==0x123 && g3.CpProtect==0x456 && g3.CpStatus1==0x789 && g3.Cp.Fsr==0x400);
            assert(std::memcmp(g3.Packet,extraPacket,sizeof(extraPacket))==0);
            assert(std::memcmp(cpRing.data(),expectedPacket,sizeof(expectedPacket))==0);
            assert(std::memcmp(cpRing.data()+9,extraPacket,sizeof(extraPacket))==0);
            assert(g2Writes.size()==1 && g2Writes[0].first==0x807 && g2Writes[0].second==20 && testMaps.size()==originalMaps);
            const ULONG expectedChecks[]={0x1A,0x1D,0x1B,0x17,0x0F};
            for(ULONG bad=0;bad<5;++bad) {
                seedG3(); GmuG3TraceInit(&g3);
                if(bad==0) g3Scratch=false;
                if(bad==1) g3Mem=false;
                if(bad==2) g3Reg=false;
                if(bad==3) g3BadGuard1=true;
                if(bad==4) g3BadGuard3=true;
                assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_IO_DEVICE_ERROR);
                assert(g3.Cp.Reason==13 && g3.Checks==expectedChecks[bad] && g3.Cp.ShadowAfter==20);
            }
            seedG3(); GmuG3TraceInit(&g3); g3FenceDrops=3;
            assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==0 && g3.Cp.WptrWrites==4 && g3Writes.size()==4);
            seedG3(); GmuG3TraceInit(&g3); g3FenceForever=true;
            assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_IO_TIMEOUT);
            assert(g3.Cp.Reason==10 && g3.Cp.WptrWrites==201 && g3.Cp.WptrAfter==9 && g3.Checks==0x18);
            for(ULONG predicate=0;predicate<3;++predicate) {
                seedG3(); GmuG3TraceInit(&g3);
                if(predicate==0) g3Completes=false;
                if(predicate==1) g2RbbmBusy=true;
                if(predicate==2) g2GmuBusy=true;
                assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_IO_TIMEOUT);
                assert(g3.Cp.Reason==9 && g3.Cp.Polls==10000);
            }
            const ULONG errorBits[]={1UL<<1,1UL<<9,1UL<<24};
            for(ULONG bit:errorBits) {
                seedG3(); GmuG3TraceInit(&g3); g3ErrorBits=bit;
                assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_IO_DEVICE_ERROR);
                assert(g3.Cp.Reason==14 && g3.Cp.IntStatus==bit && g3.Checks==0x1F);
            }
            seedG3(); GmuG3TraceInit(&g3); g2Hang=true;
            assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_IO_DEVICE_ERROR && g3.Cp.Reason==11);
            for(ULONG step=0xE00;step<=0xE05;++step) {
                seedG3(); GmuG3TraceInit(&g3); failGmuStep=step;
                assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_IO_DEVICE_ERROR && g3.Cp.Reason==6);
                assert(g3.Cp.Attempted==(step<=0xE03 ? 0UL : 1UL));
            }
            for(ULONG step=0xE10;step<=0xE18;++step) {
                seedG3(); GmuG3TraceInit(&g3); failGmuStep=step;
                assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_IO_DEVICE_ERROR && g3.Cp.Reason==6);
            }
            seedG3(); GmuG3TraceInit(&g3); g3Completes=false; failGmuStep=0xE10;
            assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_IO_TIMEOUT && g3.Cp.Reason==9);
            seedG3(); GmuG3TraceInit(&g3); dropGxAtStep=0xE10;
            assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_INVALID_DEVICE_STATE);
            assert(g3.Cp.Reason==GX_REG_HM_OFF && g3.ScratchAfter==0xFFFFFFFF && g3.Cp.CpHwFault==0xFFFFFFFF);
            seedG3(); GmuG3TraceInit(&g3); failAddrMap=0;
            assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_INSUFFICIENT_RESOURCES && g3Writes.empty());
            seedG3(); GmuG3TraceInit(&g3); cpShadow[0]=0;
            assert(GmuG3Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3)==STATUS_INVALID_DEVICE_STATE && g3Writes.empty());
            // G-3b only appends the event and arms data[4..5], preserving commands and G-3's data[0..3].
            const auto seedG3b=[&]() {
                seedG3(); allowG3bWrites=true; g3bWrites.clear(); g3bCompletes=true; g3bEvent=true;
                g3bBadGuard=false; g3bFenceForever=false; g3bFenceDrops=0; g3bErrorBits=0; g3bDamage=0;
                std::memcpy(cpRing.data()+9,extraPacket,sizeof(extraPacket));
                cpShadow[0]=20; *GmuCfgReg(0x806)=20; *GmuCfgReg(0x807)=20; *GmuCfgReg(0x885)=0xCAFE0001;
                cpRing[0x7000/4]=0xCAFE0002; cpRing[0x7004/4]=0xFFFFFFFF;
                cpRing[0x7008/4]=0xCAFE0001; cpRing[0x700C/4]=0xFFFFFFFF;
            };
            const ULONG eventPacket[]={0x70460004,4,0x70017010,0,0xCAFE0003};
            seedG3b(); GMU_G3B_TRACE g3b; GmuG3bTraceInit(&g3b);
            assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==0);
            assert(g3b.Cp.Reason==0 && g3b.Prepared==1 && g3b.Checks==0x3F && g3b.Cp.SqeWritten==0);
            assert(g3b.Cp.ShadowBefore==20 && g3b.Cp.ShadowAfter==25 && g3b.Cp.WptrBefore==20 && g3b.Cp.WptrAfter==25);
            assert(g3b.Cp.WptrWrites==1 && g2Writes.size()==1 && g2Writes[0].first==0x807 && g2Writes[0].second==25);
            assert(g3bWrites.size()==1 && g3Writes.empty() && testMaps.size()==originalMaps);
            assert(std::memcmp(cpRing.data(),expectedPacket,sizeof(expectedPacket))==0);
            assert(std::memcmp(cpRing.data()+9,extraPacket,sizeof(extraPacket))==0);
            assert(std::memcmp(cpRing.data()+20,eventPacket,sizeof(eventPacket))==0);
            assert(std::memcmp(g3b.Packet,eventPacket,sizeof(eventPacket))==0 && (g3b.Packet[1]&(1UL<<31))==0);
            for(ULONG i=0;i<4;++i) assert(g3b.DataBefore[i]==g3b.DataAfter[i]);
            assert(g3b.DataBefore[4]==0xFFFFFFFF && g3b.DataBefore[5]==0xFFFFFFFF);
            assert(g3b.DataAfter[4]==0xCAFE0003 && g3b.DataAfter[5]==0xFFFFFFFF && cpRing[25]==0 && cpRing[0x7018/4]==0);
            assert(g3b.CpInterrupt==0x123 && g3b.CpProtect==0x456 && g3b.CpStatus1==0x789 && g3b.Cp.Fsr==0x400);
            for(ULONG bad=0;bad<6;++bad) {
                seedG3b(); GmuG3bTraceInit(&g3b);
                if(bad<4) g3bDamage=1UL<<bad;
                if(bad==4) g3bEvent=false;
                if(bad==5) g3bBadGuard=true;
                assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_IO_DEVICE_ERROR);
                assert(g3b.Cp.Reason==13 && g3b.Checks==(0x3FUL&~(1UL<<bad)) && g3b.Cp.ShadowAfter==25);
            }
            seedG3b(); GmuG3bTraceInit(&g3b); g3bFenceDrops=3;
            assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==0 && g3b.Cp.WptrWrites==4 && g3bWrites.size()==4);
            seedG3b(); GmuG3bTraceInit(&g3b); g3bFenceForever=true;
            assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_IO_TIMEOUT);
            assert(g3b.Cp.Reason==10 && g3b.Cp.WptrWrites==201 && g3b.Cp.WptrAfter==20 && g3b.Checks==0x2F);
            for(ULONG predicate=0;predicate<3;++predicate) {
                seedG3b(); GmuG3bTraceInit(&g3b);
                if(predicate==0) g3bCompletes=false;
                if(predicate==1) g2RbbmBusy=true;
                if(predicate==2) g2GmuBusy=true;
                assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_IO_TIMEOUT);
                assert(g3b.Cp.Reason==9 && g3b.Cp.Polls==10000);
            }
            for(ULONG bit:errorBits) {
                seedG3b(); GmuG3bTraceInit(&g3b); g3bErrorBits=bit;
                assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_IO_DEVICE_ERROR);
                assert(g3b.Cp.Reason==14 && g3b.Cp.IntStatus==bit);
            }
            seedG3b(); GmuG3bTraceInit(&g3b); g2Hang=true;
            assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_IO_DEVICE_ERROR && g3b.Cp.Reason==11);
            for(ULONG step=0xF00;step<=0xF05;++step) {
                seedG3b(); GmuG3bTraceInit(&g3b); failGmuStep=step;
                assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_IO_DEVICE_ERROR && g3b.Cp.Reason==6);
                assert(g3b.Cp.Attempted==(step<=0xF03 ? 0UL : 1UL));
            }
            for(ULONG step=0xF10;step<=0xF18;++step) {
                seedG3b(); GmuG3bTraceInit(&g3b); failGmuStep=step;
                assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_IO_DEVICE_ERROR && g3b.Cp.Reason==6);
            }
            seedG3b(); GmuG3bTraceInit(&g3b); g3bCompletes=false; failGmuStep=0xF10;
            assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_IO_TIMEOUT && g3b.Cp.Reason==9);
            seedG3b(); GmuG3bTraceInit(&g3b); dropGxAtStep=0xF10;
            assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_INVALID_DEVICE_STATE);
            assert(g3b.Cp.Reason==GX_REG_HM_OFF && g3b.ScratchAfter==0xFFFFFFFF && g3b.Cp.CpHwFault==0xFFFFFFFF);
            seedG3b(); GmuG3bTraceInit(&g3b); failAddrMap=0;
            assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_INSUFFICIENT_RESOURCES && g3bWrites.empty());
            for(ULONG bad=0;bad<5;++bad) {
                seedG3b(); GmuG3bTraceInit(&g3b);
                if(bad==0) cpShadow[0]=9;
                if(bad==1) *GmuCfgReg(0x807)=9;
                if(bad==2) cpRing[0x7000/4]=0;
                if(bad==3) cpRing[9]=0;
                if(bad==4) probe.Regions[1].Mapped=false;
                assert(GmuG3bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3b)==STATUS_INVALID_DEVICE_STATE && g3bWrites.empty());
            }
            // G-3c publishes an IRQ event with MASK held at zero. The shim rejects any MASK write.
            const auto seedG3c=[&]() {
                seedG3b(); allowG3cWrites=true; g3cWrites.clear(); g3cClearWrites.clear();
                g3cCompletes=true; g3cEvent=true; g3cBadGuard=false; g3cFenceForever=false;
                g3cShowStatus=true; g3cShowUnmasked=true; g3cIgnoreClear=false; g3cPending=false; g3cPolling=false;
                g3cFenceDrops=0; g3cErrorBits=0; g3cDamage=0; g3cStatusDelay=0; g3cUnmaskedDelay=0;
                g3cSample=0; g3cMaskChangeStep=0xFFFFFFFF; *GmuCfgReg(0x38)=0; gPages[2][0x315]=0;
                std::memcpy(cpRing.data()+20,eventPacket,sizeof(eventPacket));
                cpShadow[0]=25; *GmuCfgReg(0x806)=25; *GmuCfgReg(0x807)=25;
                cpRing[0x7010/4]=0xCAFE0003; cpRing[0x7014/4]=0xFFFFFFFF;
            };
            const ULONG irqPacket[]={0x70460004,0x80000004,0x70017018,0,0xCAFE0004};
            seedG3c(); GMU_G3C_TRACE g3c; GmuG3cTraceInit(&g3c);
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==0);
            assert(g3c.Cp.Reason==0 && g3c.Prepared==1 && g3c.Checks==0xFF && g3c.Cp.SqeWritten==0);
            assert(g3c.Cp.ShadowBefore==25 && g3c.Cp.ShadowAfter==30 && g3c.Cp.WptrAfter==30);
            assert(g3c.Cp.WptrWrites==1 && g2Writes.size()==1 && g2Writes[0].second==30);
            assert(g3c.MaskBefore==0 && g3c.MaskAtSubmit==0 && g3c.MaskAfter==0);
            assert(g3c.StatusFirst==0 && g3c.UnmaskedFirst==0 && g3c.IrqPolls==0);
            assert(g3c.ClearAttempted==1 && g3cClearWrites.size()==1 && g3cClearWrites[0]==(1UL<<20));
            assert(g3c.StatusAfterClear==0 && g3c.UnmaskedAfterClear==0);
            assert(std::memcmp(cpRing.data(),expectedPacket,sizeof(expectedPacket))==0);
            assert(std::memcmp(cpRing.data()+9,extraPacket,sizeof(extraPacket))==0);
            assert(std::memcmp(cpRing.data()+20,eventPacket,sizeof(eventPacket))==0);
            assert(std::memcmp(cpRing.data()+25,irqPacket,sizeof(irqPacket))==0);
            assert(std::memcmp(g3c.Packet,irqPacket,sizeof(irqPacket))==0 && cpRing[30]==0);
            for(ULONG i=0;i<6;++i) assert(g3c.DataBefore[i]==g3c.DataAfter[i]);
            assert(g3c.DataBefore[6]==0xFFFFFFFF && g3c.DataAfter[6]==0xCAFE0004);
            assert(g3c.DataAfter[7]==0xFFFFFFFF && cpRing[0x7020/4]==0 && testMaps.size()==originalMaps);
            assert(g3c.CpInterrupt==0x123 && g3c.CpProtect==0x456 && g3c.CpStatus1==0x789 && g3c.Cp.Fsr==0x400);
            seedG3c(); GmuG3cTraceInit(&g3c); *GmuCfgReg(0x38)=1;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_INVALID_DEVICE_STATE);
            assert(g3c.Cp.Reason==15 && g3c.Cp.Attempted==0 && g3c.Prepared==0 && g3c.ClearAttempted==0);
            assert(g3cWrites.empty() && g3cClearWrites.empty() && cpRing[0x7018/4]==0 && cpRing[25]==0);
            // Recheck MASK immediately before submission, and before every fenced retry.
            for(ULONG step=0x1006;step<=0x1007;++step) {
                seedG3c(); GmuG3cTraceInit(&g3c); g3cMaskChangeStep=step;
                if(step==0x1007) g3cFenceDrops=1;
                assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_INVALID_DEVICE_STATE);
                assert(g3c.Cp.Reason==15 && g3c.Cp.WptrWrites==(step==0x1006 ? 0UL : 1UL));
                assert(g3c.ClearAttempted==(step==0x1006 ? 0UL : 1UL));
            }
            // Both, either, or neither interrupt source may appear; data alone determines success.
            for(ULONG visible=0;visible<4;++visible) {
                seedG3c(); GmuG3cTraceInit(&g3c); g3cShowStatus=(visible&1)!=0; g3cShowUnmasked=(visible&2)!=0;
                assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==0 && g3c.Checks==0xFF);
                assert(g3c.StatusFirst==(g3cShowStatus ? 0UL : 0xFFFFFFFF));
                assert(g3c.UnmaskedFirst==(g3cShowUnmasked ? 0UL : 0xFFFFFFFF));
                assert(g3c.IrqPolls==(visible==3 ? 0UL : 100UL));
            }
            seedG3c(); GmuG3cTraceInit(&g3c); g3cStatusDelay=7; g3cUnmaskedDelay=3;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==0);
            assert(g3c.StatusFirst==7 && g3c.UnmaskedFirst==3 && g3c.IrqPolls==7);
            seedG3c(); GmuG3cTraceInit(&g3c); g3cIgnoreClear=true;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==0);
            assert((g3c.StatusAfterClear&(1UL<<20)) && (g3c.UnmaskedAfterClear&(1UL<<20)));
            seedG3c(); GmuG3cTraceInit(&g3c); *GmuCfgReg(0x201)=0x10; gPages[2][0x315]=0x80;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==0);
            assert(g3c.StatusAfterClear==0x10 && g3c.UnmaskedAfterClear==0x80);
            for(ULONG bad=0;bad<8;++bad) {
                seedG3c(); GmuG3cTraceInit(&g3c);
                if(bad<6) g3cDamage=1UL<<bad;
                if(bad==6) g3cEvent=false;
                if(bad==7) g3cBadGuard=true;
                assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_IO_DEVICE_ERROR);
                assert(g3c.Cp.Reason==13 && g3c.Checks==(0xFFUL&~(1UL<<bad)) && g3c.ClearAttempted==1);
            }
            seedG3c(); GmuG3cTraceInit(&g3c); g3cFenceDrops=3;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==0 && g3c.Cp.WptrWrites==4);
            seedG3c(); GmuG3cTraceInit(&g3c); g3cFenceForever=true;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_IO_TIMEOUT);
            assert(g3c.Cp.Reason==10 && g3c.Cp.WptrWrites==201 && g3c.ClearAttempted==1);
            for(ULONG predicate=0;predicate<3;++predicate) {
                seedG3c(); GmuG3cTraceInit(&g3c);
                if(predicate==0) g3cCompletes=false;
                if(predicate==1) g2RbbmBusy=true;
                if(predicate==2) g2GmuBusy=true;
                assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_IO_TIMEOUT);
                assert(g3c.Cp.Reason==9 && g3c.Cp.Polls==10000 && g3c.ClearAttempted==1);
            }
            for(ULONG bit:errorBits) {
                seedG3c(); GmuG3cTraceInit(&g3c); g3cErrorBits=bit;
                assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_IO_DEVICE_ERROR);
                assert(g3c.Cp.Reason==14 && (g3c.Cp.IntStatus&bit) && g3c.ClearAttempted==1);
            }
            seedG3c(); GmuG3cTraceInit(&g3c); g2Hang=true;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_IO_DEVICE_ERROR && g3c.Cp.Reason==11);
            for(ULONG step=0x1000;step<=0x100D;++step) {
                seedG3c(); GmuG3cTraceInit(&g3c); failGmuStep=step;
                assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_IO_DEVICE_ERROR && g3c.Cp.Reason==6);
                assert(g3c.Cp.Attempted==(step<=0x1006 ? 0UL : 1UL));
                assert(g3c.ClearAttempted==(step<=0x1006 ? 0UL : 1UL));
            }
            for(ULONG step=0x1010;step<=0x1018;++step) {
                seedG3c(); GmuG3cTraceInit(&g3c); failGmuStep=step;
                assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_IO_DEVICE_ERROR && g3c.Cp.Reason==6);
            }
            seedG3c(); GmuG3cTraceInit(&g3c); g3cCompletes=false; failGmuStep=0x100A;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_IO_TIMEOUT);
            assert(g3c.Cp.Reason==9 && g3c.ClearAttempted==1); // first failure survives cleanup failure
            seedG3c(); GmuG3cTraceInit(&g3c); dropGxAtStep=0x100A;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_INVALID_DEVICE_STATE);
            assert(g3c.Cp.Reason==GX_REG_HM_OFF && g3c.ClearAttempted==0 && g3c.StatusAfterClear==0xFFFFFFFF);
            seedG3c(); GmuG3cTraceInit(&g3c); failAddrMap=0;
            assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_INSUFFICIENT_RESOURCES && g3cWrites.empty());
            for(ULONG bad=0;bad<5;++bad) {
                seedG3c(); GmuG3cTraceInit(&g3c);
                if(bad==0) cpShadow[0]=20;
                if(bad==1) *GmuCfgReg(0x807)=20;
                if(bad==2) cpRing[0x7010/4]=0;
                if(bad==3) cpRing[20]=0;
                if(bad==4) probe.Regions[1].Mapped=false;
                assert(GmuG3cSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g3c)==STATUS_INVALID_DEVICE_STATE && g3cWrites.empty());
            }
            const auto seedG4=[&]() {
                seedG3c(); allowG4Writes=true; g4Writes.clear(); g4Completes=true;
                g4Scratch=true; g4Mem=true; g4Event=true; g4FenceForever=false; g4Scratch2Damage=false;
                g4FenceDrops=0; g4ErrorBits=0; g4Damage=0; g4BadGuards=0;
                std::memcpy(cpRing.data()+25,irqPacket,sizeof(irqPacket));
                cpShadow[0]=30; *GmuCfgReg(0x806)=30; *GmuCfgReg(0x807)=30;
                cpRing[0x7018/4]=0xCAFE0004; cpRing[0x701C/4]=0xFFFFFFFF;
                *GmuCfgReg(0x886)=0x12345678;
                *GmuCfgReg(0x928)=0; *GmuCfgReg(0x929)=0; *GmuCfgReg(0x92A)=0;
            };
            const ULONG ibPacket[]={0x48088601,0xCAFE0005,0x703D8003,0x70017020,0,0xCAFE0006};
            const ULONG callPacket[]={0x70BF8003,0x70016000,0,6,0x70460004,4,0x70017024,0,0xCAFE0007};
            seedG4(); GMU_G4_TRACE g4; GmuG4TraceInit(&g4);
            assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==0);
            assert(g4.Cp.Reason==0 && g4.Prepared==1 && g4.Checks==0x1F && g4.Cp.SqeWritten==0);
            assert(g4.Cp.ShadowBefore==30 && g4.Cp.ShadowAfter==39 && g4.Cp.WptrBefore==30 && g4.Cp.WptrAfter==39);
            assert(g4.Cp.WptrWrites==1 && g4Writes.size()==1 && g2Writes.size()==1 && g2Writes[0].second==39);
            assert(g4.ScratchBefore==0x12345678 && g4.ScratchAfter==0xCAFE0005);
            assert(g4.Scratch2Before==0xCAFE0001 && g4.Scratch2After==0xCAFE0001);
            assert(g4.IbBaseLow==0x70016000 && g4.IbBaseHigh==0 && g4.IbRemaining==0);
            assert(g4.CpInterrupt==0x123 && g4.CpProtect==0x456 && g4.CpStatus1==0x789 && g4.Cp.Fsr==0x400);
            assert(g4.MaskBefore==0 && g4.MaskAtSubmit==0 && g4.MaskAfter==0 && testMaps.size()==originalMaps);
            assert(std::memcmp(cpRing.data(),expectedPacket,sizeof(expectedPacket))==0);
            assert(std::memcmp(cpRing.data()+9,extraPacket,sizeof(extraPacket))==0);
            assert(std::memcmp(cpRing.data()+20,eventPacket,sizeof(eventPacket))==0);
            assert(std::memcmp(cpRing.data()+25,irqPacket,sizeof(irqPacket))==0);
            assert(std::memcmp(g4.Ib,ibPacket,sizeof(ibPacket))==0 && std::memcmp(cpRing.data()+0x6000/4,ibPacket,sizeof(ibPacket))==0);
            assert(std::memcmp(g4.Packet,callPacket,sizeof(callPacket))==0 && cpRing[39]==0 && cpRing[0x6018/4]==0);
            for(ULONG i=0;i<8;++i) assert(g4.DataBefore[i]==g4.DataAfter[i]);
            for(ULONG i=8;i<12;++i) assert(g4.DataBefore[i]==0xFFFFFFFF);
            assert(g4.DataAfter[8]==0xCAFE0006 && g4.DataAfter[9]==0xCAFE0007 && cpRing[0x7030/4]==0);
            // Each success bit has an independent failure mode; both guards and all old data are checked.
            for(ULONG bad=0;bad<14;++bad) {
                seedG4(); GmuG4TraceInit(&g4); ULONG lost=0;
                if(bad==0) {g4Scratch=false; lost=0;}
                if(bad==1) {g4Mem=false; lost=1;}
                if(bad==2) {g4Event=false; lost=2;}
                if(bad==3 || bad==4) {g4BadGuards=1UL<<(bad-3); lost=3;}
                if(bad>=5 && bad<13) {g4Damage=1UL<<(bad-5); lost=4;}
                if(bad==13) {g4Scratch2Damage=true; lost=4;}
                assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_IO_DEVICE_ERROR);
                assert(g4.Cp.Reason==13 && g4.Checks==(0x1FUL&~(1UL<<lost)));
            }
            seedG4(); GmuG4TraceInit(&g4); g4FenceDrops=3;
            assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==0 && g4.Cp.WptrWrites==4);
            seedG4(); GmuG4TraceInit(&g4); g4FenceForever=true;
            assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_IO_TIMEOUT);
            assert(g4.Cp.Reason==10 && g4.Cp.WptrWrites==201 && g4.Cp.ShadowAfter==30);
            for(ULONG predicate=0;predicate<3;++predicate) {
                seedG4(); GmuG4TraceInit(&g4);
                if(predicate==0) g4Completes=false;
                if(predicate==1) g2RbbmBusy=true;
                if(predicate==2) g2GmuBusy=true;
                assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_IO_TIMEOUT && g4.Cp.Reason==9);
                if(predicate==0) assert(g4.IbBaseLow==0x70016000 && g4.IbRemaining==6);
            }
            for(ULONG bit:errorBits) {
                seedG4(); GmuG4TraceInit(&g4); g4ErrorBits=bit;
                assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_IO_DEVICE_ERROR && g4.Cp.Reason==14);
            }
            seedG4(); GmuG4TraceInit(&g4); g2Hang=true;
            assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_IO_DEVICE_ERROR && g4.Cp.Reason==11);
            for(ULONG step=0x1100;step<=0x1105;++step) {
                seedG4(); GmuG4TraceInit(&g4); failGmuStep=step;
                assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_IO_DEVICE_ERROR && g4.Cp.Reason==6);
                assert(g4.Cp.Attempted==(step<=0x1103 ? 0UL : 1UL));
            }
            for(ULONG step=0x1110;step<=0x111D;++step) {
                seedG4(); GmuG4TraceInit(&g4); failGmuStep=step;
                assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_IO_DEVICE_ERROR && g4.Cp.Reason==6);
            }
            seedG4(); GmuG4TraceInit(&g4); g4Completes=false; failGmuStep=0x1110;
            assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_IO_TIMEOUT && g4.Cp.Reason==9);
            seedG4(); GmuG4TraceInit(&g4); dropGxAtStep=0x1110;
            assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_INVALID_DEVICE_STATE);
            assert(g4.Cp.Reason==GX_REG_HM_OFF && g4.ScratchAfter==0xFFFFFFFF && g4.IbRemaining==0xFFFFFFFF);
            seedG4(); GmuG4TraceInit(&g4); failAddrMap=0;
            assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_INSUFFICIENT_RESOURCES && g4Writes.empty());
            for(ULONG bad=0;bad<9;++bad) {
                seedG4(); GmuG4TraceInit(&g4);
                if(bad==0) cpShadow[0]=25;
                if(bad==1) *GmuCfgReg(0x807)=25;
                if(bad==2) cpRing[0x7018/4]=0;
                if(bad==3) cpRing[25]=0;
                if(bad==4) *GmuCfgReg(0x885)=0;
                if(bad==5) probe.Regions[1].Mapped=false;
                if(bad==6) *GmuCfgReg(0x38)=1;
                if(bad==7) g3cMaskChangeStep=0x1103;
                if(bad==8) {g3cMaskChangeStep=0x1104; g4FenceDrops=1;}
                assert(GmuG4Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g4)==STATUS_INVALID_DEVICE_STATE);
                assert(g4.Cp.Reason==(bad>=6 ? 15UL : 12UL) && g4.Cp.WptrWrites==(bad==8 ? 1UL : 0UL));
            }
            const auto seedG5=[&]() {
                seedG4(); allowG5Writes=true; g5Writes.clear(); g5Completes=true; g5Flush=true;
                g5Event=true; g5Fill=true; g5FenceForever=false; g5BadRptr=false;
                g5FenceDrops=0; g5ErrorBits=0; g5Color=0x78563412; g5BadPixel=0xFFFFFFFF; g5BadSentinel=0xFFFFFFFF;
                std::memcpy(cpRing.data()+0x6000/4,ibPacket,sizeof(ibPacket));
                std::memcpy(cpRing.data()+30,callPacket,sizeof(callPacket));
                cpShadow[0]=39; *GmuCfgReg(0x806)=39; *GmuCfgReg(0x807)=39;
                *GmuCfgReg(0x886)=0xCAFE0005; *GmuCfgReg(0x8E07)=0x12345678;
                cpRing[0x7020/4]=0xCAFE0006; cpRing[0x7024/4]=0xCAFE0007;
                cpRing[0x7028/4]=0xFFFFFFFF; cpRing[0x702C/4]=0xFFFFFFFF;
            };
            seedG5(); GMU_G5_TRACE g5; GmuG5TraceInit(&g5);
            assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==0);
            assert(g5.Cp.Reason==0 && g5.Prepared==1 && g5.Checks==0x1F && g5.Cp.SqeWritten==0);
            assert(g5.CcuBefore==0x12345678 && g5.CcuAfter==0x10000000);
            assert(g5.Cp.ShadowBefore==39 && g5.Cp.ShadowAfter==48 && g5.Cp.Rptr==48 && g5.Cp.WptrAfter==48);
            assert(g5.Cp.WptrWrites==1 && g2Writes.size()==1 && g5Writes.size()==1 && g2Writes[0].second==48);
            assert(g5.IbBaseLow==0x70016100 && g5.IbBaseHigh==0 && g5.IbRemaining==0);
            assert(g5.CpInterrupt==0x123 && g5.CpProtect==0x456 && g5.CpStatus1==0x789 && g5.Cp.Fsr==0x400);
            assert(g5.PixelMatches==256 && g5.PixelFirstBad==0xFFFFFFFF && g5.PixelBadValue==0xFFFFFFFF);
            assert(g5.SentinelMatches==16 && g5.DataAfter[10]==0xCAFE0008 && g5.DataAfter[11]==0xCAFE0009);
            for(ULONG i=0;i<4;++i) assert(g5.Pixels[i]==0x78563412);
            for(ULONG i=0;i<16;++i) assert(g5.Sentinel[i]==0xFFFFFFFF);
            for(ULONG i=0;i<10;++i) assert(g5.DataAfter[i]==g5.DataBefore[i]);
            assert(g5.MaskBefore==0 && g5.MaskAtSubmit==0 && g5.MaskAfter==0 && testMaps.size()==originalMaps);
            assert(std::memcmp(cpRing.data()+30,callPacket,sizeof(callPacket))==0);
            assert(std::memcmp(cpRing.data()+0x6000/4,ibPacket,sizeof(ibPacket))==0);
            assert(std::memcmp(g5.Ib,cpRing.data()+0x6100/4,sizeof(g5.Ib))==0 && cpRing[0x6190/4]==0);
            assert(std::memcmp(g5.Packet,cpRing.data()+39,sizeof(g5.Packet))==0 && cpRing[48]==0 && cpRing[0x7840/4]==0);
            // Each Checks bit can fail independently, including a hardware RPTR different from the shadow.
            for(ULONG bad=0;bad<5;++bad) {
                seedG5(); GmuG5TraceInit(&g5);
                if(bad==0) g5BadRptr=true;
                if(bad==1) g5Flush=false;
                if(bad==2) g5Event=false;
                if(bad==3) g5Fill=false;
                if(bad==4) g5BadSentinel=0;
                assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_DEVICE_ERROR);
                assert(g5.Cp.Reason==13 && g5.Checks==(0x1FUL&~(1UL<<bad)));
            }
            const ULONG pixelIndices[]={0,15,16,255};
            for(ULONG index:pixelIndices) {
                seedG5(); GmuG5TraceInit(&g5); g5BadPixel=index;
                assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_DEVICE_ERROR);
                assert(g5.PixelMatches==255 && g5.PixelFirstBad==index && g5.PixelBadValue==0);
            }
            seedG5(); GmuG5TraceInit(&g5); g5Color=0x12345678;
            assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_DEVICE_ERROR);
            assert(g5.PixelMatches==0 && g5.PixelFirstBad==0 && g5.PixelBadValue==0x12345678 && g5.Pixels[3]==0x12345678);
            for(ULONG index=0;index<16;++index) {
                seedG5(); GmuG5TraceInit(&g5); g5BadSentinel=index;
                assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_DEVICE_ERROR);
                assert(g5.SentinelMatches==15 && g5.Sentinel[index]==0 && g5.Checks==0xF);
            }
            seedG5(); GmuG5TraceInit(&g5); g5FenceDrops=3;
            assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==0 && g5.Cp.WptrWrites==4);
            seedG5(); GmuG5TraceInit(&g5); g5FenceForever=true;
            assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_TIMEOUT);
            assert(g5.Cp.Reason==10 && g5.Cp.WptrWrites==201 && g5.PixelMatches==0 && g5.SentinelMatches==16);
            for(ULONG predicate=0;predicate<3;++predicate) {
                seedG5(); GmuG5TraceInit(&g5);
                if(predicate==0) g5Completes=false;
                if(predicate==1) g2RbbmBusy=true;
                if(predicate==2) g2GmuBusy=true;
                assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_TIMEOUT && g5.Cp.Reason==9);
                if(predicate==0) assert(g5.IbBaseLow==0x70016100 && g5.IbRemaining==36);
            }
            for(ULONG bit:errorBits) {
                seedG5(); GmuG5TraceInit(&g5); g5ErrorBits=bit;
                assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_DEVICE_ERROR && g5.Cp.Reason==14);
            }
            seedG5(); GmuG5TraceInit(&g5); g2Hang=true;
            assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_DEVICE_ERROR && g5.Cp.Reason==11);
            for(ULONG step=0x1200;step<=0x1206;++step) {
                seedG5(); GmuG5TraceInit(&g5); failGmuStep=step;
                assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_DEVICE_ERROR && g5.Cp.Reason==6);
                assert(g5.Cp.Attempted==(step<=0x1204 ? 0UL : 1UL));
            }
            for(ULONG step=0x1210;step<=0x121C;++step) {
                seedG5(); GmuG5TraceInit(&g5); failGmuStep=step;
                assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_DEVICE_ERROR && g5.Cp.Reason==6);
            }
            seedG5(); GmuG5TraceInit(&g5); g5Completes=false; failGmuStep=0x1210;
            assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_IO_TIMEOUT && g5.Cp.Reason==9);
            seedG5(); GmuG5TraceInit(&g5); dropGxAtStep=0x1210;
            assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_INVALID_DEVICE_STATE);
            assert(g5.Cp.Reason==GX_REG_HM_OFF && g5.CcuAfter==0xFFFFFFFF && g5.IbRemaining==0xFFFFFFFF);
            for(ULONG mapIndex:{0UL,3UL}) {
                seedG5(); GmuG5TraceInit(&g5); failAddrMap=mapIndex;
                assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_INSUFFICIENT_RESOURCES && g5Writes.empty());
            }
            for(ULONG bad=0;bad<9;++bad) {
                seedG5(); GmuG5TraceInit(&g5);
                if(bad==0) cpShadow[0]=30;
                if(bad==1) *GmuCfgReg(0x807)=30;
                if(bad==2) cpRing[0x7024/4]=0;
                if(bad==3) cpRing[30]=0;
                if(bad==4) cpRing[0x6000/4]=0;
                if(bad==5) probe.Regions[1].Mapped=false;
                if(bad==6) *GmuCfgReg(0x38)=1;
                if(bad==7) g3cMaskChangeStep=0x1204;
                if(bad==8) {g3cMaskChangeStep=0x1205; g5FenceDrops=1;}
                assert(GmuG5Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5)==STATUS_INVALID_DEVICE_STATE);
                assert(g5.Cp.Reason==(bad>=6 ? 15UL : 12UL) && g5.Cp.WptrWrites==(bad==8 ? 1UL : 0UL));
            }
            const auto seedG5b=[&]() {
                seedG5(); allowG5bWrites=true; g5bWrites.clear(); g5bCompletes=true; g5bFill=true;
                g5bEvent=true; g5bFenceForever=false; g5bBadRptr=false; g5bFenceDrops=0; g5bErrorBits=0;
                g5bBadSentinel=0xFFFFFFFF; testGlobalCalls=0; testGlobalStatus=STATUS_SUCCESS;
                std::memcpy(cpRing.data()+0x6100/4,GmuG5Ib,sizeof(GmuG5Ib));
                std::memcpy(cpRing.data()+39,GmuG5Packet,sizeof(GmuG5Packet));
                cpShadow[0]=48; *GmuCfgReg(0x806)=48; *GmuCfgReg(0x807)=48;
                cpRing[0x7028/4]=0xCAFE0008; cpRing[0x702C/4]=0xCAFE0009;
                for(ULONG i=0;i<272;++i) cpRing[0x7400/4+i]=0xFFFFFFFF;
                *GmuCfgReg(0x8E04)=0x10; *GmuCfgReg(0x8E01)=0x20; *GmuCfgReg(0xE12)=0x30; *GmuCfgReg(0xE19)=0x81;
            };
            seedG5b(); GMU_G5B_TRACE g5b; GmuG5bTraceInit(&g5b);
            assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==0);
            assert(g5b.Draw.Checks==0xF && g5b.Draw.Cp.Reason==0 && g5b.Draw.Cp.ShadowAfter==61);
            assert(g5b.Draw.Cp.Rptr==61 && g5b.Draw.Cp.WptrAfter==61 && g5b.Data12After==0xCAFE000A);
            assert(g5b.Draw.PixelMatches==256 && g5b.Draw.SentinelMatches==16 && g5b.Draw.Cp.WptrWrites==1);
            assert(g5b.MagicBefore[0]==0x10 && g5b.MagicAfter[0]==0x04100000 && g5b.MagicAfter[1]==0);
            assert(g5b.MagicAfter[2]==1 && g5b.MagicAfter[3]==0x81 && testMaps.size()==originalMaps);
            assert(testGlobalCalls==2 && g5b.GlobalBefore[0]==0x100 && g5b.GlobalAfter[3]==0x203);
            assert(g5b.ScanDwords==7699 && g5b.ScanCount[0]==0 && g5b.ScanCount[1]==0 && g5b.ScanCount[2]==0);
            assert(cpRing[61]==0 && cpRing[0x6228/4]==0 && std::memcmp(g5b.Ib,GmuG5bIb,sizeof(g5b.Ib))==0);
            // Scan boundaries: exclusions must not hide the first/last included DWORD next to each range.
            const ULONG included[]={0xF4,0x5FFC,0x6240,0x6FFC,0x7040,0x73FC,0x7840,0x7FFC};
            for(ULONG off:included) {
                seedG5b(); GmuG5bTraceInit(&g5b); cpRing[off/4]=0x78563412;
                assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==0);
                assert(g5b.ScanCount[0]==1 && g5b.ScanFirst[0]==off);
            }
            seedG5b(); GmuG5bTraceInit(&g5b); cpRing[0x200/4]=0x12345678; cpRing[0x204/4]=0x12;
            assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==0);
            assert(g5b.ScanCount[1]==1 && g5b.ScanFirst[1]==0x200 && g5b.ScanCount[2]==1 && g5b.ScanFirst[2]==0x204);
            for(ULONG bad=0;bad<4;++bad) {
                seedG5b(); GmuG5bTraceInit(&g5b);
                if(bad==0) g5bBadRptr=true;
                if(bad==1) g5bEvent=false;
                if(bad==2) g5bFill=false;
                if(bad==3) g5bBadSentinel=15;
                assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==STATUS_IO_DEVICE_ERROR);
                assert(g5b.Draw.Checks==(0xFUL&~(1UL<<bad)) && g5b.Draw.Cp.Reason==13);
            }
            seedG5b(); GmuG5bTraceInit(&g5b); g5bFenceDrops=3;
            assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==0 && g5b.Draw.Cp.WptrWrites==4);
            seedG5b(); GmuG5bTraceInit(&g5b); g5bFenceForever=true;
            assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==STATUS_IO_TIMEOUT && g5b.Draw.Cp.Reason==10);
            seedG5b(); GmuG5bTraceInit(&g5b); g5bCompletes=false; failGmuStep=0x1320;
            assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==STATUS_IO_TIMEOUT && g5b.Draw.Cp.Reason==9);
            for(ULONG bit:errorBits) {
                seedG5b(); GmuG5bTraceInit(&g5b); g5bErrorBits=bit;
                assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==STATUS_IO_DEVICE_ERROR && g5b.Draw.Cp.Reason==14);
            }
            for(ULONG step=0x1300;step<=0x130B;++step) {
                seedG5b(); GmuG5bTraceInit(&g5b); failGmuStep=step;
                assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==STATUS_IO_DEVICE_ERROR && g5b.Draw.Cp.Reason==6);
            }
            seedG5b(); GmuG5bTraceInit(&g5b); testGlobalStatus=STATUS_INVALID_DEVICE_STATE;
            assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==0);
            assert(g5b.GlobalStatus[0]==testGlobalStatus && g5b.GlobalBefore[0]==0xFFFFFFFF);
            for(ULONG bad=0;bad<6;++bad) {
                seedG5b(); GmuG5bTraceInit(&g5b);
                if(bad==0) cpRing[0x7400/4]=0;
                if(bad==1) cpRing[0x7800/4]=0;
                if(bad==2) cpRing[0x7028/4]=0;
                if(bad==3) cpRing[39]=0;
                if(bad==4) cpRing[0x6100/4]=0;
                if(bad==5) cpShadow[0]=39;
                assert(GmuG5bSubmit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&g5b)==STATUS_INVALID_DEVICE_STATE);
                assert(g5b.Draw.Cp.Reason==12 && g5bWrites.empty());
            }
            const auto seedS1=[&]() {
                seedG5b(); allowS1Writes=true; z1Authorized=true; s1Writes.clear(); s1Completes=true; s1Store=true;
                s1Event=true; s1FenceForever=false; s1BadRptr=false; s1FenceDrops=0; s1ErrorBits=0;
                s1BadGuard=0xFFFFFFFF; s1Scatter=0xFFFFFFFF;
                std::memcpy(cpRing.data()+0x6200/4,GmuG5bIb,sizeof(GmuG5bIb));
                std::memcpy(cpRing.data()+48,GmuG5bPacket,sizeof(GmuG5bPacket));
                std::memcpy(cpRing.data()+61,GmuZ1Packet,sizeof(GmuZ1Packet));
                cpShadow[0]=73; *GmuCfgReg(0x806)=73; *GmuCfgReg(0x807)=73;
                cpRing[0x7030/4]=0xCAFE000A; cpRing[0x7034/4]=0xCAFE000B;
            };
            const auto s1Trace=[]() {
                GMU_S1_TRACE t; GmuS1TraceInit(&t); t.ZChecks=0x1F; t.ZReason=0; t.Checks=1; return t;
            };
            seedS1(); auto s1=s1Trace();
            assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==0);
            assert(s1.Checks==0x1F && s1.Prepared==1 && s1.Cp.Reason==0 && s1.Cp.SqeWritten==0);
            assert(s1.Cp.ShadowBefore==73 && s1.Cp.WptrBefore==73 && s1.Cp.ShadowAfter==82 && s1.Cp.Rptr==82 && s1.Cp.WptrAfter==82);
            assert(s1.Cp.WptrWrites==1 && s1Writes.size()==1 && g2Writes.size()==1 && testMaps.size()==originalMaps);
            assert(std::memcmp(s1.Ib,GmuS1Ib,sizeof(s1.Ib))==0 && std::memcmp(s1.Shader,GmuS1Shader,sizeof(s1.Shader))==0);
            assert(s1.OutputAfter[0]==0xC0DE5EED && s1.Data14Before==0xFFFFFFFF && s1.Data14After==0xCAFE000C);
            for(ULONG i=0;i<16;++i) assert(s1.OutputBefore[i]==0xFFFFFFFF);
            for(ULONG i=1;i<16;++i) assert(s1.OutputAfter[i]==0xFFFFFFFF);
            assert(s1.ScanDwords==7566 && s1.ScanCount==0 && s1.ScanFirst==0xFFFFFFFF);
            assert(s1.GlobalStatus[0]==0 && s1.GlobalStatus[1]==0 && s1.Cp.Fsr==0x400);
            assert(s1.IbBaseLow==0x70016300 && s1.IbRemaining==0 && cpRing[82]==0 && cpRing[0x63D4/4]==0 && cpRing[0x6880/4]==0);
            for(ULONG bad=0;bad<4;++bad) {
                seedS1(); s1=s1Trace();
                if(bad==0) s1BadRptr=true;
                if(bad==1) s1Event=false;
                if(bad==2) s1Store=false;
                if(bad==3) s1BadGuard=15;
                assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_IO_DEVICE_ERROR);
                assert(s1.Cp.Reason==13 && s1.Checks==(0x1FUL&~(2UL<<bad)));
            }
            const ULONG scanEdges[]={0x148,0x5FFC,0x6240,0x62FC,0x6400,0x67FC,0x6880,0x6FFC,
                                      0x7040,0x73FC,0x7840,0x7BFC,0x7C40,0x7FFC};
            for(ULONG off:scanEdges) {
                seedS1(); s1=s1Trace(); s1Scatter=off;
                assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==0);
                assert(s1.ScanCount==1 && s1.ScanFirst==off);
            }
            seedS1(); s1=s1Trace(); s1FenceDrops=3;
            assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==0 && s1.Cp.WptrWrites==4);
            seedS1(); s1=s1Trace(); s1FenceForever=true;
            assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_IO_TIMEOUT && s1.Cp.Reason==10 && s1.Cp.WptrWrites==201);
            for(ULONG bad=0;bad<3;++bad) {
                seedS1(); s1=s1Trace();
                if(bad==0) s1Completes=false;
                if(bad==1) g2RbbmBusy=true;
                if(bad==2) g2GmuBusy=true;
                assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_IO_TIMEOUT && s1.Cp.Reason==9);
                if(bad==0) assert(s1.IbRemaining==53 && s1.Cp.ShadowAfter==73);
            }
            for(ULONG bit:errorBits) {
                seedS1(); s1=s1Trace(); s1ErrorBits=bit;
                assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_IO_DEVICE_ERROR && s1.Cp.Reason==14);
            }
            seedS1(); s1=s1Trace(); g2Hang=true;
            assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_IO_DEVICE_ERROR && s1.Cp.Reason==11);
            for(ULONG step=0x1700;step<=0x1706;++step) {
                seedS1(); s1=s1Trace(); failGmuStep=step;
                assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_IO_DEVICE_ERROR && s1.Cp.Reason==6);
                assert(s1.Cp.Attempted==(step<=0x1704 ? 0UL : 1UL));
            }
            for(ULONG step=0x1710;step<=0x171B;++step) {
                seedS1(); s1=s1Trace(); failGmuStep=step;
                assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_IO_DEVICE_ERROR && s1.Cp.Reason==6);
            }
            seedS1(); s1=s1Trace(); dropGxAtStep=0x1710;
            assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_INVALID_DEVICE_STATE);
            assert(s1.Cp.Reason==GX_REG_HM_OFF && s1.Cp.CpHwFault==0xFFFFFFFF && s1.IbRemaining==0xFFFFFFFF);
            seedS1(); s1=s1Trace(); s1Completes=false; failGmuStep=0x1720;
            assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_IO_TIMEOUT && s1.Cp.Reason==9);
            seedS1(); s1=s1Trace(); failAddrMap=0;
            assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_INSUFFICIENT_RESOURCES && s1Writes.empty());
            for(ULONG bad=0;bad<9;++bad) {
                seedS1(); s1=s1Trace();
                if(bad==0) cpShadow[0]=61;
                if(bad==1) *GmuCfgReg(0x807)=61;
                if(bad==2) *GmuCfgReg(0x806)=61;
                if(bad==3) cpRing[61]=0;
                if(bad==4) cpRing[0x7034/4]=0;
                if(bad==5) probe.Regions[1].Mapped=false;
                if(bad==6) *GmuCfgReg(0x38)=1;
                if(bad==7) g3cMaskChangeStep=0x1704;
                if(bad==8) {g3cMaskChangeStep=0x1705; s1FenceDrops=1;}
                assert(GmuS1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&s1)==STATUS_INVALID_DEVICE_STATE);
                assert(s1.Cp.Reason==(bad>=6 ? 15UL : 12UL) && s1.Cp.WptrWrites==(bad==8 ? 1UL : 0UL));
            }
            const auto seedR1=[&]() {
                seedS1(); allowR1Writes=true; r1Writes.clear(); r1Completes=true; r1Draw=true; r1Ccu=true;
                r1Event=true; r1FenceForever=false; r1BadRptr=false; r1FenceDrops=0; r1ErrorBits=0;
                r1BadSentinel=0xFFFFFFFF; r1BadPixel=0xFFFFFFFF; r1Color=0x000033FF; r1Scatter=0xFFFFFFFF;
                r1CopyTarget=nullptr; r1CopyBadDword=0xFFFFFFFF;
                r1RectEnd=12;
                std::memcpy(cpRing.data()+0x6300/4,GmuS1Ib,sizeof(GmuS1Ib));
                std::memcpy(cpRing.data()+0x6800/4,GmuS1Shader,sizeof(GmuS1Shader));
                std::memcpy(cpRing.data()+73,GmuS1Packet,sizeof(GmuS1Packet));
                cpShadow[0]=82; *GmuCfgReg(0x806)=82; *GmuCfgReg(0x807)=82;
                cpRing[0x7038/4]=0xCAFE000C; cpRing[0x7C00/4]=0xC0DE5EED;
                for(ULONG i=1;i<16;++i) cpRing[0x7C00/4+i]=0xFFFFFFFF;
            };
            const auto r1Trace=[]() {GMU_R1_TRACE t; GmuR1TraceInit(&t); t.SChecks=0x1F; t.SReason=0; t.Checks=1; return t;};
            seedR1(); auto r1=r1Trace();
            assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==0);
            assert(r1.Checks==0x1F && r1.Prepared==1 && r1.Cp.Reason==0 && r1.Cp.SqeWritten==0);
            assert(r1.Cp.ShadowBefore==82 && r1.Cp.ShadowAfter==91 && r1.Cp.Rptr==91 && r1.Cp.WptrAfter==91);
            assert(r1.InsideMatches==64 && r1.OutsideMatches==0 && r1.WhiteCount==192 && r1.OtherCount==0 && r1.OtherFirst==0xFFFFFFFF);
            assert(r1.IbBadCount==0 && r1.IbFirstBad==0xFFFFFFFF && r1.IbBadValue==0xFFFFFFFF);
            assert(r1.ScanDwords==6898 && r1.ScanCount[0]==0 && r1.ScanCount[1]==0);
            assert(r1.Cp.WptrWrites==1 && r1Writes.size()==1 && g2Writes.size()==1 && testMaps.size()==originalMaps);
            assert(r1.IbBaseLow==0x70014000 && r1.IbRemaining==0 && r1.Cp.Fsr==0x400);
            assert(r1.DataBefore[0]==0xFFFFFFFF && r1.DataBefore[1]==0xFFFFFFFF);
            assert(r1.DataAfter[0]==0xCAFE000D && r1.DataAfter[1]==0xCAFE000E);
            assert(std::memcmp(r1.Vs,GmuR1Vs,sizeof(r1.Vs))==0 && std::memcmp(r1.Fs,GmuR1Fs,sizeof(r1.Fs))==0);
            assert(std::memcmp(r1.Vertices,GmuR1Vertices,sizeof(r1.Vertices))==0);
            for(ULONG i=0;i<256;++i) {
                const bool inside=i%16>=4 && i%16<12 && i/16>=4 && i/16<12;
                assert(r1.Target[i]==(inside ? 0x000033FFUL : 0xFFFFFFFFUL));
            }
            for(ULONG i=0;i<16;++i) assert(r1.Sentinel[i]==0xFFFFFFFF);
            for(ULONG bad=0;bad<5;++bad) {
                seedR1(); r1=r1Trace(); ULONG bit=0;
                if(bad==0) {r1BadRptr=true; bit=2;}
                if(bad==1) {r1Ccu=false; bit=4;}
                if(bad==2) {r1Event=false; bit=4;}
                if(bad==3) {r1Draw=false; bit=8;}
                if(bad==4) {r1BadSentinel=15; bit=16;}
                assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_DEVICE_ERROR);
                assert(r1.Cp.Reason==13 && r1.Checks==(0x1FUL&~bit));
            }
            seedR1(); r1=r1Trace(); r1Color=0xFF330000;
            assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_DEVICE_ERROR);
            assert(r1.OtherCount==64 && r1.OtherFirst==68 && r1.OtherValue==0xFF330000 && r1.WhiteCount==192);
            for(ULONG end:{11UL,13UL}) {
                seedR1(); r1=r1Trace(); r1RectEnd=end;
                assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_DEVICE_ERROR);
                assert(r1.InsideMatches+r1.OutsideMatches==(end==11 ? 49UL : 81UL));
                assert(r1.Checks==(end==11 ? 0x17UL : 0xFUL));
            }
            seedR1(); r1=r1Trace(); r1BadPixel=0;
            assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_DEVICE_ERROR);
            assert(r1.Checks==0xF && r1.OtherFirst==0 && r1.OtherValue==0);
            for(ULONG off:{0x4000UL,0x4800UL,0x4880UL,0x4900UL}) {
                seedR1(); r1=r1Trace(); r1CopyTarget=cpRing.data()+off/4; r1CopyBadDword=3;
                assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_DEVICE_ERROR && r1Writes.empty());
                if(off==0x4000) assert(r1.IbBadCount==1 && r1.IbFirstBad==3 && r1.IbBadValue==(GmuR1Ib[3]^1));
            }
            const ULONG r1ScanEdges[]={0x16C,0x3FFC,0x44E8,0x47FC,0x4920,0x4BFC,0x5040,0x5FFC,
                                       0x6240,0x62FC,0x6400,0x67FC,0x6880,0x6FFC,0x7044,0x73FC,0x7840,0x7BFC,0x7C40,0x7FFC};
            for(ULONG color:{0x000033FFUL,0xFF330000UL}) for(ULONG off:r1ScanEdges) {
                seedR1(); r1=r1Trace(); r1Scatter=off; r1Color=color;
                const NTSTATUS result=GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1);
                assert(result==(color==0x000033FF ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR));
                const ULONG which=color==0x000033FF ? 0 : 1;
                assert(r1.ScanCount[which]==1 && r1.ScanFirst[which]==off);
            }
            seedR1(); r1=r1Trace(); r1FenceDrops=3;
            assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==0 && r1.Cp.WptrWrites==4);
            seedR1(); r1=r1Trace(); r1FenceForever=true;
            assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_TIMEOUT && r1.Cp.Reason==10 && r1.Cp.WptrWrites==201);
            for(ULONG bad=0;bad<3;++bad) {
                seedR1(); r1=r1Trace();
                if(bad==0) r1Completes=false;
                if(bad==1) g2RbbmBusy=true;
                if(bad==2) g2GmuBusy=true;
                assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_TIMEOUT && r1.Cp.Reason==9);
                if(bad==0) assert(r1.IbRemaining==314 && r1.Cp.ShadowAfter==82);
            }
            for(ULONG bit:errorBits) {
                seedR1(); r1=r1Trace(); r1ErrorBits=bit;
                assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_DEVICE_ERROR && r1.Cp.Reason==14);
            }
            for(ULONG step=0x1800;step<=0x1806;++step) {
                seedR1(); r1=r1Trace(); failGmuStep=step;
                assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_DEVICE_ERROR && r1.Cp.Reason==6);
                assert(r1.Cp.Attempted==(step<=0x1804 ? 0UL : 1UL));
            }
            for(ULONG step=0x1810;step<=0x181B;++step) {
                seedR1(); r1=r1Trace(); failGmuStep=step;
                assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_DEVICE_ERROR && r1.Cp.Reason==6);
            }
            seedR1(); r1=r1Trace(); r1Completes=false; failGmuStep=0x1820;
            assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_TIMEOUT && r1.Cp.Reason==9);
            seedR1(); r1=r1Trace(); dropGxAtStep=0x1810;
            assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_INVALID_DEVICE_STATE);
            assert(r1.Cp.Reason==GX_REG_HM_OFF && r1.Cp.CpHwFault==0xFFFFFFFF);
            seedR1(); r1=r1Trace(); failAddrMap=0;
            assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_INSUFFICIENT_RESOURCES && r1Writes.empty());
            seedR1(); r1=r1Trace(); g2Hang=true;
            assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_IO_DEVICE_ERROR && r1.Cp.Reason==11);
            for(ULONG bad=0;bad<9;++bad) {
                seedR1(); r1=r1Trace();
                if(bad==0) cpShadow[0]=73;
                if(bad==1) *GmuCfgReg(0x806)=73;
                if(bad==2) cpRing[73]=0;
                if(bad==3) cpRing[0x7038/4]=0;
                if(bad==4) cpRing[0x7C00/4]=0;
                if(bad==5) probe.Regions[1].Mapped=false;
                if(bad==6) *GmuCfgReg(0x38)=1;
                if(bad==7) g3cMaskChangeStep=0x1804;
                if(bad==8) {g3cMaskChangeStep=0x1805; r1FenceDrops=1;}
                assert(GmuR1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&r1)==STATUS_INVALID_DEVICE_STATE);
                assert(r1.Cp.Reason==(bad>=6 ? 15UL : 12UL) && r1.Cp.WptrWrites==(bad==8 ? 1UL : 0UL));
            }
            // D-1: two-pass peer and mandatory CPU restore, including errors and quarantine prerequisites.
            const ULONG disp[]={0xE1000000,0,1080,2400,4320,21};
            const auto seedD1=[&]() {
                probe.Count=3; seedR1();
                assert(GmuD1Plan(&probe)); probe.Regions[3].Mapped=true;
                allowD1Writes=true; d1Completes=true; d1Odd=true; d1Fences=true; d1Outside=false; d1FenceForever=false;
                d1Restoring=false; d1MapFails=false; d1RestoreDrop=0xFFFFFFFF; d1ErrorBits=0; d1FenceDrops=0;
                d1Maps=0; d1Unmaps=0; d1Writes.clear(); failAfter=-1;
                std::memcpy(cpRing.data()+0x4000/4,GmuR1Ib,sizeof(GmuR1Ib));
                std::memcpy(cpRing.data()+0x4800/4,GmuR1Vs,sizeof(GmuR1Vs));
                std::memcpy(cpRing.data()+0x4880/4,GmuR1Fs,sizeof(GmuR1Fs));
                std::memcpy(cpRing.data()+0x4900/4,GmuR1Vertices,sizeof(GmuR1Vertices));
                std::memcpy(cpRing.data()+82,GmuR1Packet,sizeof(GmuR1Packet));
                cpShadow[0]=91; *GmuCfgReg(0x806)=91; *GmuCfgReg(0x807)=91;
                cpRing[0x703C/4]=0xCAFE000D; cpRing[0x7040/4]=0xCAFE000E;
                for(ULONG i=0;i<256;++i) cpRing[0x4C00/4+i]=(i%16>=4 && i%16<12 && i/16>=4 && i/16<12) ? 0x000033FF : 0xFFFFFFFF;
                for(ULONG i=0;i<d1Band.size();++i) d1Band[i]=0xAB000000+i;
            };
            const auto d1Trace=[&]() {GMU_D1_TRACE t; GmuD1TraceInit(&t, disp); t.RChecks=0x1F; t.RReason=0; t.Checks=1; return t;};
            seedD1(); auto d1=d1Trace(); const auto bandBefore=d1Band; const auto poolBefore=allocations.size();
            assert(GmuD1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&d1,reinterpret_cast<HANDLE>(2))==0);
            assert(d1.Checks==0x3F && d1.Cp.Reason==0 && d1.Cp.WptrAfter==100 && d1.Cp.ShadowAfter==100);
            assert(d1.InsideEven==32768 && d1.InsideOdd==32768 && d1.InsideOther==0 && d1.OutsideChanged==0);
            assert(d1.HoldMs==5000 && d1.HoldStatus==0 && d1.RestoreMismatch==0 && d1.RestoreStatus==0);
            assert(d1.BandMapped==1 && d1Maps==1 && d1Unmaps==1 && d1Band==bandBefore && allocations.size()==poolBefore);
            assert(d1.ScanDwords==6766 && d1.ScanCount==0 && d1.IbBadCount==0);
            assert(d1.Edge[1]==0xFFFF3300 && d1.Edge[2]==0xFFFF3300 && d1.Edge[8]==0xFFFF3300);
            assert(d1.DataAfter[0]==0xCAFE000F && d1.DataAfter[1]==0xCAFE0010 && d1.DataAfter[2]==0xCAFE0011);
            for(ULONG bad=0;bad<9;++bad) {
                seedD1(); d1=d1Trace();
                if(bad==0) d1Odd=false;
                if(bad==1) d1Outside=true;
                if(bad==2) d1Fences=false;
                if(bad==3) d1Completes=false;
                if(bad==4) failGmuStep=0x1931;
                if(bad==5) failGmuStep=0x1932;
                if(bad==6) failGmuStep=0x1930;
                if(bad==7) {d1Completes=false; failGmuStep=0x1920;}
                if(bad==8) d1ErrorBits=1UL<<9;
                assert(!NT_SUCCESS(GmuD1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&d1,reinterpret_cast<HANDLE>(2))));
                assert(d1.RestoreMismatch==0 && d1Band==bandBefore && d1Unmaps==1 && allocations.size()==poolBefore);
                if(bad==3 || bad==7) assert(d1.Cp.Reason==9);
                failGmuStep=0xFFFFFFFF;
            }
            seedD1(); d1=d1Trace(); d1RestoreDrop=(1072*4320+412*4-0x462000)/4;
            assert(GmuD1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&d1,reinterpret_cast<HANDLE>(2))==STATUS_IO_DEVICE_ERROR);
            assert(d1.RestoreMismatch==1 && d1.RestoreFirst==d1RestoreDrop*4 && !(d1.Checks&32) && d1Unmaps==1);
            for(ULONG bad=0;bad<3;++bad) {
                seedD1(); d1=d1Trace();
                if(bad==0) d1MapFails=true;
                if(bad==1) failAfter=0;
                if(bad==2) {d1.Disp[4]=1;}
                assert(!NT_SUCCESS(GmuD1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&d1,reinterpret_cast<HANDLE>(2))));
                assert(d1Writes.empty() && d1Maps==0 && d1Band==bandBefore && allocations.size()==poolBefore);
            }
            seedD1(); d1=d1Trace(); d1FenceDrops=3;
            assert(GmuD1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&d1,reinterpret_cast<HANDLE>(2))==0 && d1.Cp.WptrWrites==4);
            seedD1(); d1=d1Trace(); d1FenceForever=true;
            assert(GmuD1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&d1,reinterpret_cast<HANDLE>(2))==STATUS_IO_TIMEOUT);
            assert(d1.Cp.Reason==10 && d1.Cp.WptrWrites==201 && d1Band==bandBefore && d1Unmaps==1);
            // V-1 re-backs-up the restored band and runs only after successful D-1.
            const auto seedV1=[&]() {
                seedD1(); allowV1Writes=true; v1Completes=true; v1Odd=true; v1BadAlpha=false; v1Outside=false;
                v1CoreOut=false; v1EdgeOnly=false; v1FenceForever=false; v1BadRptr=false; v1SaveFails=false;
                v1Delta[0]=0; v1Delta[1]=0; v1Delta[2]=0; v1Fences=7; v1FenceDrops=0; v1ErrorBits=0;
                v1Scatter=0xFFFFFFFF; v1BadCore=0xFFFFFFFF; v1Writes.clear();
                std::memcpy(cpRing.data()+0x4500/4,GmuD1Ib,sizeof(GmuD1Ib));
                std::memcpy(cpRing.data()+0x4A00/4,GmuD1Fs,sizeof(GmuD1Fs));
                std::memcpy(cpRing.data()+0x4A80/4,GmuD1VertA,sizeof(GmuD1VertA));
                std::memcpy(cpRing.data()+0x4AA0/4,GmuD1VertB,sizeof(GmuD1VertB));
                std::memcpy(cpRing.data()+91,GmuD1Packet,sizeof(GmuD1Packet));
                cpShadow[0]=100; *GmuCfgReg(0x806)=100; *GmuCfgReg(0x807)=100;
                cpRing[0x7044/4]=0xCAFE000F; cpRing[0x7048/4]=0xCAFE0010; cpRing[0x704C/4]=0xCAFE0011;
                GmuD1TraceInit(&d1,disp); d1.Cp.Status=0; d1.Cp.Reason=0; d1.Checks=0x3F;
                d1.Cp.WptrAfter=100; d1.DataAfter[0]=0xCAFE000F; d1.DataAfter[1]=0xCAFE0010;
                d1.DataAfter[2]=0xCAFE0011; d1.RestoreMismatch=0; d1.RestoreStatus=0;
            };
            GMU_V1_TRACE v1;
            const auto initV1=[&]() {GmuV1TraceInit(&v1); v1.DChecks=d1.Checks; v1.DReason=d1.Cp.Reason; v1.Checks=1;};
            const auto submitV1=[&]() {return GmuV1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&v1,reinterpret_cast<HANDLE>(2),&d1);};
            seedV1(); initV1(); assert(GmuV1CanFollow(&d1) && submitV1()==0);
            assert(v1.Checks==0x3F && v1.Cp.Reason==0 && v1.Cp.WptrAfter==109 && v1.Cp.ShadowAfter==109 && v1.Cp.Rptr==109);
            assert(v1.CoreEven==15500 && v1.CoreOdd==15500 && v1.CoreBad==0 && v1.OutsideChanged==0);
            assert(v1.MaxErr[0]==0 && v1.MaxErr[1]==0 && v1.MaxErr[2]==0 && v1.EdgeChanged+v1.EdgeSame==3536);
            assert(v1.HoldMs==5000 && v1.RestoreMismatch==0 && v1.RestoreStatus==0 && d1Band==bandBefore);
            assert(v1.ScanDwords==6551 && v1.ScanCount==0 && v1.IbBadCount==0 && d1Maps==1 && d1Unmaps==1 && allocations.size()==poolBefore);
            const ULONG samples[]={0xFF555556,0xFFF20607,0xFF07F305,0xFF0704F4,0xFF077C7D,0xFF37A325};
            for(ULONG i=0;i<6;++i) assert(v1.Sample[i*2]==samples[i] && v1.Sample[i*2+1]==samples[i]);
            for(ULONG bad=0;bad<8;++bad) {
                seedV1();
                if(bad==0) d1.Cp.Status=STATUS_IO_DEVICE_ERROR;
                if(bad==1) d1.Cp.Reason=13;
                if(bad==2) d1.Checks&=~32UL;
                if(bad==3) d1.Cp.WptrAfter=99;
                if(bad==4) d1.DataAfter[0]=0;
                if(bad==5) d1.DataAfter[2]=0;
                if(bad==6) d1.RestoreMismatch=1;
                if(bad==7) d1.RestoreStatus=STATUS_IO_DEVICE_ERROR;
                assert(!GmuV1CanFollow(&d1));
            }
            for(LONG delta:{-2L,2L,3L}) {
                seedV1(); initV1(); v1Delta[0]=delta; v1Delta[1]=delta; v1Delta[2]=delta;
                const NTSTATUS result=submitV1();
                if(delta==3) {assert(result==STATUS_IO_DEVICE_ERROR && !(v1.Checks&8) && v1.CoreBad && v1.MaxErr[0]==3);}
                else {assert(result==0 && v1.Checks==0x3F && v1.MaxErr[0]==2 && v1.MaxErr[1]==2 && v1.MaxErr[2]==2);}
                assert(d1Band==bandBefore && d1Unmaps==1);
            }
            seedV1(); initV1(); v1EdgeOnly=true;
            assert(submitV1()==0 && v1.Checks==0x3F && v1.OutsideChanged==0 && v1.EdgeChanged>0);
            for(ULONG bad=0;bad<9;++bad) {
                seedV1(); initV1();
                if(bad==0) v1Odd=false;
                if(bad==1) v1Outside=true;
                if(bad==2) v1CoreOut=true;
                if(bad==3) v1BadAlpha=true;
                if(bad==4) v1Fences=6;
                if(bad==5) v1BadCore=(1242*4320+540*4-0x462000)/4;
                if(bad==6) v1BadRptr=true;
                if(bad==7) v1Completes=false;
                if(bad==8) v1ErrorBits=1UL<<9;
                assert(!NT_SUCCESS(submitV1()) && v1.RestoreMismatch==0 && d1Band==bandBefore && d1Unmaps==1);
                assert(allocations.size()==poolBefore);
                if(bad==7) assert(v1.Cp.Reason==9);
                if(bad==8) assert(v1.Cp.Reason==14);
            }
            for(ULONG off:{0x5100UL,0x5400UL,0x5480UL,0x5500UL,400UL}) {
                seedV1(); initV1(); r1CopyTarget=cpRing.data()+off/4; r1CopyBadDword=3;
                assert(submitV1()==STATUS_IO_DEVICE_ERROR && v1Writes.empty() && v1.RestoreMismatch==0 && d1Band==bandBefore);
                if(off==0x5100) assert(v1.IbBadCount==1 && v1.IbFirstBad==3);
            }
            for(ULONG step:{0x1A00UL,0x1A01UL,0x1A02UL,0x1A08UL,0x1A03UL,0x1A04UL,0x1A05UL,0x1A06UL,
                            0x1A10UL,0x1A1BUL,0x1A20UL,0x1A30UL,0x1A31UL,0x1A32UL}) {
                seedV1(); initV1(); failGmuStep=step;
                assert(submitV1()==STATUS_IO_DEVICE_ERROR && d1Band==bandBefore && allocations.size()==poolBefore);
                if(d1Maps) assert(d1Unmaps==1 && v1.RestoreMismatch==0);
            }
            seedV1(); initV1(); v1Completes=false; failGmuStep=0x1A20;
            assert(submitV1()==STATUS_IO_TIMEOUT && v1.Cp.Reason==9 && d1Band==bandBefore);
            seedV1(); initV1(); dropGxAtStep=0x1A10;
            assert(submitV1()==STATUS_INVALID_DEVICE_STATE && v1.RestoreMismatch==0 && d1Band==bandBefore && d1Unmaps==1);
            seedV1(); initV1(); v1SaveFails=true;
            assert(submitV1()==STATUS_IO_DEVICE_ERROR && v1.HoldMs==0 && v1.Cp.Reason==6 && d1Band==bandBefore);
            seedV1(); initV1(); d1RestoreDrop=(1242*4320+540*4-0x462000)/4;
            assert(submitV1()==STATUS_IO_DEVICE_ERROR && v1.RestoreMismatch==1 && !(v1.Checks&32) && d1Unmaps==1);
            for(ULONG off:{0x52CCUL,0x53FCUL,0x5560UL,0x705CUL,0x50FCUL}) {
                seedV1(); initV1(); v1Scatter=off;
                assert(submitV1()==0 && v1.ScanCount==1 && v1.ScanFirst==off && d1Band==bandBefore);
            }
            for(ULONG bad=0;bad<10;++bad) {
                seedV1(); initV1();
                if(bad==0) cpRing[91]=0;
                if(bad==1) cpRing[0x4500/4]=0;
                if(bad==2) cpRing[0x4A00/4]=1;
                if(bad==3) cpRing[0x4A80/4]=0;
                if(bad==4) cpRing[0x7044/4]=0;
                if(bad==5) cpShadow[0]=99;
                if(bad==6) probe.Regions[3].Mapped=false;
                if(bad==7) *GmuCfgReg(0x38)=1;
                if(bad==8) d1MapFails=true;
                if(bad==9) failAfter=0;
                assert(!NT_SUCCESS(submitV1()) && v1Writes.empty() && d1Maps==0 && allocations.size()==poolBefore);
            }
            seedV1(); initV1(); v1FenceDrops=3;
            assert(submitV1()==0 && v1.Cp.WptrWrites==4 && d1Band==bandBefore);
            seedV1(); initV1(); v1FenceForever=true;
            assert(submitV1()==STATUS_IO_TIMEOUT && v1.Cp.Reason==10 && v1.Cp.WptrWrites==201 && d1Band==bandBefore);
            // A-1: sequential append, changing VB only after the previous unique fence.
            const auto seedA1=[&]() {
                seedV1(); allowA1Writes=true; a1FenceForever=false; a1BadSq=false; a1BadBg=false; a1Outside=false; a1Odd=true; a1SaveFails=false;
                a1StopFrame=0xFFFFFFFF; a1WrongFrame=0xFFFFFFFF; a1ErrorFrame=0xFFFFFFFF; a1FenceDrops=0; a1DelayUs=0; a1Fences=3;
                a1Pending=0xFFFFFFFF; a1Due=0; a1Writes.clear(); a1Frames.clear(); a1Progress.clear(); a1Vertices.clear();
                std::memcpy(cpRing.data()+0x5100/4,GmuV1Ib,sizeof(GmuV1Ib));
                std::memcpy(cpRing.data()+0x5400/4,GmuV1Fs,sizeof(GmuV1Fs));
                std::memcpy(cpRing.data()+0x5480/4,GmuV1VertA,sizeof(GmuV1VertA));
                std::memcpy(cpRing.data()+0x5500/4,GmuV1VertB,sizeof(GmuV1VertB));
                std::memcpy(cpRing.data()+100,GmuV1Packet,sizeof(GmuV1Packet));
                cpShadow[0]=109; *GmuCfgReg(0x806)=109; *GmuCfgReg(0x807)=109;
                cpRing[0x7050/4]=0xCAFE0012; cpRing[0x7054/4]=0xCAFE0013; cpRing[0x7058/4]=0xCAFE0014;
                GmuV1TraceInit(&v1); v1.Cp.Status=0; v1.Cp.Reason=0; v1.Checks=0x3F; v1.Cp.WptrAfter=109;
                v1.DataAfter[0]=0xCAFE0012; v1.DataAfter[1]=0xCAFE0013; v1.DataAfter[2]=0xCAFE0014;
                v1.RestoreMismatch=0; v1.RestoreStatus=0;
            };
            GMU_A1_TRACE a1;
            const auto initA1=[&]() {GmuA1TraceInit(&a1); a1.VChecks=v1.Checks; a1.VReason=v1.Cp.Reason; a1.Checks=1;};
            const auto submitA1=[&]() {return GmuA1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&a1,reinterpret_cast<HANDLE>(2),&d1);};
            seedA1(); initA1(); assert(GmuA1CanFollow(&v1) && submitA1()==0);
            assert(a1.Checks==0x3F && a1.FramesDone==241 && a1.FailFrame==0xFFFFFFFF && a1.Cp.Reason==0);
            assert(a1.Cp.Rptr==2278 && a1.Cp.WptrAfter==2278 && a1.Cp.ShadowAfter==2278 && cpRing[2278]==0);
            assert(a1.SqGoodEven==12324 && a1.SqGoodOdd==12324 && a1.SqBad==0 && a1.BgGoodEven==19484 && a1.BgGoodOdd==19484 && a1.BgBad==0);
            assert(a1.OutsideChanged==0 && a1.RestoreMismatch==0 && a1.HoldMs==2000 && d1Band==bandBefore && allocations.size()==poolBefore);
            assert(a1.ScanDwords==4027 && a1.ScanCount==0 && d1Maps==1 && d1Unmaps==1 && a1.ElapsedMs==3840);
            assert(a1Frames.size()==241 && a1Writes.size()==241 && a1Progress.size()==16 && a1Progress.front()==0x1B40 && a1Progress.back()==0x1B4F);
            const ULONG aSamples[]={0xFF7F0181,0xFFF9060B,0xFF06EE0B,0xFF0606F4,0xFFF9EEF4,0xFFBE0141,0xFF3F01C1,0xFF202020,0xFF202020};
            for(ULONG i=0;i<9;++i) assert(a1.Sample[i*2]==aSamples[i] && a1.Sample[i*2+1]==aSamples[i]);
            for(ULONG fr=0;fr<241;++fr) for(ULONG pass=0;pass<2;++pass) for(ULONG i=48;i<96;++i)
                assert(a1Vertices[fr][pass*48+i-48]==GmuA1VertexWord(fr%120,pass!=0,i));
            seedA1(); initA1(); a1DelayUs=200;
            assert(submitA1()==0 && a1.FenceMaxUs==200 && a1.FenceTotalUs==48200 && a1.ElapsedMs==3888);
            for(ULONG bad=0;bad<5;++bad) {
                seedA1(); initA1();
                if(bad==0) a1BadSq=true;
                if(bad==1) a1BadBg=true;
                if(bad==2) a1Outside=true;
                if(bad==3) a1Odd=false;
                if(bad==4) a1Fences=0;
                assert(submitA1()==STATUS_IO_DEVICE_ERROR && a1.Cp.Reason==13 && d1Band==bandBefore && a1.RestoreMismatch==0);
                if(bad==0) assert(a1.SqBad==1 && (a1.Checks&16) && !(a1.Checks&8));
                if(bad==1) assert(a1.BgBad==1 && (a1.Checks&8) && !(a1.Checks&16));
            }
            for(ULONG bad=0;bad<3;++bad) {
                seedA1(); initA1();
                if(bad==0) a1StopFrame=5;
                if(bad==1) a1WrongFrame=5;
                if(bad==2) a1ErrorFrame=5;
                const NTSTATUS result=submitA1();
                assert(!NT_SUCCESS(result) && a1.FramesDone==5 && a1.FailFrame==5 && a1Writes.size()==6 && cpRing[109+9*6]==0);
                assert(a1.FailWptr==163 && a1.Cp.Reason==(bad==2 ? 14UL : 16UL) && d1Band==bandBefore && d1Unmaps==1);
                assert(a1.HoldMs==0 && a1.HoldStatus==STATUS_NOT_FOUND);
                if(bad==0) assert(a1.FailData25==0xA1000004 && a1.FailRptr==154 && a1.FenceMaxUs==100000);
                for(ULONG i=48;i<96;++i) assert(cpRing[0x5800/4+i]==GmuA1VertexWord(5,FALSE,i));
            }
            for(ULONG step:{0x1B00UL,0x1B01UL,0x1B02UL,0x1B08UL,0x1B03UL,0x1B41UL,0x1B10UL,0x1B20UL,0x1B30UL,0x1B31UL,0x1B32UL}) {
                seedA1(); initA1(); failGmuStep=step;
                assert(submitA1()==STATUS_IO_DEVICE_ERROR && d1Band==bandBefore && allocations.size()==poolBefore);
                if(d1Maps) assert(d1Unmaps==1 && a1.RestoreMismatch==0);
                if(step==0x1B41) assert(a1.FramesDone==16 && a1.FailFrame==16 && a1Writes.size()==16);
            }
            for(ULONG bad=0;bad<5;++bad) {
                seedA1(); initA1();
                if(bad==0) cpRing[2277]=1;
                if(bad==1) cpRing[0x5400/4]=0;
                if(bad==2) *GmuCfgReg(0x38)=1;
                if(bad==3) d1MapFails=true;
                if(bad==4) failAfter=0;
                assert(!NT_SUCCESS(submitA1()) && a1Writes.empty() && d1Maps==0 && allocations.size()==poolBefore);
            }
            seedA1(); initA1(); r1CopyTarget=cpRing.data()+0x5600/4; r1CopyBadDword=1;
            assert(submitA1()==STATUS_IO_DEVICE_ERROR && a1.IbBadCount==1 && a1Writes.empty() && d1Band==bandBefore);
            seedA1(); initA1(); a1SaveFails=true;
            assert(submitA1()==STATUS_IO_DEVICE_ERROR && a1.HoldMs==0 && d1Band==bandBefore);
            seedA1(); initA1(); a1StopFrame=5; failGmuStep=0x1B20;
            assert(submitA1()==STATUS_IO_TIMEOUT && a1.Cp.Reason==16 && d1Band==bandBefore);
            seedA1(); initA1(); d1RestoreDrop=(1200*4320+540*4-0x462000)/4;
            assert(submitA1()==STATUS_IO_DEVICE_ERROR && a1.RestoreMismatch==1 && !(a1.Checks&32));
            seedA1(); initA1(); a1FenceDrops=3;
            assert(submitA1()==0 && a1.Cp.WptrWrites==244 && a1.FramesDone==241);
            seedA1(); initA1(); a1FenceForever=true;
            assert(submitA1()==STATUS_IO_TIMEOUT && a1.Cp.Reason==10 && a1.FramesDone==0 && a1.FailFrame==0 && d1Band==bandBefore);
            const auto seedT1=[&]() {
                seedA1(); allowT1Writes=true; t1Completes=true; t1Zero=false; t1Black=false; t1Pad=false; t1Transpose=false; t1FlipX=false; t1FlipY=false;
                t1Outside=false; t1BoxDamage=false; t1Odd=true; t1FenceForever=false; t1BadRemaining=false; t1SaveFails=false; t1Pending=false;
                t1Fences=7; t1FenceDrops=0; t1ErrorBits=0; t1DelayUs=0; t1BadTexel=0xFFFFFFFF; t1Writes.clear();
                t1CopyTrigger=nullptr; t1CorruptTarget=nullptr;
                std::memcpy(cpRing.data()+0x5600/4,GmuA1Ib,sizeof(GmuA1Ib));
                assert(GmuA1Vertices(cpRing.data(),0,TRUE)==0);
                for(ULONG f=0;f<241;++f) for(ULONG i=0;i<9;++i) cpRing[109+9*f+i]=i<8 ? GmuA1Packet[i] : (0xA1000000|f);
                cpShadow[0]=2278; *GmuCfgReg(0x806)=2278; *GmuCfgReg(0x807)=2278;
                cpRing[0x705C/4]=0xCAFE0015; cpRing[0x7060/4]=0xCAFE0016; cpRing[0x7064/4]=0xA10000F0;
                GmuA1TraceInit(&a1); a1.Cp.Status=0; a1.Cp.Reason=0; a1.Checks=0x3F; a1.FramesDone=241; a1.Cp.WptrAfter=2278;
                a1.DataAfter[0]=0xCAFE0015; a1.DataAfter[1]=0xCAFE0016; a1.DataAfter[2]=0xA10000F0;
                a1.RestoreMismatch=0; a1.RestoreStatus=0;
            };
            GMU_T1_TRACE t1;
            const auto initT1=[&]() {GmuT1TraceInit(&t1); t1.AChecks=a1.Checks; t1.AReason=a1.Cp.Reason; t1.Checks=1;};
            const auto submitT1=[&]() {return GmuT1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&t1,reinterpret_cast<HANDLE>(2),&d1);};
            seedT1(); initT1(); assert(GmuT1CanFollow(&a1) && submitT1()==0);
            assert(t1.Checks==0x3F && t1.Cp.Reason==0 && t1.Cp.Rptr==2287 && t1.Cp.ShadowAfter==2287 && t1.Cp.WptrAfter==2287);
            assert(t1.SqGoodEven==12800 && t1.SqGoodOdd==12800 && t1.SqBad==0 && t1.TexelBad[0]==0 && t1.TexelBad[1]==0);
            assert(t1.BoxGoodEven==19968 && t1.BoxGoodOdd==19968 && t1.BoxBad==0 && t1.OutsideChanged==0);
            assert(t1.DescBad==0 && t1.IbBadCount==0 && t1.HoldMs==5000 && t1.RestoreMismatch==0 && d1Band==bandBefore && d1Unmaps==1);
            assert(t1.ScanDwords==3407 && t1.ScanCount==0 && allocations.size()==poolBefore && cpRing[2287]==0);
            const ULONG tSamples[]={0xFF101040,0xFFF010C0,0xFFF0F040,0xFF10F0C0,0xFF707040,0xFF90B0C0,0xFF101040,0xFF303040};
            for(ULONG i=0;i<8;++i) assert(t1.Sample[i*2]==tSamples[i] && t1.Sample[i*2+1]==tSamples[i]);
            for(ULONG bad=0;bad<9;++bad) {
                seedT1(); initT1();
                if(bad==0) t1Zero=true;
                if(bad==1) t1Black=true;
                if(bad==2) t1Pad=true;
                if(bad==3) t1FlipX=true;
                if(bad==4) t1FlipY=true;
                if(bad==5) t1Transpose=true;
                if(bad==6) t1BoxDamage=true;
                if(bad==7) t1Outside=true;
                if(bad==8) t1Odd=false;
                assert(submitT1()==STATUS_IO_DEVICE_ERROR && t1.Cp.Reason==13 && d1Band==bandBefore);
                if(bad<2) assert(t1.SqZero==25600 && t1.TexelBad[0]==0xFFFFFFFF && t1.TexelBad[1]==0xFFFFFFFF);
                if(bad==2) assert(t1.SqPad==25600);
                if(bad==6) assert(t1.BoxBad==1 && t1.BoxOld!=t1.BoxValue);
            }
            for(ULONG texel:{3UL,63UL}) {
                seedT1(); initT1(); t1BadTexel=texel;
                assert(submitT1()==STATUS_IO_DEVICE_ERROR && t1.SqBad==400 && t1.MaxErr[0]==1);
                assert(t1.TexelBad[texel/32]==(1UL<<(texel%32)) && t1.TexelBad[1-texel/32]==0);
            }
            for(ULONG off:{0x6884UL,0x6900UL,0x69C0UL,0x6A80UL,0x6AC0UL,0x6B00UL,0x6C00UL}) {
                seedT1(); initT1(); t1CopyTrigger=cpRing.data()+2278; t1CorruptTarget=cpRing.data()+off/4;
                assert(submitT1()==STATUS_IO_DEVICE_ERROR && t1.DescBad==1 && t1Writes.empty() && d1Band==bandBefore);
            }
            seedT1(); initT1(); r1CopyTarget=cpRing.data()+0x6400/4; r1CopyBadDword=1;
            assert(submitT1()==STATUS_IO_DEVICE_ERROR && t1.IbBadCount==1 && t1Writes.empty());
            for(ULONG bad=0;bad<8;++bad) {
                seedT1(); initT1();
                if(bad==0) cpRing[2277]=0;
                if(bad==1) cpRing[2286]=1;
                if(bad==2) cpRing[0x6400/4]=1;
                if(bad==3) cpRing[0x6DFC/4]=1;
                if(bad==4) cpRing[0x58C0/4]=0;
                if(bad==5) *GmuCfgReg(0x38)=1;
                if(bad==6) d1MapFails=true;
                if(bad==7) failAfter=0;
                assert(!NT_SUCCESS(submitT1()) && t1Writes.empty() && d1Maps==0 && allocations.size()==poolBefore);
            }
            for(ULONG bad=0;bad<3;++bad) {
                seedT1(); initT1();
                if(bad==0) t1Completes=false;
                if(bad==1) t1Fences=3;
                if(bad==2) t1BadRemaining=true;
                assert(submitT1()==STATUS_IO_TIMEOUT && t1.Cp.Reason==16 && t1.FenceUs==100000 && t1.HoldMs==0 && d1Band==bandBefore);
            }
            for(ULONG step:{0x1C00UL,0x1C01UL,0x1C02UL,0x1C08UL,0x1C03UL,0x1C04UL,0x1C10UL,0x1C20UL,0x1C30UL,0x1C31UL,0x1C32UL}) {
                seedT1(); initT1(); failGmuStep=step;
                assert(submitT1()==STATUS_IO_DEVICE_ERROR && d1Band==bandBefore && allocations.size()==poolBefore);
                if(d1Maps) assert(t1.RestoreMismatch==0 && d1Unmaps==1);
            }
            seedT1(); initT1(); t1DelayUs=200;
            assert(submitT1()==0 && t1.FenceUs==200);
            seedT1(); initT1(); t1ErrorBits=1UL<<9;
            assert(submitT1()==STATUS_IO_DEVICE_ERROR && t1.Cp.Reason==14 && d1Band==bandBefore);
            seedT1(); initT1(); t1SaveFails=true;
            assert(submitT1()==STATUS_IO_DEVICE_ERROR && t1.HoldMs==0 && d1Band==bandBefore);
            seedT1(); initT1(); d1RestoreDrop=(1130*4320+470*4-0x462000)/4;
            assert(submitT1()==STATUS_IO_DEVICE_ERROR && t1.RestoreMismatch==1 && !(t1.Checks&32));
            seedT1(); initT1(); t1FenceDrops=3;
            assert(submitT1()==0 && t1.Cp.WptrWrites==4);
            seedT1(); initT1(); t1FenceForever=true;
            assert(submitT1()==STATUS_IO_TIMEOUT && t1.Cp.Reason==10 && d1Band==bandBefore);
            const auto seedW1=[&]() {
                seedT1(); allowW1Writes=true; w1Active=false; w1FenceForever=false; w1Pending=false; w1ShadowOnly=false; w1SaveFails=false;
                w1FenceDrops=0; w1StopPub=0xFFFFFFFF; w1BadSizePub=0xFFFFFFFF; w1WrongPub=0xFFFFFFFF; w1ErrorPub=0xFFFFFFFF;
                w1DamageRing=0xFFFFFFFF; w1DamageOutside=0xFFFFFFFF; w1Cursor=2287; w1NextPacket=0; w1DelayUs=0;
                w1Writes.clear(); w1Pubs.clear(); w1ReadPackets.clear(); w1Progress.clear();
                std::memcpy(cpRing.data()+0x6400/4,GmuT1Ib,sizeof(GmuT1Ib));
                for(ULONG r=0;r<RTL_NUMBER_OF(GmuT1Regions);++r) for(ULONG off=GmuT1Regions[r][0];off<GmuT1Regions[r][0]+GmuT1Regions[r][1];off+=4)
                    cpRing[off/4]=GmuT1Word(off);
                std::memcpy(cpRing.data()+2278,GmuT1Packet,sizeof(GmuT1Packet));
                cpShadow[0]=2287; *GmuCfgReg(0x806)=2287; *GmuCfgReg(0x807)=2287; *GmuCfgReg(0x802)=0x20B;
                cpRing[0x7068/4]=0xCAFE0017; cpRing[0x706C/4]=0xCAFE0018; cpRing[0x7070/4]=0xCAFE0019;
                GmuT1TraceInit(&t1); t1.Cp.Status=0; t1.Cp.Reason=0; t1.Checks=0x3F; t1.Cp.WptrAfter=2287;
                t1.DataAfter[0]=0xCAFE0017; t1.DataAfter[1]=0xCAFE0018; t1.DataAfter[2]=0xCAFE0019;
                t1.RestoreMismatch=0; t1.RestoreStatus=0;
            };
            GMU_W1_TRACE w1;
            const auto initW1=[&]() {GmuW1TraceInit(&w1); w1.TChecks=t1.Checks; w1.TReason=t1.Cp.Reason; w1.RbCntlG1=0x20B;};
            const auto submitW1=[&]() {return GmuW1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&w1,reinterpret_cast<HANDLE>(2),&d1);};
            seedW1(); initW1(); assert(GmuW1CanFollow(&t1) && submitW1()==0);
            assert(w1.Checks==0x3F && w1.PubsDone==650 && w1.PacketsDone==1000 && w1.WrapCount==2 && w1.Cp.Reason==0);
            assert(w1.RbCntlG1==0x20B && w1.RbCntlLive==0x20B && w1.ZeroWptrPub==200 && w1.ZeroSeenRptr==0 && w1.StraddlePub==607);
            assert(w1.Cp.Rptr==3095 && w1.Cp.WptrAfter==3095 && w1.Cp.ShadowAfter==3095 && w1.DataAfter[3]==0xB10003E7);
            assert(w1.MaxRptrSeen==4095 && w1.RingBad==0 && w1.OutsideRingBad==0 && w1.ScanDwords==1597 && w1.ScanCount==0);
            assert(w1.SqGoodEven==12800 && w1.SqGoodOdd==12800 && w1.BoxGoodEven==19968 && w1.BoxGoodOdd==19968);
            assert(w1.HoldMs==2000 && w1.RestoreMismatch==0 && d1Band==bandBefore && allocations.size()==poolBefore && d1Unmaps==1);
            assert(w1Pubs.size()==650 && w1ReadPackets.size()==1000 && w1Pubs[200]==0 && w1Pubs[606]==4095 && w1Pubs[607]==71);
            assert(w1Progress.size()==11 && w1Progress.front()==0x1D40 && w1Progress.back()==0x1D4A);
            for(ULONG i=0;i<4096;++i) assert(cpRing[i]==GmuW1Expected(i));
            for(ULONG i=0;i<1000;++i) assert(w1ReadPackets[i]==i);
            seedW1(); initW1(); w1DelayUs=200;
            assert(submitW1()==0 && w1.WaitMaxUs==200 && w1.WaitTotalUs==130000);
            for(ULONG bad=0;bad<4;++bad) {
                seedW1(); initW1();
                if(bad==0) w1DamageRing=0;
                if(bad==1) w1DamageOutside=0x6400;
                if(bad==2) w1DamageOutside=0x7070;
                if(bad==3) t1Zero=true;
                assert(submitW1()==STATUS_IO_DEVICE_ERROR && w1.Cp.Reason==13 && d1Band==bandBefore);
                if(bad==0) assert(w1.RingBad==1 && w1.RingFirst==0);
                if(bad==1 || bad==2) assert(w1.OutsideRingBad==1 && w1.OutsideRingFirst==w1DamageOutside);
            }
            for(ULONG bad=0;bad<4;++bad) {
                seedW1(); initW1(); w1BadSizePub=bad>=2 ? 0 : 200;
                if(bad==1) w1ShadowOnly=true;
                if(bad==2 || bad==3) w1FenceForever=true;
                assert(submitW1()==STATUS_IO_DEVICE_ERROR && w1.Cp.Reason==18 && w1.MaxRptrSeen>=4096 && w1.HoldMs==0 && d1Band==bandBefore);
                if(bad<2) assert(w1.PubsDone==200 && w1.FailPub==200 && w1Writes.size()==201);
                if(bad==3) assert(w1.Cp.WptrWrites==1 && w1.FailPub==0); // detects a mismatch before any AHB retry
            }
            for(ULONG bad=0;bad<3;++bad) {
                seedW1(); initW1();
                if(bad==0) w1StopPub=607;
                if(bad==1) w1WrongPub=607;
                if(bad==2) w1ErrorPub=607;
                assert(!NT_SUCCESS(submitW1()) && w1.PubsDone==607 && w1.PacketsDone==656 && w1.FailPub==607 && w1.FailPacket==663);
                assert(w1.Cp.Reason==(bad==2 ? 14UL : 16UL) && d1Band==bandBefore && w1Writes.size()==608);
            }
            for(ULONG bad=0;bad<6;++bad) {
                seedW1(); initW1();
                if(bad==0) *GmuCfgReg(0x802)=0x20C;
                if(bad==1) w1.RbCntlG1=0x20C;
                if(bad==2) cpRing[0x7074/4]=1;
                if(bad==3) cpRing[2286]=0;
                if(bad==4) d1MapFails=true;
                if(bad==5) failAfter=0;
                assert(!NT_SUCCESS(submitW1()) && w1Writes.empty() && d1Maps==0 && allocations.size()==poolBefore);
                if(bad<4) assert(!(w1.Checks&1));
            }
            for(ULONG step:{0x1D00UL,0x1D01UL,0x1D02UL,0x1D08UL,0x1D03UL,0x1D41UL,0x1D10UL,0x1D20UL,0x1D30UL,0x1D31UL,0x1D32UL}) {
                seedW1(); initW1(); failGmuStep=step;
                assert(submitW1()==STATUS_IO_DEVICE_ERROR && d1Band==bandBefore && allocations.size()==poolBefore);
                if(step==0x1D41) assert(w1.PubsDone==64 && w1.FailPub==64 && w1Writes.size()==64);
            }
            seedW1(); initW1(); w1SaveFails=true;
            assert(submitW1()==STATUS_IO_DEVICE_ERROR && w1.HoldMs==0 && d1Band==bandBefore);
            seedW1(); initW1(); w1StopPub=607; failGmuStep=0x1D20;
            assert(submitW1()==STATUS_IO_TIMEOUT && w1.Cp.Reason==16 && d1Band==bandBefore);
            seedW1(); initW1(); d1RestoreDrop=(1130*4320+470*4-0x462000)/4;
            assert(submitW1()==STATUS_IO_DEVICE_ERROR && w1.RestoreMismatch==1 && !(w1.Checks&32));
            seedW1(); initW1(); w1FenceDrops=3;
            assert(submitW1()==0 && w1.Cp.WptrWrites==653 && w1.PubsDone==650);
            seedW1(); initW1(); w1FenceForever=true;
            assert(submitW1()==STATUS_IO_TIMEOUT && w1.Cp.Reason==10 && w1.PubsDone==0 && d1Band==bandBefore);
            // M-1: start from the W-1 final ring, preserving all T-1 resources.
            const auto seedM1=[&]() {
                seedW1();
                allowM1Writes=false; m1Active=false; m1Pending=false; m1Completes=true; m1SaveFails=false;
                m1NoClear=false; m1NoLoad=false; m1NoStore=false; m1FenceForever=false; m1BadSize=false; m1ShadowOnly=false;
                m1DelayUs=0; m1StopTile=0xFFFFFFFF; m1ErrorBits=0; m1FenceDrops=0; m1WrongTile=0xFFFFFFFF;
                m1DamageRing=0xFFFFFFFF; m1DamageOutside=0xFFFFFFFF; m1DamagePixel=0xFFFFFFFF; m1Due=0;
                m1Writes.clear(); m1Tiles.clear(); std::fill(m1TileMemory.begin(),m1TileMemory.end(),0xEEEEEEEE);

                allowM1Writes=true;
                for(ULONG i=0;i<4096;++i) cpRing[i]=GmuW1Expected(i);
                cpShadow[0]=3095; *GmuCfgReg(0x806)=3095; *GmuCfgReg(0x807)=3095;
                *GmuCfgReg(0x8E07)=0x10000000; cpRing[0x7074/4]=0xB10003E7;
                GmuW1TraceInit(&w1); w1.Cp.Status=0; w1.Cp.Reason=0; w1.Checks=0x3F;
                w1.PubsDone=650; w1.PacketsDone=1000; w1.Cp.Rptr=w1.Cp.WptrAfter=w1.Cp.ShadowAfter=3095;
                w1.DataAfter[3]=0xB10003E7; w1.RestoreMismatch=0; w1.RestoreStatus=0;
            };
            GMU_M1_TRACE m1;
            const auto initM1=[&]() {GmuM1TraceInit(&m1); m1.WChecks=w1.Checks; m1.WReason=w1.Cp.Reason;};
            const auto submitM1=[&]() {return GmuM1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&m1,reinterpret_cast<HANDLE>(2),&d1);};
            seedM1(); initM1(); assert(GmuM1CanFollow(&w1) && submitM1()==0);
            assert(m1.Checks==0x3F && m1.Cp.Reason==0 && m1.TilesDone==12 && m1.Data42==0xCAFE001A);
            assert(m1.CcuBefore==0x10000000 && m1.CcuAfter==0x10000000 && m1.RbCntlLive==0x20B);
            assert(m1.Cp.Rptr==3272 && m1.Cp.WptrAfter==3272 && m1.Cp.ShadowAfter==3272 && m1.Cp.WptrWrites==1);
            assert(m1.SqGood[0]==12800 && m1.SqGood[1]==12800 && m1.ClearGood==22016 && m1.BoxGood==19968);
            assert(m1.RingBad==0 && m1.OutsideRingBad==0 && m1.OutsideChanged==0 && m1.ScanCount==0 && m1.ScanDwords==1182);
            assert(m1.HoldMs==5000 && m1.RestoreMismatch==0 && d1Band==bandBefore && allocations.size()==poolBefore && d1Unmaps==1);
            assert(m1Writes.size()==1 && m1Writes[0]==3272 && m1Tiles.size()==12);
            for(ULONG k=0;k<12;++k) assert(m1.TileData[k]==(0xB2000000UL|k) && m1.TileBad[k]==0);
            assert(m1.FlushData[0]==0xB2000100 && m1.FlushData[1]==0xB2000101);
            assert(m1.Sample[20]==0xFF2E5A8C && m1.Sample[22]==0xFF2E5A8C);
            seedM1(); initM1(); m1DelayUs=300;
            assert(submitM1()==0 && m1.WaitUs==300 && d1Band==bandBefore);
            for(ULONG bad=0;bad<9;++bad) {
                seedM1(); initM1();
                if(bad==0) m1NoClear=true;
                if(bad==1) m1NoLoad=true;
                if(bad==2) m1NoStore=true;
                if(bad==3) m1DamageRing=3095;
                if(bad==4) m1DamageOutside=0x6400;
                if(bad==5) m1DamageOutside=0x7074;
                if(bad==6) m1WrongTile=4;
                if(bad==7) m1DamagePixel=(1072*4320+399*4-0x462000)/4;
                if(bad==8) m1DamageOutside=0x70AC;
                assert(submitM1()==STATUS_IO_DEVICE_ERROR && m1.Cp.Reason==13 && d1Band==bandBefore && allocations.size()==poolBefore);
                if(bad==0) assert(m1.ClearBad>0 && !(m1.Checks&16));
                if(bad==1) assert(m1.BoxBad>0 && !(m1.Checks&16));
                if(bad==3) assert(m1.RingBad==1 && m1.RingFirst==3095);
                if(bad==4 || bad==5 || bad==8) assert(m1.OutsideRingBad==1 && m1.OutsideRingFirst==m1DamageOutside);
                if(bad==6) assert(m1.TilesDone==4 && !(m1.Checks&2));
                if(bad==7) assert(m1.OutsideChanged==1);
            }
            for(ULONG k:{0UL,5UL,11UL}) {
                seedM1(); initM1(); m1StopTile=k;
                assert(submitM1()==STATUS_IO_TIMEOUT && m1.Cp.Reason==16 && m1.TilesDone==k+1 && m1.CcuAfter==0x3C400000);
                assert(m1.HoldMs==0 && d1Band==bandBefore && allocations.size()==poolBefore);
            }
            for(ULONG bad=0;bad<6;++bad) {
                seedM1(); initM1();
                if(bad==0) cpRing[12]^=1;
                if(bad==1) *GmuCfgReg(0x802)=0x20C;
                if(bad==2) *GmuCfgReg(0x8E07)=0x3C400000;
                if(bad==3) cpRing[0x6880/4]^=1;
                if(bad==4) cpRing[0x7840/4]=1;
                if(bad==5) *GmuCfgReg(0x38)=1;
                assert(submitM1()==STATUS_INVALID_DEVICE_STATE && !m1.Cp.Attempted && !(m1.Checks&1) && m1Writes.empty() && d1Maps==0);
            }
            for(ULONG step:{0x1E00UL,0x1E01UL,0x1E02UL,0x1E08UL,0x1E03UL,0x1E04UL,0x1E10UL,0x1E1CUL,0x1E20UL,0x1E30UL,0x1E31UL,0x1E32UL}) {
                seedM1(); initM1(); failGmuStep=step;
                assert(submitM1()==STATUS_IO_DEVICE_ERROR && d1Band==bandBefore && allocations.size()==poolBefore);
            }
            for(ULONG bad=0;bad<2;++bad) {
                seedM1(); initM1(); m1BadSize=true; m1ShadowOnly=bad!=0; m1FenceForever=true;
                assert(submitM1()==STATUS_IO_DEVICE_ERROR && m1.Cp.Reason==18 && m1.Cp.WptrWrites==1 && m1.HoldMs==0 && d1Band==bandBefore);
            }
            seedM1(); initM1(); m1FenceDrops=3;
            assert(submitM1()==0 && m1.Cp.WptrWrites==4 && d1Band==bandBefore);
            seedM1(); initM1(); m1FenceForever=true;
            assert(submitM1()==STATUS_IO_TIMEOUT && m1.Cp.Reason==10 && m1.HoldMs==0 && d1Band==bandBefore);
            seedM1(); initM1(); m1Completes=false; failGmuStep=0x1E20;
            assert(submitM1()==STATUS_IO_TIMEOUT && m1.Cp.Reason==16 && d1Band==bandBefore);
            seedM1(); initM1(); m1ErrorBits=1UL<<9;
            assert(submitM1()==STATUS_IO_DEVICE_ERROR && m1.Cp.Reason==14 && d1Band==bandBefore);
            seedM1(); initM1(); m1SaveFails=true;
            assert(submitM1()==STATUS_IO_DEVICE_ERROR && m1.HoldMs==0 && d1Band==bandBefore);
            seedM1(); initM1(); d1RestoreDrop=(1130*4320+470*4-0x462000)/4;
            assert(submitM1()==STATUS_IO_DEVICE_ERROR && m1.RestoreMismatch==1 && !(m1.Checks&32));
            seedM1(); initM1(); failAfter=0;
            assert(submitM1()==STATUS_INSUFFICIENT_RESOURCES && !m1.Cp.Attempted && d1Maps==0 && allocations.size()==poolBefore);
            seedM1(); initM1(); d1MapFails=true;
            assert(submitM1()==STATUS_INSUFFICIENT_RESOURCES && !m1.Cp.Attempted && allocations.size()==poolBefore);
            seedM1(); initM1(); failAddrMap=3; // RB_CCU_CNTL page
            assert(submitM1()==STATUS_INSUFFICIENT_RESOURCES && !m1.Cp.Attempted && d1Maps==0);
            seedM1(); initM1(); w1.Checks=0x1F; assert(!GmuM1CanFollow(&w1));
            // Q-1: M-1's final memory and restored framebuffer are the entry state.
            const auto seedQ1=[&]() {
                seedM1();
                allowQ1Writes=false; q1Active=false; q1Pending=false; q1FenceForever=false; q1ShadowOnly=false; q1SaveFails=false;
                q1Zero=false; q1Pad=false; q1Bg=false; q1Outside=false; q1BackgroundBad=false;
                q1StopFrame=0xFFFFFFFF; q1WrongFrame=0xFFFFFFFF; q1ErrorFrame=0xFFFFFFFF; q1BadSizeFrame=0xFFFFFFFF;
                q1FenceDrops=0; q1DelayUs=0; q1PendingFrame=0; q1DamageRing=0xFFFFFFFF; q1DamageOutside=0xFFFFFFFF; q1Due=0;
                q1Writes.clear(); q1Frames.clear(); q1Progress.clear(); q1Vertices.clear();
                allowQ1Writes=true;
                for(ULONG i=0;i<177;++i) cpRing[3095+i]=GmuM1Packet[i];
                for(const auto& ib:GmuM1Ibs) std::memcpy(cpRing.data()+ib.Offset/4,ib.Words,ib.Count*4);
                for(ULONG k=0;k<12;++k) cpRing[0x7078/4+k]=0xB2000000|k;
                cpRing[0x70A8/4]=0xCAFE001A; cpRing[0x70AC/4]=0xB2000100; cpRing[0x70B0/4]=0xB2000101;
                cpShadow[0]=3272; *GmuCfgReg(0x806)=3272; *GmuCfgReg(0x807)=3272;
                GmuM1TraceInit(&m1); m1.Cp.Status=0; m1.Cp.Reason=0; m1.Checks=0x3F;
                m1.Cp.Rptr=m1.Cp.WptrAfter=m1.Cp.ShadowAfter=3272; m1.Data42=0xCAFE001A;
                m1.CcuAfter=0x10000000; m1.RestoreMismatch=0; m1.RestoreStatus=0;
            };
            GMU_Q1_TRACE q1;
            const auto initQ1=[&]() {GmuQ1TraceInit(&q1); q1.MChecks=m1.Checks; q1.MReason=m1.Cp.Reason;};
            const auto submitQ1=[&]() {return GmuQ1Submit(&probe,reinterpret_cast<HANDLE>(1),cc,&oob,&q1,reinterpret_cast<HANDLE>(2),&d1);};
            seedQ1(); initQ1(); assert(GmuQ1CanFollow(&m1) && submitQ1()==0);
            assert(q1.Checks==0x3F && q1.Cp.Reason==0 && q1.FramesDone==121 && q1.DataAfter[2]==0xA2000078);
            assert(q1.CcuLive==0x10000000 && q1.RbCntlLive==0x20B && q1.PlaceBad==0 && q1.IbBadCount==0 && q1.DescBad==0);
            assert(q1.WrapCount==1 && q1.StraddleFrame==91 && q1.StraddleRptr==4 && q1.MaxRptrSeen==4091);
            assert(q1.Cp.Rptr==265 && q1.Cp.WptrAfter==265 && q1.Cp.ShadowAfter==265 && q1.WptrWrites==121);
            assert(q1.RingBad==0 && q1.OutsideRingBad==0 && q1.ScanCount==0 && q1.ScanDwords==731);
            assert(q1.SqGood[0]==12800 && q1.SqGood[1]==12800 && q1.BgGood[0]==19968 && q1.BgGood[1]==19968);
            assert(q1.HoldMs==2000 && q1.RestoreMismatch==0 && d1Band==bandBefore && allocations.size()==poolBefore && d1Unmaps==1);
            assert(q1Frames.size()==121 && q1Writes.size()==121 && q1Writes[90]==4091 && q1Writes[91]==4 && q1Writes[120]==265);
            assert(q1Progress.size()==8 && q1Progress.front()==0x1F40 && q1Progress.back()==0x1F47);
            for(ULONG fr=0;fr<121;++fr) for(ULONG pass=0;pass<2;++pass) for(ULONG i=0;i<96;++i)
                assert(q1Vertices[fr][96*pass+i]==GmuQ1VertexWord(fr%120,pass!=0,i));
            for(ULONG i=0;i<4096;++i) assert(cpRing[i]==GmuQ1Expected(i));
            assert(q1.Sample[16]==0xFF202020 && q1.Sample[18]==0xFF202020);
            seedQ1(); initQ1(); r1CopyTarget=cpRing.data()+0x7C40/4; r1CopyBadDword=5;
            assert(submitQ1()==STATUS_IO_DEVICE_ERROR && q1.IbBadCount==1 && q1.IbFirstBad==5 && !q1.Cp.Attempted && q1Writes.empty() && d1Band==bandBefore);
            seedQ1(); initQ1(); q1DelayUs=200;
            assert(submitQ1()==0 && q1.FenceMaxUs==200 && q1.FenceTotalUs==24200 && q1.ElapsedMs==1944);
            for(ULONG bad=0;bad<9;++bad) {
                seedQ1(); initQ1();
                if(bad==0) q1DamageRing=0;
                if(bad==1) q1DamageOutside=0x6400;
                if(bad==2) q1DamageOutside=0x70A8;
                if(bad==3) q1Zero=true;
                if(bad==4) q1Pad=true;
                if(bad==5) q1Bg=true;
                if(bad==6) q1BackgroundBad=true;
                if(bad==7) q1Outside=true;
                if(bad==8) q1DamageOutside=0x70B4;
                assert(submitQ1()==STATUS_IO_DEVICE_ERROR && q1.Cp.Reason==13 && d1Band==bandBefore && allocations.size()==poolBefore);
                if(bad==0) assert(q1.RingBad==1 && q1.RingFirst==0);
                if(bad==1 || bad==2 || bad==8) assert(q1.OutsideRingBad==1 && q1.OutsideRingFirst==q1DamageOutside);
                if(bad==3) assert(q1.SqZero==25600);
                if(bad==4) assert(q1.SqPad==25600);
                if(bad==5) assert(q1.SqBg==25600);
                if(bad==6) assert(q1.BgBad==1);
                if(bad==7) assert(q1.OutsideChanged==1);
            }
            for(ULONG bad=0;bad<3;++bad) {
                seedQ1(); initQ1();
                if(bad==0) q1StopFrame=91;
                if(bad==1) q1WrongFrame=91;
                if(bad==2) q1ErrorFrame=91;
                assert(!NT_SUCCESS(submitQ1()) && q1.FramesDone==91 && q1.FailFrame==91 && q1.Cp.Reason==(bad==2 ? 14UL : 16UL));
                assert(q1Writes.size()==92 && q1Vertices.size()==92 && q1.HoldMs==0 && d1Band==bandBefore);
                for(ULONG pass=0;pass<2;++pass) for(ULONG i=0;i<96;++i)
                    assert(cpRing[(pass ? 0x7280 : 0x7100)/4+i]==GmuQ1VertexWord(91,pass!=0,i));
            }
            for(ULONG bad=0;bad<9;++bad) {
                seedQ1(); initQ1();
                if(bad==0) cpRing[0]^=1;
                if(bad==1) *GmuCfgReg(0x802)=0x20C;
                if(bad==2) *GmuCfgReg(0x8E07)=0x3C400000;
                if(bad==3) cpRing[0x6880/4]^=1;
                if(bad==4) cpRing[0x6E00/4]=1;
                if(bad==5) cpRing[0x70B4/4]=1;
                if(bad==6) cpRing[0x7100/4]=1;
                if(bad==7) cpRing[0x7FFF/4]=1;
                if(bad==8) *GmuCfgReg(0x38)=1;
                assert(submitQ1()==STATUS_INVALID_DEVICE_STATE && !q1.Cp.Attempted && !(q1.Checks&1) && q1Writes.empty() && d1Maps==0);
                if(bad>=4 && bad<8) assert(q1.PlaceBad==1);
            }
            for(ULONG step:{0x1F00UL,0x1F01UL,0x1F02UL,0x1F08UL,0x1F03UL,0x1F40UL,0x1F41UL,0x1F10UL,0x1F20UL,0x1F30UL,0x1F31UL,0x1F32UL}) {
                seedQ1(); initQ1(); failGmuStep=step;
                assert(submitQ1()==STATUS_IO_DEVICE_ERROR && d1Band==bandBefore && allocations.size()==poolBefore);
                if(step==0x1F41) assert(q1.FramesDone==16 && q1.FailFrame==16 && q1Writes.size()==16);
            }
            for(ULONG bad=0;bad<2;++bad) {
                seedQ1(); initQ1(); q1BadSizeFrame=91; q1ShadowOnly=bad!=0; q1FenceForever=false;
                assert(submitQ1()==STATUS_IO_DEVICE_ERROR && q1.Cp.Reason==18 && q1.FailFrame==91 && q1.WptrWrites==92 && q1.HoldMs==0 && d1Band==bandBefore);
            }
            seedQ1(); initQ1(); q1BadSizeFrame=0; q1FenceForever=true;
            assert(submitQ1()==STATUS_IO_DEVICE_ERROR && q1.Cp.Reason==18 && q1.WptrWrites==1);
            seedQ1(); initQ1(); q1FenceDrops=3;
            assert(submitQ1()==0 && q1.WptrWrites==124 && q1.FramesDone==121);
            seedQ1(); initQ1(); q1FenceForever=true;
            assert(submitQ1()==STATUS_IO_TIMEOUT && q1.Cp.Reason==10 && q1.FramesDone==0 && d1Band==bandBefore);
            seedQ1(); initQ1(); q1StopFrame=91; failGmuStep=0x1F20;
            assert(submitQ1()==STATUS_IO_TIMEOUT && q1.Cp.Reason==16 && d1Band==bandBefore);
            seedQ1(); initQ1(); q1SaveFails=true;
            assert(submitQ1()==STATUS_IO_DEVICE_ERROR && q1.HoldMs==0 && d1Band==bandBefore);
            seedQ1(); initQ1(); d1RestoreDrop=(1130*4320+470*4-0x462000)/4;
            assert(submitQ1()==STATUS_IO_DEVICE_ERROR && q1.RestoreMismatch==1 && !(q1.Checks&32));
            seedQ1(); initQ1(); failAfter=0;
            assert(submitQ1()==STATUS_INSUFFICIENT_RESOURCES && !q1.Cp.Attempted && d1Maps==0 && allocations.size()==poolBefore);
            seedQ1(); initQ1(); d1MapFails=true;
            assert(submitQ1()==STATUS_INSUFFICIENT_RESOURCES && !q1.Cp.Attempted && allocations.size()==poolBefore);
            seedQ1(); initQ1(); failAddrMap=3;
            assert(submitQ1()==STATUS_INSUFFICIENT_RESOURCES && !q1.Cp.Attempted && d1Maps==0);
            seedQ1(); initQ1(); m1.Checks=0x1F; assert(!GmuQ1CanFollow(&m1));
            allowQ1Writes=false; q1Active=false;
            allowM1Writes=false; m1Active=false;
            allowW1Writes=false; w1Active=false;
            t1CopyTrigger=nullptr; t1CorruptTarget=nullptr; allowT1Writes=false;
            allowA1Writes=false;
            allowV1Writes=false;
            allowD1Writes=false; failAfter=-1;
            r1CopyTarget=nullptr; r1CopyBadDword=0xFFFFFFFF; allowR1Writes=false;
            allowS1Writes=false;
            allowG5bWrites=false;
            allowG5Writes=false;
            allowG4Writes=false;
            allowG3cWrites=false; g3cPolling=false; g3cPending=false; g3cMaskChangeStep=0xFFFFFFFF; *GmuCfgReg(0x38)=0;
            allowG3bWrites=false; allowG3Writes=false; allowG2Writes=false; g2Ring=nullptr; g2Shadow=nullptr; probe.Count=0; probe.Smmu=nullptr;
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
    // The first allocation is CPU-only traces. Failure must not touch hardware or diagnostics.
    Reset(); failAfter=0; keep=true;
    assert(Run(f,&keep)==EPI_GMU_E_NOMEM && !keep && gmuWrites==0 && diagnostics.empty() && testMaps.empty() && allocations.empty());
    Reset();
    {
        bool entered=false;
        const auto callout=+[](PVOID p) { *static_cast<bool*>(p)=true; };
        assert(KeExpandKernelStackAndCalloutEx(callout,&entered,MAXIMUM_EXPANSION_SIZE,TRUE,nullptr)==0 && entered);
        entered=false; testExpandStatus=STATUS_INSUFFICIENT_RESOURCES;
        assert(KeExpandKernelStackAndCalloutEx(callout,&entered,MAXIMUM_EXPANSION_SIZE,TRUE,nullptr)==STATUS_INSUFFICIENT_RESOURCES && !entered);
        EpiGmuStackReset(); assert(EpiGmuStackMinimum()==0xFFFFFFFF);
        testRemainingStack=32000; assert(GmuStep(nullptr,1)==0 && EpiGmuStackMinimum()==32000);
        testRemainingStack=24000; assert(GmuStep(nullptr,2)==0 && EpiGmuStackMinimum()==24000);
        testRemainingStack=30000; assert(GmuStep(nullptr,3)==0 && EpiGmuStackMinimum()==24000);
        failGmuStep=4; testRemainingStack=16000;
        assert(GmuStep(nullptr,4)==STATUS_IO_DEVICE_ERROR && EpiGmuStackMinimum()==16000);
    }
    // Fail each region/context allocation; all already acquired buffers must be released.
    for(int i=0;i<11;++i) {
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
        for(int fail=0;fail<14;++fail) {
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
    // Full G-3 path preserves G-2's behavior, makes no additional SMMU mappings and retains data on unsafe teardown.
    {
        const auto sqe=SqeImage();
        Reset(); assert(RunG2(f,sqe,&keep,true)==0 && !keep && allocations.empty());
        assert(testMaps.size()==11 && g3Writes.size()==1 && g2Writes.size()==3 && g2Writes[2].second==20);
        Reset(); g3Mem=false;
        assert(RunG2(f,sqe,&keep,true)==EPI_GMU_E_CP && !keep && allocations.empty() && g3Writes.size()==1);
        Reset(); g3Completes=false;
        assert(RunG2(f,sqe,&keep,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g2Completes=false;
        assert(RunG2(f,sqe,&keep,true)==EPI_GMU_E_CP && g3Writes.empty() && !keep && allocations.empty());
        Reset(); g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu->Smmu->enabled);
        GMU_REGION* retained=GmuFind(g_KeptGmu,GmuG1Iova[1],GmuG1Size[1]);
        assert(retained && retained->Va && static_cast<ULONG*>(retained->Va)[0x7000/4]==0xCAFE0002);
        ClearQuarantine();
        Reset(); smmuDisableFails=true;
        assert(RunG2(f,sqe,&keep,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        for(ULONG i=0;i<3;++i) assert(GmuFind(g_KeptGmu,GmuG1Iova[i],GmuG1Size[i])->Va!=nullptr);
        ClearQuarantine(); Reset();
    }
    // Full G-3b: G-3 must pass first; results and all buffers survive any unsafe teardown.
    {
        const auto sqe=SqeImage();
        Reset(); assert(RunG2(f,sqe,&keep,true,true)==0 && !keep && allocations.empty());
        assert(testMaps.size()==11 && g3Writes.size()==1 && g3bWrites.size()==1 && g2Writes.size()==4 && g2Writes[3].second==25);
        Reset(); g3bEvent=false;
        assert(RunG2(f,sqe,&keep,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g3bCompletes=false;
        assert(RunG2(f,sqe,&keep,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g3Mem=false;
        assert(RunG2(f,sqe,&keep,true,true)==EPI_GMU_E_CP && g3bWrites.empty() && !keep && allocations.empty());
        Reset(); g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu->Smmu->enabled);
        GMU_REGION* retained=GmuFind(g_KeptGmu,GmuG1Iova[1],GmuG1Size[1]);
        assert(retained && retained->Va && static_cast<ULONG*>(retained->Va)[0x7010/4]==0xCAFE0003);
        ClearQuarantine();
        Reset(); smmuDisableFails=true;
        assert(RunG2(f,sqe,&keep,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        for(ULONG i=0;i<3;++i) assert(GmuFind(g_KeptGmu,GmuG1Iova[i],GmuG1Size[i])->Va!=nullptr);
        ClearQuarantine(); Reset();
    }
    // Full G-3c: parent success, unchanged resource mappings, and quarantine on unsafe teardown.
    {
        const auto sqe=SqeImage();
        Reset(); assert(RunG2(f,sqe,&keep,true,true,true)==0 && !keep && allocations.empty());
        assert(testMaps.size()==11 && g3cWrites.size()==1 && g3cClearWrites.size()==1);
        assert(g2Writes.size()==5 && g2Writes[1].second==9 && g2Writes[2].second==20 &&
               g2Writes[3].second==25 && g2Writes[4].second==30);
        Reset(); g3cShowStatus=false; g3cShowUnmasked=false;
        assert(RunG2(f,sqe,&keep,true,true,true)==0 && !keep && allocations.empty());
        Reset(); g3cIgnoreClear=true;
        assert(RunG2(f,sqe,&keep,true,true,true)==0 && !keep && allocations.empty());
        Reset(); g3cEvent=false;
        assert(RunG2(f,sqe,&keep,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g3bEvent=false;
        assert(RunG2(f,sqe,&keep,true,true,true)==EPI_GMU_E_CP && g3cWrites.empty() && g3cClearWrites.empty() && !keep);
        Reset(); g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu->Smmu->enabled);
        GMU_REGION* retained=GmuFind(g_KeptGmu,GmuG1Iova[1],GmuG1Size[1]);
        assert(retained && retained->Va && static_cast<ULONG*>(retained->Va)[0x7018/4]==0xCAFE0004);
        ClearQuarantine();
        Reset(); smmuDisableFails=true;
        assert(RunG2(f,sqe,&keep,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        for(ULONG i=0;i<3;++i) assert(GmuFind(g_KeptGmu,GmuG1Iova[i],GmuG1Size[i])->Va!=nullptr);
        ClearQuarantine(); Reset();
    }
    // Full G-4 keeps the original allocation/SMMU layout and never runs after a failed parent.
    {
        const auto sqe=SqeImage();
        Reset(); assert(RunG2(f,sqe,&keep,true,true,true,true)==0 && !keep && allocations.empty());
        assert(testMaps.size()==11 && g4Writes.size()==1 && g2Writes.size()==6 && g2Writes[5].second==39);
        Reset(); assert(RunG2(f,sqe,&keep,true,true,true)==0 && g4Writes.empty());
        Reset(); g3cEvent=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true)==EPI_GMU_E_CP && g4Writes.empty() && !keep && allocations.empty());
        Reset(); g4Event=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g4Completes=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu->Smmu->enabled);
        GMU_REGION* retained=GmuFind(g_KeptGmu,GmuG1Iova[1],GmuG1Size[1]);
        assert(retained && retained->Va && static_cast<ULONG*>(retained->Va)[0x7024/4]==0xCAFE0007);
        assert(std::memcmp(static_cast<UCHAR*>(retained->Va)+0x6000,GmuG4Ib,sizeof(GmuG4Ib))==0);
        ClearQuarantine();
        Reset(); smmuDisableFails=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        for(ULONG i=0;i<3;++i) assert(GmuFind(g_KeptGmu,GmuG1Iova[i],GmuG1Size[i])->Va!=nullptr);
        ClearQuarantine(); Reset();
    }
    // Full G-5 uses the original GPU allocations, follows G-4, and retains pixels on unsafe teardown.
    {
        const auto sqe=SqeImage();
        Reset(); assert(RunG2(f,sqe,&keep,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(testMaps.size()==11 && g5Writes.size()==1 && g2Writes.size()==7 && g2Writes[6].second==48);
        Reset(); assert(RunG2(f,sqe,&keep,true,true,true,true)==0 && g5Writes.empty());
        Reset(); g4Event=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true)==EPI_GMU_E_CP && g5Writes.empty() && !keep && allocations.empty());
        Reset(); g5Fill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g5Completes=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu->Smmu->enabled);
        GMU_REGION* retained=GmuFind(g_KeptGmu,GmuG1Iova[1],GmuG1Size[1]);
        assert(retained && retained->Va && static_cast<ULONG*>(retained->Va)[0x7400/4]==0x78563412);
        assert(std::memcmp(static_cast<UCHAR*>(retained->Va)+0x6100,GmuG5Ib,sizeof(GmuG5Ib))==0);
        ClearQuarantine();
        Reset(); smmuDisableFails=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        for(ULONG i=0;i<3;++i) assert(GmuFind(g_KeptGmu,GmuG1Iova[i],GmuG1Size[i])->Va!=nullptr);
        ClearQuarantine(); Reset();
    }
    // G-5's data-only failure remains in its own trace, but does not veto a successful G-5b retry.
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(g5bWrites.size()==1 && g2Writes.back().second==61 && testMaps.size()==11 && testGlobalCalls==2);
        Reset(); g5Fill=false; g5bFill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g5Completes=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true)==EPI_GMU_E_CP && g5bWrites.empty() && !keep);
        Reset(); // Already-filled pixels must be rejected rather than attributing old success to new magic registers.
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true)==EPI_GMU_E_CP && g5bWrites.empty() && !keep);
        Reset(); g5Fill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        GMU_REGION* retained=GmuFind(g_KeptGmu,GmuG1Iova[1],GmuG1Size[1]);
        assert(retained && static_cast<ULONG*>(retained->Va)[0x7030/4]==0xCAFE000A);
        ClearQuarantine();
        Reset(); g5Fill=false; smmuDisableFails=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        ClearQuarantine(); Reset();
    }
    // Z-1 replaces only the expected no-pixels G-5b outcome, never an earlier transport failure.
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(z1Writes.size()==1 && g2Writes.back().second==73 && smcInputs.size()==4 && ZapOwner);
        Reset(); g5Fill=false; g5bFill=false; smcSupportedResult=0;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(z1Writes.size()==1 && g2Writes.back().second==73 && smcInputs.size()==4 && z1Authorized);
        Reset(); g5Fill=false; g5bFill=false; smcFailCmd=5;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && z1Writes.empty() && allocations.empty());
        Reset(); g5Fill=false; g5bFill=false; z1Fill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g5Fill=false; g5bFill=false; z1Completes=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g5Fill=false; g5bCompletes=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true)==EPI_GMU_E_CP && smcInputs.empty() && z1Writes.empty());
        Reset(); g5Fill=false; // Successful G-5b drawing is outside Z-1's no-pixels precondition.
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true)==EPI_GMU_E_CP && smcInputs.empty());
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        ClearQuarantine(); Reset();
    }
    // S-1 is published only after successful Z-1c, and uses the same allocations/SMMU mappings.
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false; smcSupportedResult=0;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(s1Writes.size()==1 && g2Writes.back().second==82 && testMaps.size()==11 && smcInputs.size()==4);
        Reset(); g5Fill=false; g5bFill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true)==0 && s1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; smcFailCmd=5;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && s1Writes.empty() && !keep);
        Reset(); g5Fill=false; g5bFill=false; z1Fill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && s1Writes.empty() && !keep);
        Reset(); g5Fill=false; g5bFill=false; s1Store=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g5Fill=false; g5bFill=false; s1Completes=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        GMU_REGION* retained=GmuFind(g_KeptGmu,GmuG1Iova[1],GmuG1Size[1]);
        assert(retained && static_cast<ULONG*>(retained->Va)[0x7C00/4]==0xC0DE5EED);
        assert(std::memcmp(static_cast<UCHAR*>(retained->Va)+0x6800,GmuS1Shader,sizeof(GmuS1Shader))==0);
        ClearQuarantine();
        Reset(); g5Fill=false; g5bFill=false; smmuDisableFails=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        ClearQuarantine(); Reset();
    }
    // Full R-1 preserves old modes, parent gating, and resource quarantine.
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false; smcSupportedResult=0;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(r1Writes.size()==1 && g2Writes.back().second==91 && testMaps.size()==11 && smcInputs.size()==4);
        Reset(); g5Fill=false; g5bFill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true)==0 && r1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; s1Store=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && r1Writes.empty() && !keep);
        Reset(); g5Fill=false; g5bFill=false; r1Draw=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g5Fill=false; g5bFill=false; r1Completes=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && allocations.empty());
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        GMU_REGION* retained=GmuFind(g_KeptGmu,GmuG1Iova[1],GmuG1Size[1]);
        assert(retained && static_cast<ULONG*>(retained->Va)[0x4C00/4+68]==0x000033FF);
        assert(std::memcmp(static_cast<UCHAR*>(retained->Va)+0x4000,GmuR1Ib,sizeof(GmuR1Ib))==0);
        ClearQuarantine();
        Reset(); g5Fill=false; g5bFill=false; smmuDisableFails=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep && g_KeptGmu);
        ClearQuarantine(); Reset();
    }
    // D-1 external physical RAM is never allocated/freed as a GMU buffer.
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false; const auto original=d1Band;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(testMaps.size()==12 && testMaps.back().Iova==0x71000000 && testMaps.back().Pa==0xE1462000);
        assert(testMaps.back().Size==0x120000 && testMaps.back().Flags==(EPI_SMMU_READ|EPI_SMMU_WRITE));
        assert(d1Writes.size()==1 && d1Maps==1 && d1Unmaps==1 && d1Band==original);
        Reset(); g5Fill=false; g5bFill=false; r1Draw=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep);
        assert(d1Writes.empty() && d1Maps==0);
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep);
        GMU_REGION* ext=GmuFind(g_KeptGmu,0x71000000,0x120000);
        assert(ext && ext->External && ext->Mapped && !ext->Va && ext->Pa.QuadPart==0xE1462000);
        assert(d1Band==original && d1Unmaps==1); ClearQuarantine(); Reset();
    }
    // Full V-1 follows D-1 restore under one OOB lease, with no extra SMMU map requests.
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false; const auto original=d1Band;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(v1Writes.size()==1 && testMaps.size()==12 && d1Maps==2 && d1Unmaps==2 && d1Band==original);
        assert(g2Writes.back().second==109);
        Reset(); g5Fill=false; g5bFill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true)==0 && v1Writes.empty() && d1Maps==1);
        Reset(); g5Fill=false; g5bFill=false; d1Odd=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && v1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep);
        assert(d1Band==original && d1Maps==2 && d1Unmaps==2);
        GMU_REGION* ext=GmuFind(g_KeptGmu,0x71000000,0x120000);
        assert(ext && ext->External && ext->Mapped && !ext->Va); ClearQuarantine(); Reset();
    }
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false; const auto original=d1Band;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(a1Frames.size()==241 && a1Writes.back()==2278 && testMaps.size()==12 && d1Maps==3 && d1Unmaps==3 && d1Band==original);
        Reset(); g5Fill=false; g5bFill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true)==0 && a1Frames.empty());
        Reset(); g5Fill=false; g5bFill=false; v1BadAlpha=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && a1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep);
        assert(d1Band==original && d1Maps==3 && d1Unmaps==3); ClearQuarantine(); Reset();
    }
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false; const auto original=d1Band;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(t1Writes.size()==1 && t1Writes.back()==2287 && testMaps.size()==12 && d1Maps==4 && d1Unmaps==4 && d1Band==original);
        Reset(); g5Fill=false; g5bFill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true)==0 && t1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; a1BadSq=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && t1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep);
        assert(d1Band==original && d1Maps==4 && d1Unmaps==4); ClearQuarantine(); Reset();
    }
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false; const auto original=d1Band;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(w1Pubs.size()==650 && w1ReadPackets.size()==1000 && testMaps.size()==12 && d1Maps==5 && d1Unmaps==5 && d1Band==original);
        bool sawSmall=false; for(const auto& w:g1Writes) if(w.first==0x802) {assert(w.second==0x20B); sawSmall=true;} assert(sawSmall);
        Reset(); g5Fill=false; g5bFill=false;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true)==0 && w1Writes.empty());
        for(const auto& w:g1Writes) if(w.first==0x802) assert(w.second==0x20C);
        Reset(); g5Fill=false; g5bFill=false; failG1Dword=0x802;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && g2Writes.empty() && w1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; t1Zero=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && w1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep);
        assert(d1Band==original && d1Maps==5 && d1Unmaps==5); ClearQuarantine(); Reset();
    }
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false; const auto original=d1Band;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(m1Writes.size()==1 && m1Tiles.size()==12 && testMaps.size()==12 && d1Maps==6 && d1Unmaps==6 && d1Band==original);
        Reset(); g5Fill=false; g5bFill=false; w1DamageRing=0;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && m1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep);
        assert(d1Band==original && d1Maps==6 && d1Unmaps==6); ClearQuarantine(); Reset();
    }
    {
        const auto sqe=SqeImage();
        Reset(); g5Fill=false; g5bFill=false; const auto original=d1Band;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==0 && !keep && allocations.empty());
        assert(q1Frames.size()==121 && q1Writes.back()==265 && testMaps.size()==12 && d1Maps==7 && d1Unmaps==7 && d1Band==original);
        Reset(); g5Fill=false; g5bFill=false; m1NoClear=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_CP && !keep && q1Writes.empty());
        Reset(); g5Fill=false; g5bFill=false; g2KeepGxOn=true;
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true)==EPI_GMU_E_TEARDOWN && keep);
        assert(d1Band==original && d1Maps==7 && d1Unmaps==7); ClearQuarantine(); Reset();
        assert(RunG2(f,sqe,&keep,true,true,true,true,true,true,true,true,true,true,true,true,true,true,false,true)==EPI_GMU_E_PRECOND && !keep && gmuWrites==0 && allocations.empty());
    }
    std::puts("GMU tests passed: firmware regions, synthetic/real image load, allocation failures, boot timeout, reset and SMMU teardown quarantine.");
}
