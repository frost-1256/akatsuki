// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - G1c-3: GMU CM3 boot test. New file, not derived from KMDOD.
//
// Clean room: register offsets, the firmware block format, the GMU memory layout and the boot order are hardware /
// firmware facts read from mainline Linux (a6xx_gmu.c, a6xx_gmu.xml, a6xx_hfi.c/.h, a6xx_catalog.c) used as a
// datasheet only; no code taken. Nothing here is derived from qcdxkm. The adreno SMMU is only driven through the
// epi_smmu.hxx API.
//
// Order (inside the EpiGpuProbe CX window; CM3 held in reset on entry, GX off):
//   1. Regions: fixed a660-family GMU buffers (dummy 16 KB @0x60000000, debug 28 KB @0x60400000, log 16 KB
//      @0x60004000, HFI 16 KB @0x60008000), then every size-0 descriptor block of a660_gmu.bin outside the TCMs
//      (icache code @0x4000, zero-initialised regions @0x100000/0x200000/0x300000). Descriptors already covered by a
//      fixed buffer (the two type-7 pages inside dummy) are skipped.
//   2. Allocate each region (physically contiguous, non-cached), zero it, copy the data blocks that fall in it, and
//      build the HFI queue table (table header + 3 queue headers, queues at +4/8/12 KB).
//   3. EpiSmmuCreate + EpiSmmuMap for every region (SMMU still disabled).
//   4. Initialise ten RSCC controls, then GMU setup: CX FAL interfaces, ITCM/DTCM load with read-back,
//      FW_INIT_RESULT 0, BOOT_CONFIG 2, RPMH_CTRL=0xF11,
//      HFI table, AHB fence range, CM3_CFG, chip id, log buffer, bus/cache config, inter-frame control.
//      Bit10 additionally applies four GBIF QSB settings after the log address, with GX still off.
//   5. EpiSmmuEnable, then release the CM3 (SYSRESET 1, CP_RESP 0, SYSRESET 0) and poll FW_INIT_RESULT. This
//      firmware is newer than 0x20010004, so the done value is (v & 0x1FF) == 0x100. Up to 100 ms.
//      Bit11 first votes GCC GPLL0_DIV and switches GMU/hub RCGs to 200/150 MHz while CM3 is reset.
//   6. After successful FW init, start HFI and send PERF_TABLE / BW_TABLE; bit9 adds CORE_FW_START / START.
//      Bit10 adds GX_BW_PERF_VOTE after START and read-only GCC/TCS/CX-busy/GBIF observations.
//      Bit12 then performs OOB GPU_SET/release and PREPARE_SLUMBER before diagnostics/reset.
//      Bit13 reads six GX registers once each after OOB ACK and GX-on checks, strictly before release.
//      Bit14 then clears CX GBIF_HALT and RBBM_GBIF_HALT with immediate posted-write readbacks.
//      Bit15 then clears four SECVID TSB controls under the same OOB lease (stage A only).
//      Bit16 follows successful stage A with twelve ADDR_MODE controls set to 1; bit17 then enables hwcg.
//      Bit18 then applies stage D fixed settings without CP_AHB_CNTL; no CP execution is started.
//      Bit19 adds stage E UBWC / CP protection settings after successful stage D under the same OOB lease.
//      Bit20 then writes CP_AHB_CNTL and reads four known registers, still before OOB release.
//      Bit21 prepares GPU buffers/streams before Enable and programs seven CP bases under OOB; CP remains stopped.
//      Bit22 explicitly starts SQE, publishes ME_INIT, and waits for CP/GMU idle under the same OOB lease.
//      Bit23 appends the scratch/MEM_WRITE/REG_TO_MEM probe and waits for WPTR=20 before releasing OOB.
//      Bit24 appends CACHE_FLUSH_TS without IRQ, waits for WPTR=25 and verifies both new and preserved data.
//      Bit25 probes the IRQ source with MASK=0; never unmasks the interrupt line or changes the ISR.
//      Bit26 calls one IB at ring+0x6000, then a return-to-ring event; WPTR=39, no new memory mappings.
//      Bit27 fills a 16x16 RGBA8 image through CP_BLIT in a second IB; WPTR=48, existing ring memory only.
//      Bit28 applies three magic registers then repeats the fill, records global faults and scans ring memory.
//      Bit29 performs direct PAS zap loading, then SET_SECURE_MODE 0 and repeats the fill under OOB.
//      Bit31 follows successful Z-1c with one compute shader invocation and global store; WPTR=82.
//      EpiGpuProbe2 bit0 follows successful S-1 with a VS/FS RECTLIST draw to a private 16x16 target; WPTR=91.
//      Record ACKs/queue indices,
//      diagnostics, the SMMU fault state and the GMU log; put CM3 back in reset, bit12 rpmh_stop, check RSCC/GX idle.
//   7. EpiSmmuDisable, restore GMU controls, optional GBIF and RSCC, then Unmap / Destroy / free.
//      A failed Disable keeps everything and asks the caller to keep
//      CX on (*KeepCx).
// Not done here: rpmh_start, force_off, SECVID_TRUST_CNTL, GPU PDC (reading it reset the phone on 2026-11-07).
// No PLL programming; bit11 uses the existing GCC GPLL0_DIV source.
// Preserve all RSCC HIDDEN_TCS ADDR and PDC slave/sequence/match values.

#ifdef EPI_GMU_HOST_TEST
#include "gmu_shim.hxx"
#else
#include "BDD.hxx"
#include "epi_hw.hxx"
#include <ntstrsafe.h>
#endif
#include "epi_smmu.hxx"
#include "epi_gmu.hxx"
#include "epi_zap.hxx"

#define EPI_GMU_GPU_BASE        0x03D00000ULL
#define EPI_GMU_PAGE_C_PHYS     0x03D7D000ULL       // dwords 0x1F400..0x1F7FF
#define EPI_GMU_PAGE_A_PHYS     0x03D7E000ULL       // dwords 0x1F800..0x1FBFF
#define EPI_GMU_PAGE_B_PHYS     0x03D8E000ULL       // dwords 0x23800..0x23BFF
#define EPI_GMU_ITCM_PHYS_      (EPI_GMU_GPU_BASE + 0x1B400ULL * 4)
#define EPI_GMU_DTCM_PHYS_      (EPI_GMU_GPU_BASE + 0x1C400ULL * 4)
#define EPI_GMU_TCM_SIZE        0x4000UL
#define EPI_GMU_ITCM_BASE       0x00000000UL
#define EPI_GMU_DTCM_BASE       0x10004000UL        // a650 family
#define EPI_GMU_VER_DTCM_OFF    0x3FE0UL            // CORE_FW_VERSION (dword 0x1D3F8)
#define EPI_GMU_RSCC_PHYS       0x03DE0000ULL
#define EPI_GMU_RSCC_SIZE       0x2000UL
#define EPI_GMU_GBIF_PHYS       (EPI_GMU_GPU_BASE + 0xF000ULL)
#define EPI_GMU_GCC_VOTE_PHYS   0x00152000ULL
static_assert(EPI_GMU_RSCC_SIZE>=0x1500,"RSCC mapping must cover TCS3 status");

// register dwords (from the GPU base)
#define R_ICACHE_CONFIG         0x1F400
#define R_DCACHE_CONFIG         0x1F401
#define R_SYS_BUS_CONFIG        0x1F40F
#define R_CM3_SYSRESET          0x1F800
#define R_CM3_BOOT_CONFIG       0x1F801
#define R_CM3_FW_BUSY           0x1F81A
#define R_CM3_FW_INIT_RESULT    0x1F81C
#define R_CM3_CFG               0x1F82D
#define R_PWR_COL_IFRAME_CTRL   0x1F8C0
#define R_GMU_RPMH_CTRL         0x1F8E8
#define R_GMU_RPMH_POWER_STATE  0x1F8EC
#define R_CX_FAL_INTF           0x1F8F0
#define R_CX_FALNEXT_INTF       0x1F8F1
#define R_PWR_COL_CP_MSG        0x1F900
#define R_PWR_COL_CP_RESP       0x1F901
#define R_HFI_SFR_ADDR          0x1F982
#define R_HFI_CTRL_STATUS       0x1F980
#define R_HFI_QTBL_INFO         0x1F984
#define R_HFI_QTBL_ADDR         0x1F985
#define R_HFI_CTRL_INIT         0x1F986
#define R_GMU2HOST_INTR_CLR     0x1F991
#define R_GMU2HOST_INTR_INFO    0x1F992
#define R_HOST2GMU_INTR_SET     0x1F994
#define R_GENERAL_0             0x1F9C5
#define R_GENERAL_1             0x1F9C6
#define R_GENERAL_7             0x1F9CC
#define R_AO_HOST_INTR          0x23B05
#define R_AHB_FENCE_RANGE_0     0x23B11

// values (a660 family / A635, chip id 0x06030500 in the GMU packing)
#define V_GMU_CHIPID            0x06035000UL
#define V_AHB_FENCE_RANGE_0     (0x80000000UL | (0x0AUL << 18) | 0xA0UL)
#define V_CM3_CFG               0x4052UL
#define V_IFRAME_CTRL           0x09C40400UL
#define V_FW_INIT_MASK          0x1FFUL
#define V_FW_INIT_DONE          0x100UL

// fixed GMU buffers
#define EPI_GMU_DUMMY_IOVA      0x60000000UL
#define EPI_GMU_DUMMY_SIZE      0x4000UL
#define EPI_GMU_DEBUG_IOVA      0x60400000UL
#define EPI_GMU_DEBUG_SIZE      0x7000UL
#define EPI_GMU_LOG_IOVA        0x60004000UL
#define EPI_GMU_LOG_SIZE        0x4000UL
#define EPI_GMU_HFI_IOVA        0x60008000UL
#define EPI_GMU_HFI_SIZE        0x4000UL
#define EPI_GMU_LOG_DUMP        0x2000UL
static_assert(EPI_GMU_LOG_DUMP<=EPI_GMU_LOG_SIZE,"GMU log dump must fit its allocated region");

#define EPI_GMU_MAX_REGIONS     16
#define EPI_GMU_POLL_LOOPS      1000                // x 100 us
#define EPI_GMU_TAG             'gmEE'

typedef struct _EPI_GMU_FBLK { ULONG Addr; ULONG Size; ULONG Type; ULONG Value; } EPI_GMU_FBLK;

struct GMU_REGION {
    ULONG Iova, Size, Flags;
    PVOID Va;
    PHYSICAL_ADDRESS Pa;
    BOOLEAN Mapped;
    BOOLEAN External; // Physical RAM owned by the display subsystem: never allocate, zero, or free it.
};
struct GMU_TEST {
    GMU_REGION Regions[EPI_GMU_MAX_REGIONS];
    ULONG Count;
    EPI_SMMU* Smmu;
    PVOID Maps[8]; // control C/A/B, ITCM, DTCM, RSCC, optional GBIF/GCC vote; retained on failed teardown
    ULONG SmmuCbar[5][3]; // GX snapshot stages: {stage, query NTSTATUS, CBAR0}
};
static GMU_TEST* g_KeptGmu = NULL; // debugger-visible ownership of quarantined allocations
static const ULONG GmuG1Iova[]={0x70000000,0x70010000,0x70020000};
static const ULONG GmuG1Size[]={0xA000,0x8000,0x1000};
static const ULONG GmuG1Flags[]={EPI_SMMU_READ,EPI_SMMU_READ|EPI_SMMU_WRITE,
                                EPI_SMMU_READ|EPI_SMMU_WRITE|EPI_SMMU_PRIVILEGED};
struct GMU_G1_TRACE {
    ULONG Requested;
    NTSTATUS MemoryStatus, RegisterStatus;
    ULONG Reason, Count, Gdscr, GxHm, SqeBytes, SqeFirst, SqeLast;
    ULONG Regions[3][6], Before[7], Value[7], After[7];
};
static_assert(sizeof(GMU_G1_TRACE)==196,"G-1 diagnostic ABI");
static VOID GmuG1TraceInit(GMU_G1_TRACE* t,BOOLEAN ShortRing=FALSE)
{
    RtlFillMemory(t,sizeof(*t),0xFF);
    t->Requested=1; t->MemoryStatus=STATUS_NOT_FOUND; t->RegisterStatus=STATUS_NOT_FOUND;
    t->Reason=1; t->Count=0;
    const ULONG values[]={GmuG1Iova[0],0,GmuG1Iova[1],0,ShortRing ? 0x20BUL : 0x20CUL,GmuG1Iova[2],0};
    for (ULONG i=0;i<7;++i) { t->Value[i]=values[i]; }
    for (ULONG i=0;i<3;++i)
    { t->Regions[i][0]=GmuG1Iova[i]; t->Regions[i][1]=GmuG1Size[i]; t->Regions[i][2]=GmuG1Flags[i]; t->Regions[i][5]=0; }

}

static SIZE_T GmuMapLength(ULONG Index)
{
    return Index==5 ? EPI_GMU_RSCC_SIZE : ((Index==3 || Index==4) ? EPI_GMU_TCM_SIZE : PAGE_SIZE);
}

static BOOLEAN GmuRange(ULONG Addr, ULONG Size, ULONG Base, ULONG Length)
{
    return Size != 0 && Addr >= Base && Size <= Length && Addr-Base <= Length-Size;
}
static GMU_REGION* GmuFind(GMU_TEST* S, ULONG Addr, ULONG Size)
{
    for (ULONG i = 0; i < S->Count; ++i)
    { if (GmuRange(Addr, Size, S->Regions[i].Iova, S->Regions[i].Size)) { return &S->Regions[i]; } }
    return NULL;
}
static BOOLEAN GmuAdd(GMU_TEST* S, ULONG Addr, ULONG Size, ULONG Flags)
{
    if (Size == 0 || Size > 0x40000 || (Addr & 0xFFF) != 0 || S->Count == EPI_GMU_MAX_REGIONS)
    { return FALSE; }
    const ULONG rounded = (Size+0xFFF) & ~0xFFFUL;
    if (static_cast<ULONGLONG>(Addr)+rounded > (1ULL<<32)) { return FALSE; }
    if (GmuFind(S, Addr, rounded) != NULL) { return TRUE; }
    for (ULONG i = 0; i < S->Count; ++i)
    {
        const GMU_REGION* r = &S->Regions[i];
        if (static_cast<ULONGLONG>(Addr) < static_cast<ULONGLONG>(r->Iova)+r->Size &&
            static_cast<ULONGLONG>(r->Iova) < static_cast<ULONGLONG>(Addr)+rounded) { return FALSE; }
    }
    GMU_REGION* r = &S->Regions[S->Count++];
    r->Iova = Addr; r->Size = rounded; r->Flags = Flags; r->External=FALSE;
    return TRUE;
}
static BOOLEAN GmuD1Plan(GMU_TEST* S)
{
    if (S->Count==EPI_GMU_MAX_REGIONS) { return FALSE; }
    for (ULONG i=0;i<S->Count;++i)
    {
        const GMU_REGION* r=&S->Regions[i];
        if (0x71000000ULL<static_cast<ULONGLONG>(r->Iova)+r->Size && r->Iova<0x71120000ULL) { return FALSE; }
    }
    GMU_REGION* r=&S->Regions[S->Count++];
    r->Iova=0x71000000; r->Size=0x120000; r->Flags=EPI_SMMU_READ|EPI_SMMU_WRITE;
    r->Va=NULL; r->Pa.QuadPart=0xE1462000; r->Mapped=FALSE; r->External=TRUE;
    return TRUE;
}
// No MMIO or allocation. Validate the complete stream before using any data block.
static BOOLEAN GmuPlan(GMU_TEST* S, const UCHAR* Fw, ULONG Length)
{
    const ULONG priv = EPI_SMMU_READ|EPI_SMMU_WRITE|EPI_SMMU_PRIVILEGED;
    if (Fw == NULL || Length == 0 || Length > 0x40000) { return FALSE; }
    if (!GmuAdd(S, EPI_GMU_DUMMY_IOVA, EPI_GMU_DUMMY_SIZE, priv) ||
        !GmuAdd(S, EPI_GMU_DEBUG_IOVA, EPI_GMU_DEBUG_SIZE, priv) ||
        !GmuAdd(S, EPI_GMU_LOG_IOVA, EPI_GMU_LOG_SIZE, EPI_SMMU_READ|EPI_SMMU_WRITE) ||
        !GmuAdd(S, EPI_GMU_HFI_IOVA, EPI_GMU_HFI_SIZE, EPI_SMMU_READ|EPI_SMMU_WRITE)) { return FALSE; }
    for (ULONG off = 0; off < Length;)
    {
        if (Length-off < sizeof(EPI_GMU_FBLK)) { return FALSE; }
        EPI_GMU_FBLK b; RtlCopyMemory(&b, Fw+off, sizeof(b));
        if (b.Size > Length-off-sizeof(b) || ((b.Addr|b.Size) & 3) != 0 ||
            static_cast<ULONGLONG>(b.Addr)+b.Size > (1ULL<<32)) { return FALSE; }
        if (b.Size == 0)
        {
            if (b.Value == 0 || (b.Type != 1 && b.Type != 7)) { return FALSE; }
            const BOOLEAN tcm = GmuRange(b.Addr,b.Value,EPI_GMU_ITCM_BASE,EPI_GMU_TCM_SIZE) ||
                                GmuRange(b.Addr,b.Value,EPI_GMU_DTCM_BASE,EPI_GMU_TCM_SIZE);
            if (!tcm && !GmuAdd(S,b.Addr,b.Value,priv)) { return FALSE; }
        }
        off += sizeof(b)+b.Size;
    }
    BOOLEAN code = FALSE, vector = FALSE, version = FALSE;
    for (ULONG off = 0; off < Length;)
    {
        EPI_GMU_FBLK b; RtlCopyMemory(&b,Fw+off,sizeof(b));
        if (b.Size != 0)
        {
            BOOLEAN tcm = GmuRange(b.Addr,b.Size,EPI_GMU_ITCM_BASE,EPI_GMU_TCM_SIZE) ||
                          GmuRange(b.Addr,b.Size,EPI_GMU_DTCM_BASE,EPI_GMU_TCM_SIZE);
            if (!tcm && GmuFind(S,b.Addr,b.Size) == NULL) { return FALSE; }
            if (b.Addr == 0 && b.Size >= 8) { vector = TRUE; }
            if (b.Addr == 0x4000) { code = TRUE; }
            const ULONG verAddr = EPI_GMU_DTCM_BASE+EPI_GMU_VER_DTCM_OFF;
            if (GmuRange(verAddr,4,b.Addr,b.Size))
            {
                ULONG v; RtlCopyMemory(&v,Fw+off+sizeof(b)+verAddr-b.Addr,4);
                version = v == 0x30010008; // this probe is for the measured v3.1.8 image
            }
        }
        off += sizeof(b)+b.Size;
    }
    return code && vector && version;
}
static VOID GmuQueues(GMU_TEST* S)
{
    GMU_REGION* r = GmuFind(S,EPI_GMU_HFI_IOVA,EPI_GMU_HFI_SIZE);
    ULONG* h = static_cast<ULONG*>(r->Va);
    // Wire layout: 6-word table header, then three 12-word queue headers.
    // Table size is 168 BYTES (the firmware ABI), offsets/header sizes are DWORDs.
    h[0]=0; h[1]=168; h[2]=6; h[3]=12; h[4]=3; h[5]=3;
    for (ULONG i = 0; i < 3; ++i)
    {
        ULONG* q = h+6+i*12;
        q[0]=1; q[1]=r->Iova+(i+1)*0x1000; q[2]=0xA00|i; q[3]=0x400;
        q[6]=1; q[7]=1; q[8]=1;
    }
    KeMemoryBarrier();
}
static NTSTATUS GmuG1Plan(GMU_TEST* S,const UCHAR* Sqe,ULONG Length)
{
    if (Sqe==NULL || Length!=EPI_GMU_SQE_FILE_SIZE || S->Count>EPI_GMU_MAX_REGIONS-3)
    { return STATUS_INVALID_PARAMETER; }
    ULONG header[2]; RtlCopyMemory(header,Sqe,sizeof(header));
    if (header[0]!=0 || header[1]!=0x016DC094) { return STATUS_INVALID_PARAMETER; }
    // Refuse even an exact/contained overlap: GPU allocations must never alias firmware regions.
    for (ULONG i=0;i<3;++i) for (ULONG j=0;j<S->Count;++j)
    {
        const GMU_REGION* r=&S->Regions[j];
        if (static_cast<ULONGLONG>(GmuG1Iova[i])<static_cast<ULONGLONG>(r->Iova)+r->Size &&
            static_cast<ULONGLONG>(r->Iova)<static_cast<ULONGLONG>(GmuG1Iova[i])+GmuG1Size[i])
        { return STATUS_CONFLICTING_ADDRESSES; }
    }
    for (ULONG i=0;i<3;++i)
    { if (!GmuAdd(S,GmuG1Iova[i],GmuG1Size[i],GmuG1Flags[i])) { return STATUS_CONFLICTING_ADDRESSES; } }
    return STATUS_SUCCESS;
}
static VOID GmuG1Diag(GMU_TEST* S,HANDLE Diag,GMU_G1_TRACE* T)
{
    for (ULONG i=0;i<3;++i)
    {
        const GMU_REGION* r=GmuFind(S,GmuG1Iova[i],GmuG1Size[i]);
        if (r==NULL) { continue; }
        T->Regions[i][3]=r->Va!=NULL ? static_cast<ULONG>(r->Pa.QuadPart) : 0xFFFFFFFF;
        T->Regions[i][4]=r->Va!=NULL ? static_cast<ULONG>(static_cast<ULONGLONG>(r->Pa.QuadPart)>>32) : 0xFFFFFFFF;
        T->Regions[i][5]=r->Mapped ? 1 : 0;
    }
    EpiSetValue(Diag,L"GpuGmuG1",REG_BINARY,T,sizeof(*T));
    EPI_SMMU_GPU_STREAMS_TRACE streams;
    if (S->Smmu!=NULL && NT_SUCCESS(EpiSmmuQueryGpuStreams(S->Smmu,&streams)))
    { EpiSetValue(Diag,L"GpuSmmuGpuStreams",REG_BINARY,&streams,sizeof(streams)); }
}
static volatile ULONG* GmuReg(GMU_TEST* S, ULONG Dword)
{
    ULONG page = 0, first = 0x1F400;
    if (Dword >= 0x23800) { page=2; first=0x23800; }
    else if (Dword >= 0x1F800) { page=1; first=0x1F800; }
    NT_ASSERT(Dword >= first && Dword-first < 0x400);
    return static_cast<volatile ULONG*>(S->Maps[page])+Dword-first;
}
// Probe execution is serialized by the caller; use atomic updates so sampling never raises the minimum.
static volatile LONG g_GmuStackMin=0x7FFFFFFF;
VOID EpiGmuStackReset()
{
    InterlockedExchange(&g_GmuStackMin,0x7FFFFFFF);
}
ULONG EpiGmuStackMinimum()
{
    const LONG value=InterlockedCompareExchange(&g_GmuStackMin,0,0);
    return value==0x7FFFFFFF ? 0xFFFFFFFFUL : static_cast<ULONG>(value);
}
static VOID GmuStackSample()
{
    const LONG remaining=static_cast<LONG>(IoGetRemainingStackSize());
    LONG previous=InterlockedCompareExchange(&g_GmuStackMin,0,0);
    while (remaining<previous)
    {
        const LONG observed=InterlockedCompareExchange(&g_GmuStackMin,remaining,previous);
        if (observed==previous) { break; }
        previous=observed;
    }
}
static NTSTATUS GmuStep(HANDLE Params, ULONG Step)
{
    GmuStackSample();
    NTSTATUS st=EpiSetDword(Params,L"EpiGpuProbeStep",Step);
    return NT_SUCCESS(st) ? ZwFlushKey(Params) : st;
}
static NTSTATUS GmuG1Copy(GMU_TEST* S,HANDLE Params,const UCHAR* Sqe,GMU_G1_TRACE* T)
{
    const NTSTATUS st=GmuStep(Params,0xC01); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* r=GmuFind(S,GmuG1Iova[0],GmuG1Size[0]);
    if (r==NULL || r->Va==NULL) { return STATUS_INVALID_DEVICE_STATE; }
    RtlCopyMemory(r->Va,Sqe+4,EPI_GMU_SQE_CODE_SIZE);
    KeMemoryBarrier();
    T->SqeBytes=EPI_GMU_SQE_CODE_SIZE;
    RtlCopyMemory(&T->SqeFirst,r->Va,4);
    RtlCopyMemory(&T->SqeLast,static_cast<const UCHAR*>(r->Va)+EPI_GMU_SQE_CODE_SIZE-4,4);
    return STATUS_SUCCESS;
}
static NTSTATUS GmuWrite(GMU_TEST* S, HANDLE Params, ULONG Dword, ULONG Value, ULONG Step,
                         ULONG CmpMask = 0xFFFFFFFFUL, ULONG* Readback = NULL)
{
    NTSTATUS st=GmuStep(Params,Step); if (!NT_SUCCESS(st)) { return st; }
    WRITE_REGISTER_ULONG(GmuReg(S,Dword),Value);
    const ULONG readback=READ_REGISTER_ULONG(GmuReg(S,Dword));
    if (Readback != NULL) { *Readback=readback; }
    return (readback & CmpMask) == (Value & CmpMask) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}
struct GMU_GBIF_TRACE {
    struct SETTING { ULONG Reg, Original, Value, Readback, Restored; } Entries[4];
    BOOLEAN Attempted;
    ULONG Written, BadIndex, RestoreBadIndex;
    NTSTATUS Status, RestoreStatus;
};
static VOID GmuGbifDiag(HANDLE Diag, const GMU_GBIF_TRACE* T)
{
    EpiSetValue(Diag,L"GpuGmuGbifSetup",REG_BINARY,T->Entries,sizeof(T->Entries));
    EpiSetDword(Diag,L"GpuGmuGbifStatus",static_cast<ULONG>(T->Status));
    EpiSetDword(Diag,L"GpuGmuGbifRestoreStatus",static_cast<ULONG>(T->RestoreStatus));
    EpiSetDword(Diag,L"GpuGmuGbifBadIndex",T->BadIndex);
    EpiSetDword(Diag,L"GpuGmuGbifRestoreBadIndex",T->RestoreBadIndex);
}
static NTSTATUS GmuGbifSetup(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc, GMU_GBIF_TRACE* T)
{
    T->Attempted=TRUE;
    if ((READ_REGISTER_ULONG(Cc+0x100C/4)&0x80000001UL)!=1 ||
        (READ_REGISTER_ULONG(GmuReg(S,0x1F8D0))&0xC0)!=0xC0 ||
        READ_REGISTER_ULONG(GmuReg(S,R_CM3_SYSRESET))!=1) { return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* g=static_cast<volatile ULONG*>(S->Maps[6]);
    // Read all originals and validate all four before the first write.
    for (ULONG i=0;i<RTL_NUMBER_OF(T->Entries);++i)
    {
        const NTSTATUS st=GmuStep(Params,0x6A0+i);
        if (!NT_SUCCESS(st)) { T->BadIndex=i; return st; }
        T->Entries[i].Original=READ_REGISTER_ULONG(g+3+i);
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(T->Entries);++i)
    {
        const ULONG v=T->Entries[i].Original;
        if (v==0xFFFFFFFFUL || (v&0xFFFF0000UL)==0xDEAD0000UL)
        { T->BadIndex=i; return STATUS_IO_DEVICE_ERROR; }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(T->Entries);++i)
    {
        const NTSTATUS st=GmuStep(Params,0x6C0+i);
        if (!NT_SUCCESS(st)) { T->BadIndex=i; return st; }
        T->Written=i+1;
        WRITE_REGISTER_ULONG(g+3+i,T->Entries[i].Value);
        T->Entries[i].Readback=READ_REGISTER_ULONG(g+3+i);
        if (T->Entries[i].Readback!=T->Entries[i].Value)
        { T->BadIndex=i; return STATUS_IO_DEVICE_ERROR; }
    }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuGbifRestore(GMU_TEST* S, HANDLE Params, GMU_GBIF_TRACE* T)
{
    NTSTATUS result=STATUS_SUCCESS;
    volatile ULONG* g=static_cast<volatile ULONG*>(S->Maps[6]);
    for (ULONG i=T->Written;i-- > 0;)
    {
        NTSTATUS st=GmuStep(Params,0x6D0+i);
        // Restoration remains mandatory even if breadcrumb persistence fails.
        WRITE_REGISTER_ULONG(g+3+i,T->Entries[i].Original);
        T->Entries[i].Restored=READ_REGISTER_ULONG(g+3+i);
        if (T->Entries[i].Restored!=T->Entries[i].Original) { st=STATUS_IO_DEVICE_ERROR; }
        if (!NT_SUCCESS(st) && NT_SUCCESS(result)) { result=st; T->RestoreBadIndex=i; }
    }
    return result;
}
struct GMU_CLOCK_RCG {
    ULONG CmdOffset, CfgOffset, OriginalCmd, OriginalCfg, TargetCfg;
    ULONG SetCmd, SetCfg, SetPolls, RestoreCmd, RestoreCfg, RestorePolls, PendingPolls;
};
static_assert(sizeof(GMU_CLOCK_RCG)==48,"clock RCG diagnostic ABI");
struct GMU_CLOCK_TRACE {
    BOOLEAN Attempted, Saved, Changed[2];
    NTSTATUS Status, RestoreStatus;
    // Before enable, after enable, before disable, after disable, other-bit XORs (enable/disable),
    // vote added by us, enable write issued, disable write issued.
    ULONG Gcc[9];
    GMU_CLOCK_RCG Rcgs[2]; // GMU, hub
    ULONG Branches[3][3]; // before setup / after setup / after restore; cx_gmu, hub_cx_int, hub_aon
};
static VOID GmuClockDiag(HANDLE Diag, const GMU_CLOCK_TRACE* T)
{
    EpiSetDword(Diag,L"GpuGmuClockStatus",static_cast<ULONG>(T->Status));
    EpiSetDword(Diag,L"GpuGmuClockRestoreStatus",static_cast<ULONG>(T->RestoreStatus));
    EpiSetValue(Diag,L"GpuGmuClockGcc",REG_BINARY,T->Gcc,sizeof(T->Gcc));
    EpiSetValue(Diag,L"GpuGmuClockRcgs",REG_BINARY,T->Rcgs,sizeof(T->Rcgs));
    EpiSetValue(Diag,L"GpuGmuClockBranches",REG_BINARY,T->Branches,sizeof(T->Branches));
}
static VOID GmuClockBranches(volatile ULONG* Cc, GMU_CLOCK_TRACE* T, ULONG Stage)
{
    const ULONG offsets[]={0x1098,0x1204,0x1178};
    for (ULONG i=0;i<RTL_NUMBER_OF(offsets);++i)
    { T->Branches[Stage][i]=READ_REGISTER_ULONG(Cc+offsets[i]/4); }
}
static NTSTATUS GmuClockWait(volatile ULONG* Cmd, ULONG* Polls, ULONG* Last)
{
    for (ULONG i=0;i<=50;++i)
    {
        *Polls=i; *Last=READ_REGISTER_ULONG(Cmd);
        if (!(*Last&1)) { return STATUS_SUCCESS; }
        if (i!=50) { KeStallExecutionProcessor(10); }
    }
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS GmuClockApply(HANDLE Params, volatile ULONG* Cc, GMU_CLOCK_TRACE* T,
                              ULONG Index, BOOLEAN Restore, ULONG Step)
{
    GMU_CLOCK_RCG* r=&T->Rcgs[Index];
    volatile ULONG* cmd=Cc+r->CmdOffset/4;
    volatile ULONG* cfg=Cc+r->CfgOffset/4;
    NTSTATUS st=GmuStep(Params,Step); if (!NT_SUCCESS(st)) { return st; }
    if (Restore)
    {
        // Do not rewrite a pending RCG transaction after an UPDATE timeout.
        st=GmuClockWait(cmd,&r->PendingPolls,&r->RestoreCmd);
        r->RestoreCfg=READ_REGISTER_ULONG(cfg);
        if (!NT_SUCCESS(st)) { return st; }
    }
    const ULONG value=Restore ? r->OriginalCfg : r->TargetCfg;
    if (!Restore) { T->Changed[Index]=TRUE; } // includes a shadow CFG write even if the later breadcrumb fails
    WRITE_REGISTER_ULONG(cfg,value);
    st=GmuStep(Params,Step+1); if (!NT_SUCCESS(st)) { return st; }
    WRITE_REGISTER_ULONG(cmd,READ_REGISTER_ULONG(cmd)|1UL);
    // Poll immediately: a registry flush between UPDATE and polling would distort the 500-us wait/count.
    ULONG last=0;
    st=GmuClockWait(cmd,Restore ? &r->RestorePolls : &r->SetPolls,&last);
    const ULONG readback=READ_REGISTER_ULONG(cfg);
    if (Restore) { r->RestoreCmd=last; r->RestoreCfg=readback; }
    else { r->SetCmd=last; r->SetCfg=readback; }
    if (!NT_SUCCESS(st)) { return st; }
    st=GmuStep(Params,Step+2); if (!NT_SUCCESS(st)) { return st; }
    return !(last&0x80000000UL) && readback==value ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}
static NTSTATUS GmuClockSetup(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc, GMU_CLOCK_TRACE* T)
{
    T->Attempted=TRUE;
    NTSTATUS st=GmuStep(Params,0x6E0); if (!NT_SUCCESS(st)) { return st; }
    GmuClockBranches(Cc,T,0);
    if (READ_REGISTER_ULONG(GmuReg(S,R_CM3_SYSRESET))!=1 ||
        (READ_REGISTER_ULONG(Cc+0x100C/4)&0x80000001UL)!=1 ||
        (READ_REGISTER_ULONG(GmuReg(S,0x1F8D0))&0xC0)!=0xC0 ||
        (T->Branches[0][0]&0x80000000UL)!=0) { return STATUS_INVALID_DEVICE_STATE; }
    for (ULONG i=0;i<RTL_NUMBER_OF(T->Rcgs);++i)
    {
        T->Rcgs[i].OriginalCmd=READ_REGISTER_ULONG(Cc+T->Rcgs[i].CmdOffset/4);
        T->Rcgs[i].OriginalCfg=READ_REGISTER_ULONG(Cc+T->Rcgs[i].CfgOffset/4);
    }
    // This opt-in test owns an XO -> GPLL0_DIV -> XO transition only.
    for (ULONG i=0;i<RTL_NUMBER_OF(T->Rcgs);++i)
    { if ((T->Rcgs[i].OriginalCmd&1) || T->Rcgs[i].OriginalCfg!=0) { return STATUS_INVALID_DEVICE_STATE; } }
    T->Saved=TRUE;
    st=GmuStep(Params,0x6E1); if (!NT_SUCCESS(st)) { return st; }
    volatile ULONG* vote=static_cast<volatile ULONG*>(S->Maps[7]);
    T->Gcc[0]=READ_REGISTER_ULONG(vote); // fresh read after flushing; never replay an earlier shared-register snapshot
    if (!(T->Gcc[0]&0x10000UL))
    {
        T->Gcc[6]=1; T->Gcc[7]=1;
        WRITE_REGISTER_ULONG(vote,T->Gcc[0]|0x10000UL);
        T->Gcc[1]=READ_REGISTER_ULONG(vote);
        T->Gcc[4]=(T->Gcc[1]^T->Gcc[0])&~0x10000UL;
        if (!(T->Gcc[1]&0x10000UL)) { return STATUS_IO_DEVICE_ERROR; }
        KeStallExecutionProcessor(10); // BRANCH_HALT_DELAY, no halt-status register
    }
    else { T->Gcc[1]=T->Gcc[0]; T->Gcc[4]=0; }
    st=GmuClockApply(Params,Cc,T,0,FALSE,0x6E2); if (!NT_SUCCESS(st)) { return st; }
    st=GmuClockApply(Params,Cc,T,1,FALSE,0x6E5); if (!NT_SUCCESS(st)) { return st; }
    st=GmuStep(Params,0x6E8); if (!NT_SUCCESS(st)) { return st; }
    GmuClockBranches(Cc,T,1);
    for (ULONG i=0;i<RTL_NUMBER_OF(T->Rcgs);++i)
    {
        GMU_CLOCK_RCG* r=&T->Rcgs[i];
        r->SetCmd=READ_REGISTER_ULONG(Cc+r->CmdOffset/4); r->SetCfg=READ_REGISTER_ULONG(Cc+r->CfgOffset/4);
        if ((r->SetCmd&0x80000001UL)!=0 || r->SetCfg!=r->TargetCfg) { return STATUS_IO_DEVICE_ERROR; }
    }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuClockRestore(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc, GMU_CLOCK_TRACE* T)
{
    NTSTATUS result=STATUS_SUCCESS;
    // Reverse order. Keep the source vote if either RCG cannot be restored.
    for (ULONG i=2;i-- > 0;)
    {
        if (!T->Changed[i]) { continue; }
        const NTSTATUS st=GmuClockApply(Params,Cc,T,i,TRUE,i==1 ? 0x6F0 : 0x6F3);
        if (!NT_SUCCESS(st) && NT_SUCCESS(result)) { result=st; }
    }
    GmuClockBranches(Cc,T,2);
    if (!NT_SUCCESS(result)) { return result; }
    NTSTATUS st=GmuStep(Params,0x6F6); if (!NT_SUCCESS(st)) { return st; }
    for (ULONG i=0;i<RTL_NUMBER_OF(T->Rcgs);++i)
    {
        // Fresh verification of XO and UPDATE completion before touching the shared source vote.
        if (READ_REGISTER_ULONG(Cc+T->Rcgs[i].CfgOffset/4)!=0 ||
            (READ_REGISTER_ULONG(Cc+T->Rcgs[i].CmdOffset/4)&1)!=0) { return STATUS_IO_DEVICE_ERROR; }
    }
    volatile ULONG* vote=static_cast<volatile ULONG*>(S->Maps[7]);
    T->Gcc[2]=READ_REGISTER_ULONG(vote);
    T->Gcc[3]=T->Gcc[2]; T->Gcc[5]=0;
    if (T->Gcc[6] && (T->Gcc[2]&0x10000UL))
    {
        T->Gcc[8]=1;
        WRITE_REGISTER_ULONG(vote,T->Gcc[2]&~0x10000UL);
        T->Gcc[3]=READ_REGISTER_ULONG(vote);
        T->Gcc[5]=(T->Gcc[3]^T->Gcc[2])&~0x10000UL;
        if (T->Gcc[3]&0x10000UL) { return STATUS_IO_DEVICE_ERROR; }
    }
    return STATUS_SUCCESS;
}
static VOID GmuText(PWCHAR Text, SIZE_T Cch, PCWSTR Name, ULONG Value)
{
    WCHAR line[72];
    RtlStringCchPrintfW(line,RTL_NUMBER_OF(line),L"%s=%08X; ",Name,Value);
    RtlStringCchCatW(Text,Cch,line);
}
static VOID GmuSmmuDisableDiag(EPI_SMMU* Context, HANDLE Diag)
{
    EPI_SMMU_DISABLE_WRITE writes[EPI_SMMU_DISABLE_WRITE_MAX]={};
    ULONG count=0;
    if (NT_SUCCESS(EpiSmmuQueryDisableWrites(Context,writes,EPI_SMMU_DISABLE_WRITE_MAX,&count)))
    { EpiSetValue(Diag,L"GpuSmmuDisableWrites",REG_BINARY,writes,static_cast<ULONG>(count*sizeof(writes[0]))); }
}

// Non-legacy HFI wire facts: G0_GPU_SURVEY.md section 9.11 and mainline a6xx_hfi.h.
// Volatile accesses + barriers publish non-cached shared RAM; command/W1C registers are not compared.
struct GMU_HFI_MESSAGE {
    ULONG Header;
    NTSTATUS Status;
    ULONG AckDwords, Ack[19], Skipped, FwErrors, LastFwError[4];
    ULONG LastSkippedDwords, LastSkipped[8];
};
static_assert(sizeof(GMU_HFI_MESSAGE)==148,"HFI message diagnostic ABI");
struct GMU_HFI_TRACE {
    BOOLEAN Attempted, BwPublished, ExtraEnabled;
    NTSTATUS Status;
    NTSTATUS ExtraStatus;
    ULONG CtrlStatus;
    GMU_HFI_MESSAGE Messages[6];
    ULONG Queues[9][7]; // stage, then {read_index, write_index} for command/response/debug
    ULONG Power[4][6];  // stage, RPMh power, GX GDSCR, GX HM, HFI status, interrupt info
    ULONG Rscc[4][5];   // same stages as Power: stage, status0, seq_busy, control_req, control_ack
    ULONG Extra[5][12]; // Power stages 0..3, plus stage 4 immediately before GX vote
};
static NTSTATUS GmuHfiExtra(GMU_TEST* S, HANDLE Params, GMU_HFI_TRACE* T, ULONG Stage)
{
    volatile ULONG* r=static_cast<volatile ULONG*>(S->Maps[5]);
    volatile ULONG* g=static_cast<volatile ULONG*>(S->Maps[6]);
    volatile ULONG* regs[]={static_cast<volatile ULONG*>(S->Maps[7]),
        r+0xD18/4,r+0xFB8/4,r+0x1258/4,r+0x14F8/4,
        GmuReg(S,0x23B0C),GmuReg(S,0x23B0D),g+3,g+4,g+5,g+6};
    for (ULONG i=0;i<RTL_NUMBER_OF(regs);++i)
    {
        const NTSTATUS st=GmuStep(Params,0x770+Stage*0x10+i);
        if (!NT_SUCCESS(st))
        {
            if (T->ExtraStatus==STATUS_NOT_FOUND || NT_SUCCESS(T->ExtraStatus)) { T->ExtraStatus=st; }
            return st;
        }
        T->Extra[Stage][i+1]=READ_REGISTER_ULONG(regs[i]);
    }
    if (T->ExtraStatus==STATUS_NOT_FOUND) { T->ExtraStatus=STATUS_SUCCESS; }
    return STATUS_SUCCESS;
}
static volatile ULONG* GmuHfiHeader(GMU_TEST* S, ULONG Queue)
{
    return static_cast<volatile ULONG*>(GmuFind(S,EPI_GMU_HFI_IOVA,EPI_GMU_HFI_SIZE)->Va)+6+Queue*12;
}
static volatile ULONG* GmuHfiData(GMU_TEST* S, ULONG Queue)
{
    return static_cast<volatile ULONG*>(GmuFind(S,EPI_GMU_HFI_IOVA,EPI_GMU_HFI_SIZE)->Va)+(Queue+1)*0x400;
}
static VOID GmuHfiQueues(GMU_TEST* S, GMU_HFI_TRACE* T, ULONG Stage)
{
    KeMemoryBarrier();
    for (ULONG q=0;q<3;++q)
    {
        volatile ULONG* h=GmuHfiHeader(S,q);
        T->Queues[Stage][1+q*2]=h[10]; T->Queues[Stage][2+q*2]=h[11];
    }
}
static VOID GmuHfiPower(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc, GMU_HFI_TRACE* T, ULONG Stage)
{
    T->Power[Stage][1]=READ_REGISTER_ULONG(GmuReg(S,R_GMU_RPMH_POWER_STATE));
    T->Power[Stage][2]=READ_REGISTER_ULONG(Cc+0x100C/4);
    T->Power[Stage][3]=READ_REGISTER_ULONG(GmuReg(S,0x1F8D0));
    T->Power[Stage][4]=READ_REGISTER_ULONG(GmuReg(S,R_HFI_CTRL_STATUS));
    T->Power[Stage][5]=READ_REGISTER_ULONG(GmuReg(S,R_GMU2HOST_INTR_INFO));
    T->Rscc[Stage][1]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(S->Maps[5])+0x004);
    T->Rscc[Stage][2]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(S->Maps[5])+0x101);
    T->Rscc[Stage][3]=READ_REGISTER_ULONG(GmuReg(S,0x23B07));
    T->Rscc[Stage][4]=READ_REGISTER_ULONG(GmuReg(S,0x23B08));
    if (T->ExtraEnabled) { GmuHfiExtra(S,Params,T,Stage); }
}
static BOOLEAN GmuHfiIdle(GMU_TEST* S, volatile ULONG* Cc, BOOLEAN RpmhStopIssued = FALSE)
{
    // Reset/GX off and a cleared request remain mandatory in both awake and sleeping RSCC states.
    if (READ_REGISTER_ULONG(GmuReg(S,R_CM3_SYSRESET))!=1 ||
        READ_REGISTER_ULONG(GmuReg(S,0x23B07))!=0 ||
        (READ_REGISTER_ULONG(Cc+0x100C/4)&0x80000001UL)!=1 ||
        (READ_REGISTER_ULONG(GmuReg(S,0x1F8D0))&0xC0)!=0xC0) { return FALSE; }
    const ULONG busy=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(S->Maps[5])+0x101);
    if (RpmhStopIssued && busy==1 &&
        (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(S->Maps[5])+0x004)&(1UL<<16)))
    { return TRUE; } // sleep completion: unlike rpmh_start, rpmh_stop does not wait for SEQ_BUSY=0
    return busy==0 && READ_REGISTER_ULONG(GmuReg(S,0x23B08))==0;
}
static VOID GmuHfiDiag(HANDLE Diag, const GMU_HFI_TRACE* T)
{
    EpiSetDword(Diag,L"GpuGmuHfiStatus",static_cast<ULONG>(T->Status));
    EpiSetDword(Diag,L"GpuGmuHfiCtrlStatus",T->CtrlStatus);
    EpiSetDword(Diag,L"GpuGmuHfiBwPublished",T->BwPublished ? 1 : 0);
    EpiSetValue(Diag,L"GpuGmuHfiMessages",REG_BINARY,T->Messages,sizeof(T->Messages));
    EpiSetValue(Diag,L"GpuGmuHfiQueues",REG_BINARY,T->Queues,sizeof(T->Queues));
    EpiSetValue(Diag,L"GpuGmuHfiPower",REG_BINARY,T->Power,sizeof(T->Power));
    EpiSetValue(Diag,L"GpuGmuHfiRscc",REG_BINARY,T->Rscc,sizeof(T->Rscc));
    if (T->ExtraEnabled)
    {
        EpiSetDword(Diag,L"GpuGmuHfiExtraStatus",static_cast<ULONG>(T->ExtraStatus));
        EpiSetValue(Diag,L"GpuGmuHfiExtra",REG_BINARY,T->Extra,sizeof(T->Extra));
    }
}
static NTSTATUS GmuHfiPulse(GMU_TEST* S, HANDLE Params, ULONG Reg, ULONG Step)
{
    const NTSTATUS st=GmuStep(Params,Step);
    if (!NT_SUCCESS(st)) { return st; }
    WRITE_REGISTER_ULONG(GmuReg(S,Reg),1);
    return STATUS_SUCCESS;
}
static NTSTATUS GmuHfiExchange(GMU_TEST* S, HANDLE Params, ULONG* Words, ULONG Count,
                              ULONG Id, ULONG Seq, ULONG Step, GMU_HFI_TRACE* T, GMU_HFI_MESSAGE* M)
{
    const ULONG mask=0x3FF, padded=(Count+3)&~3UL;
    Words[0]=(Seq<<20)|(Count<<8)|Id; M->Header=Words[0];
    volatile ULONG* cmd=GmuHfiHeader(S,0);
    const ULONG read=cmd[10], write=cmd[11];
    if (cmd[3]!=0x400 || read>mask || write>mask || ((read|write)&3)!=0 ||
        cmd[1]!=EPI_GMU_HFI_IOVA+0x1000) { return STATUS_IO_DEVICE_ERROR; }
    KeMemoryBarrier();
    if (((read-write-1)&mask)<padded) { return STATUS_DEVICE_BUSY; }
    NTSTATUS st=GmuStep(Params,Step); if (!NT_SUCCESS(st)) { return st; }
    volatile ULONG* dst=GmuHfiData(S,0);
    for (ULONG i=0;i<padded;++i) { dst[(write+i)&mask]=i<Count ? Words[i] : 0xFAFAFAFA; }
    KeMemoryBarrier();
    if (Id==3) { T->BwPublished=TRUE; } // even a failed doorbell can leave a command visible to GMU
    cmd[11]=(write+padded)&mask;
    KeMemoryBarrier();
    st=GmuHfiPulse(S,Params,R_HOST2GMU_INTR_SET,Step+1); if (!NT_SUCCESS(st)) { return st; }
    st=GmuStep(Params,Step+2); if (!NT_SUCCESS(st)) { return st; }
    BOOLEAN notified=FALSE;
    for (ULONG tick=0;tick<10000;++tick) // total ACK wait budget: 10000 x 100 us, including empty/rejected messages
    {
        if (READ_REGISTER_ULONG(GmuReg(S,R_GMU2HOST_INTR_INFO))&1)
        {
            st=GmuHfiPulse(S,Params,R_GMU2HOST_INTR_CLR,Step+3);
            if (!NT_SUCCESS(st)) { return st; }
            notified=TRUE;
        }
        volatile ULONG* rsp=GmuHfiHeader(S,1);
        const ULONG r=rsp[10], w=rsp[11];
        if (rsp[3]!=0x400 || r>mask || w>mask || ((r|w)&3)!=0 ||
            rsp[1]!=EPI_GMU_HFI_IOVA+0x2000) { return STATUS_IO_DEVICE_ERROR; }
        KeMemoryBarrier();
        if (notified && r!=w)
        {
            volatile ULONG* src=GmuHfiData(S,1);
            const ULONG header=src[r], n=(header>>8)&0xFF, occupied=(w-r)&mask;
            const ULONG advance=(n+3)&~3UL;
            if (n==0 || n>RTL_NUMBER_OF(M->Ack) || advance>occupied)
            { M->AckDwords=n; M->Ack[0]=header; return STATUS_IO_DEVICE_ERROR; }
            ULONG packet[19]={};
            for (ULONG i=0;i<n;++i) { packet[i]=src[(r+i)&mask]; }
            KeMemoryBarrier(); rsp[10]=(r+advance)&mask; KeMemoryBarrier();
            if ((header&0xFF)==100)
            {
                if (n<4) { return STATUS_IO_DEVICE_ERROR; }
                ++M->FwErrors;
                for (ULONG i=0;i<4;++i) { M->LastFwError[i]=packet[i]; }
            }
            else if (n>=3 && ((packet[1]>>20)&0xFFF)==Seq)
            {
                M->AckDwords=n;
                for (ULONG i=0;i<n;++i) { M->Ack[i]=packet[i]; }
                return packet[1]==Words[0] && packet[2]==0 ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
            }
            // Preserve the last packet we skipped, including FW error notifications.
            ++M->Skipped; M->LastSkippedDwords=n;
            for (ULONG i=0;i<RTL_NUMBER_OF(M->LastSkipped);++i)
            { M->LastSkipped[i]=i<n ? packet[i] : 0; }
        }
        else if (r==w) { rsp[8]=1; KeMemoryBarrier(); } // recheck indices on next pass, avoiding a lost wakeup
        KeStallExecutionProcessor(100);
    }
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS GmuHfiTables(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc, GMU_HFI_TRACE* T,
                            BOOLEAN StartFirmware, BOOLEAN VoteGx)
{
    T->Attempted=TRUE;
    GmuHfiQueues(S,T,0); GmuHfiPower(S,Params,Cc,T,0);
    if (VoteGx && !NT_SUCCESS(T->ExtraStatus)) { return T->ExtraStatus; }
    NTSTATUS st=GmuHfiPulse(S,Params,R_GMU2HOST_INTR_CLR,0x700);
    if (!NT_SUCCESS(st)) { return st; }
    st=GmuHfiPulse(S,Params,R_HFI_CTRL_INIT,0x701); if (!NT_SUCCESS(st)) { return st; }
    st=GmuStep(Params,0x702); if (!NT_SUCCESS(st)) { return st; }
    for (ULONG i=0;i<=100;++i)
    {
        T->CtrlStatus=READ_REGISTER_ULONG(GmuReg(S,R_HFI_CTRL_STATUS));
        if (T->CtrlStatus&1) { break; }
        if (i==100) { return STATUS_IO_TIMEOUT; }
        KeStallExecutionProcessor(100);
    }
    GmuHfiQueues(S,T,1);
    // 59 dwords: header/counts, sixteen {GX vote, ACD, kHz}, four {CX vote, kHz}.
    ULONG perf[59]={}; perf[1]=3; perf[2]=2;
    for (ULONG i=0;i<16;++i) { perf[4+i*3]=0xFFFFFFFF; }
    perf[6]=0x00400201; perf[8]=315000;
    perf[9]=0x00800302; perf[11]=450000;
    perf[53]=0x00400202; perf[54]=200000;
    st=GmuHfiExchange(S,Params,perf,RTL_NUMBER_OF(perf),4,1,0x710,T,&T->Messages[0]);
    T->Messages[0].Status=st; GmuHfiQueues(S,T,2);
    if (!NT_SUCCESS(st)) { return st; }
    // 160 dwords: counts/masks, CNOC addresses[6]/data[2][6], DDR addresses[8]/data[16][8].
    ULONG bw[160]={}; bw[1]=1; bw[2]=1; bw[3]=3; bw[4]=1; bw[5]=7;
    bw[6]=0x5006C; bw[12]=0x40000000; bw[18]=0x60000001;
    bw[24]=0x50004; bw[25]=0x50000; bw[26]=0x50088;
    bw[32]=0x40000000; bw[33]=0x40000000; bw[34]=0x40000000;
    st=GmuHfiExchange(S,Params,bw,RTL_NUMBER_OF(bw),3,2,0x720,T,&T->Messages[1]);
    T->Messages[1].Status=st; GmuHfiQueues(S,T,3);
    if (!NT_SUCCESS(st) || !StartFirmware) { return st; }
    ULONG coreStart[2]={}; // header, handle=0; header 0x0030020E, padded to four DWORDs
    st=GmuHfiExchange(S,Params,coreStart,RTL_NUMBER_OF(coreStart),14,3,0x740,T,&T->Messages[2]);
    T->Messages[2].Status=st; GmuHfiQueues(S,T,4);
    if (!NT_SUCCESS(st)) { return st; }
    ULONG start[1]={}; // header only: 0x0040010A, padded to four DWORDs
    st=GmuHfiExchange(S,Params,start,RTL_NUMBER_OF(start),10,4,0x750,T,&T->Messages[3]);
    T->Messages[3].Status=st; GmuHfiQueues(S,T,5);
    if (!NT_SUCCESS(st) || !VoteGx) { return st; }
    st=GmuHfiExtra(S,Params,T,4); if (!NT_SUCCESS(st)) { return st; }
    ULONG vote[4]={0,1,1,0}; // ack_type=1, GX level 1 (315 MHz), BW level 0
    st=GmuHfiExchange(S,Params,vote,RTL_NUMBER_OF(vote),30,5,0x760,T,&T->Messages[4]);
    T->Messages[4].Status=st; GmuHfiQueues(S,T,6);
    return st;
}
// GX probe diagnostic ABI: all fields are DWORDs; no compiler-dependent BOOLEAN padding.
struct GMU_GX_TRACE {
    ULONG Attempted;
    NTSTATUS OobStatus;
    ULONG OobPolls, OobInfo;
    NTSTATUS HaltStatus;
    ULONG HaltPolls[2], HaltAck[2];
    NTSTATUS SlumberStatus, BusyStatus;
    ULONG BusyPolls, PowerCounterOriginal, QueueEmpty[2];
    NTSTATUS RpmhStopStatus;
    ULONG RpmhStopPolls;
    NTSTATUS IdleStatus;
    ULONG IdlePolls, IdleLast, BusyLast, RpmhStopLast;
    NTSTATUS BreadcrumbStatus;
    ULONG PowerCounterSaved;
    NTSTATUS PowerCounterRestoreStatus;
    ULONG Snap[5][12];
};
static_assert(sizeof(GMU_GX_TRACE)==340,"GX diagnostic ABI");
static NTSTATUS GmuGxStep(HANDLE Params, ULONG Step, GMU_GX_TRACE* T)
{
    const NTSTATUS st=GmuStep(Params,Step);
    if (!NT_SUCCESS(st) && NT_SUCCESS(T->BreadcrumbStatus)) { T->BreadcrumbStatus=st; }
    return st;
}
static VOID GmuGxSnap(GMU_TEST* S, volatile ULONG* Cc, GMU_GX_TRACE* T, ULONG Stage)
{
    // The caller stamps the phase once; all accesses stay in CX/RSCC/GPU_CC, never RBBM/CP.
    ULONG* v=T->Snap[Stage];
    volatile ULONG* r=static_cast<volatile ULONG*>(S->Maps[5]);
    v[1]=READ_REGISTER_ULONG(Cc+0x100C/4);
    v[2]=READ_REGISTER_ULONG(GmuReg(S,0x1F8D0));
    v[3]=READ_REGISTER_ULONG(GmuReg(S,R_GMU_RPMH_POWER_STATE));
    const ULONG offsets[]={0xD18,0xFB8,0x1258,0x14F8};
    for (ULONG i=0;i<RTL_NUMBER_OF(offsets);++i) { v[4+i]=READ_REGISTER_ULONG(r+offsets[i]/4); }
    v[8]=READ_REGISTER_ULONG(r+0x101); v[9]=READ_REGISTER_ULONG(r+0x004);
    v[10]=READ_REGISTER_ULONG(GmuReg(S,0x23B0C)); v[11]=READ_REGISTER_ULONG(Cc);
    ULONG cbar=0xFFFFFFFF;
    S->SmmuCbar[Stage][1]=static_cast<ULONG>(EpiSmmuQueryCbar(S->Smmu,&cbar));
    S->SmmuCbar[Stage][2]=cbar;
}
static NTSTATUS GmuGxWait(volatile ULONG* Reg, ULONG Mask, ULONG Value, ULONG DelayUs, ULONG Limit,
                           ULONG* Polls, ULONG* Last)
{
    for (ULONG i=0;i<=Limit;++i)
    {
        *Polls=i; *Last=READ_REGISTER_ULONG(Reg);
        if ((*Last&Mask)==Value) { return STATUS_SUCCESS; }
        if (i!=Limit) { KeStallExecutionProcessor(DelayUs); }
    }
    return STATUS_IO_TIMEOUT;
}
enum GMU_GX_REG_REASON {
    GX_REG_READ=0, GX_REG_NOT_REACHED=1, GX_REG_NO_ACK=2, GX_REG_GDSC_OFF=3,
    GX_REG_HM_OFF=4, GX_REG_MAP_FAILED=5, GX_REG_BREADCRUMB_FAILED=6
};
struct GMU_GX_REG_TRACE { ULONG Attempted, Reason, Gdscr, GxHm, Count, Value[6]; };
static_assert(sizeof(GMU_GX_REG_TRACE)==44,"GX register diagnostic ABI");
struct GMU_GX_HALT_TRACE {
    ULONG Attempted, Reason, Gdscr, GxHm, Count;
    NTSTATUS Status, CxIdleStatus;
    ULONG Before[4], After[4], FinalCx[2], CxPolls;
};
static_assert(sizeof(GMU_GX_HALT_TRACE)==72,"GX halt-clear diagnostic ABI");
static BOOLEAN GmuGxRegReady(volatile ULONG* Cc, GMU_TEST* S, const GMU_GX_TRACE* T, GMU_GX_REG_TRACE* R)
{
    R->Gdscr=READ_REGISTER_ULONG(Cc+0x100C/4);
    R->GxHm=READ_REGISTER_ULONG(GmuReg(S,0x1F8D0));
    if (!NT_SUCCESS(T->OobStatus)) { R->Reason=GX_REG_NO_ACK; return FALSE; }
    if (!(R->Gdscr&0x80000000UL)) { R->Reason=GX_REG_GDSC_OFF; return FALSE; }
    if (R->GxHm&0xC0) { R->Reason=GX_REG_HM_OFF; return FALSE; }
    return TRUE;
}
static NTSTATUS GmuGxClearHalt(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc,
                               const GMU_GX_TRACE* T, volatile ULONG* Gpu, GMU_GX_HALT_TRACE* H)
{
    volatile ULONG* cx=static_cast<volatile ULONG*>(S->Maps[6])+0x45;
    volatile ULONG* regs[]={cx,cx+1,Gpu+0x016,Gpu+0x017};
    for (ULONG phase=0;phase<3;++phase)
    {
        const NTSTATUS st=GmuStep(Params,phase==0 ? 0x7CE : 0x7D5+phase);
        if (!NT_SUCCESS(st)) { H->Reason=GX_REG_BREADCRUMB_FAILED; return st; }
        GMU_GX_REG_TRACE guard={};
        const BOOLEAN ready=GmuGxRegReady(Cc,S,T,&guard);
        H->Gdscr=guard.Gdscr; H->GxHm=guard.GxHm;
        if (!ready) { H->Reason=guard.Reason; return STATUS_INVALID_DEVICE_STATE; }
        if (phase==0)
        {
            for (ULONG i=0;i<4;++i) { H->Before[i]=READ_REGISTER_ULONG(regs[i]); }
            continue;
        }
        const ULONG i=(phase-1)*2;
        H->Attempted=1;
        WRITE_REGISTER_ULONG(regs[i],0);
        // Immediate readback drains the posted write; no persistence/guard operation in between.
        H->After[i]=READ_REGISTER_ULONG(regs[i]);
        H->After[i+1]=READ_REGISTER_ULONG(regs[i+1]);
        H->Count=phase;
        if (H->After[i]!=0) { H->Reason=7; return STATUS_IO_DEVICE_ERROR; }
    }
    H->Reason=0; return STATUS_SUCCESS;
}
static NTSTATUS GmuGxCxHaltIdle(GMU_TEST* S, HANDLE Params, GMU_GX_HALT_TRACE* H)
{
    const NTSTATUS st=GmuStep(Params,0x7D8); if (!NT_SUCCESS(st)) { return st; }
    volatile ULONG* cx=static_cast<volatile ULONG*>(S->Maps[6])+0x45;
    for (ULONG i=0;i<=100;++i)
    {
        H->CxPolls=i;
        H->FinalCx[0]=READ_REGISTER_ULONG(cx); H->FinalCx[1]=READ_REGISTER_ULONG(cx+1);
        // ACK may remain asserted with HALT=0; retain it for diagnostics only.
        if (H->FinalCx[0]==0) { return STATUS_SUCCESS; }
        if (i!=100) { KeStallExecutionProcessor(100); }
    }
    return STATUS_IO_TIMEOUT;
}
static VOID GmuGxReadReg(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc,
                         const GMU_GX_TRACE* T, GMU_GX_REG_TRACE* R, GMU_GX_HALT_TRACE* Halt = NULL)
{
    if (!GmuGxRegReady(Cc,S,T,R)) { return; }
    PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE;
    PVOID map=MmMapIoSpaceEx(pa,PAGE_SIZE,(Halt!=NULL ? PAGE_READWRITE : PAGE_READONLY)|PAGE_NOCACHE);
    if (map==NULL) { R->Reason=GX_REG_MAP_FAILED; return; }
    // A failed diagnostic read preparation must not prevent the mandatory OOB release/slumber path.
    const ULONG dwords[]={0x210,0x213,0x201,0x0AE,0x016,0x017};
    static_assert(RTL_NUMBER_OF(dwords)==RTL_NUMBER_OF(R->Value),"GX register diagnostic count");
    for (ULONG i=0;i<RTL_NUMBER_OF(dwords);++i)
    {
        if (!NT_SUCCESS(GmuStep(Params,0x7C8+i))) { R->Reason=GX_REG_BREADCRUMB_FAILED; break; }
        if (!GmuGxRegReady(Cc,S,T,R)) { break; } // recheck after every flush, still under the same OOB lease
        R->Attempted=1; R->Reason=GX_REG_READ;
        R->Value[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(map)+dwords[i]);
        R->Count=i+1;
    }
    if (Halt!=NULL && R->Count==RTL_NUMBER_OF(R->Value) && R->Reason==GX_REG_READ)
    { Halt->Status=GmuGxClearHalt(S,Params,Cc,T,static_cast<volatile ULONG*>(map),Halt); }
    MmUnmapIoSpace(map,PAGE_SIZE);
}
struct GMU_GX_INIT_TRACE {
    ULONG Attempted, Reason, Gdscr, GxHm, Count;
    NTSTATUS Status;
    ULONG Before[16], After[16];
};
static_assert(sizeof(GMU_GX_INIT_TRACE)==152,"GX init diagnostic ABI");
static VOID GmuGxInitTraceInit(GMU_GX_INIT_TRACE* t)
{
    RtlZeroMemory(t,sizeof(*t)); t->Reason=GX_REG_NOT_REACHED; t->Status=STATUS_NOT_FOUND;
    t->Gdscr=0xFFFFFFFF; t->GxHm=0xFFFFFFFF;
    for (ULONG i=0;i<16;++i) { t->Before[i]=0xFFFFFFFF; t->After[i]=0xFFFFFFFF; }

}
static NTSTATUS GmuGxSecvid(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc,
                             const GMU_GX_TRACE* T, GMU_GX_INIT_TRACE* I)
{
    // Only stage A is enabled: one page, no ADDR_MODE/TRUST_CNTL/hwcg accesses.
    const ULONG offsets[]={3,0,1,2}; // TSB_CNTL, TRUSTED_BASE low/high, TRUSTED_SIZE
    GMU_GX_REG_TRACE guard={};
    const BOOLEAN readyToMap=GmuGxRegReady(Cc,S,T,&guard);
    I->Gdscr=guard.Gdscr; I->GxHm=guard.GxHm;
    if (!readyToMap) { I->Reason=guard.Reason; return STATUS_INVALID_DEVICE_STATE; }
    PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+0x3E000;
    PVOID map=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
    if (map==NULL) { I->Reason=GX_REG_MAP_FAILED; return STATUS_INSUFFICIENT_RESOURCES; }
    NTSTATUS st=STATUS_SUCCESS;
    for (ULONG phase=0;phase<2 && NT_SUCCESS(st);++phase)
    {
        for (ULONG i=0;i<RTL_NUMBER_OF(offsets);++i)
        {
            st=GmuStep(Params,(phase==0 ? 0x800 : 0x810)+i);
            if (!NT_SUCCESS(st)) { I->Reason=GX_REG_BREADCRUMB_FAILED; break; }
            const BOOLEAN ready=GmuGxRegReady(Cc,S,T,&guard);
            I->Gdscr=guard.Gdscr; I->GxHm=guard.GxHm;
            if (!ready) { I->Reason=guard.Reason; st=STATUS_INVALID_DEVICE_STATE; break; }
            volatile ULONG* reg=static_cast<volatile ULONG*>(map)+offsets[i];
            if (phase==0) { I->Before[i]=READ_REGISTER_ULONG(reg); continue; }
            I->Attempted=1;
            WRITE_REGISTER_ULONG(reg,0);
            I->After[i]=READ_REGISTER_ULONG(reg); // drain the posted write immediately
            I->Count=i+1;
            if (I->After[i]!=0) { I->Reason=7; st=STATUS_IO_DEVICE_ERROR; break; }
        }
    }
    if (NT_SUCCESS(st)) { I->Reason=0; }
    MmUnmapIoSpace(map,PAGE_SIZE);
    return st;
}
static NTSTATUS GmuGxAddrMode(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc,
                               const GMU_GX_TRACE* T, GMU_GX_INIT_TRACE* I)
{
    if (I->Reason!=0 || I->Count!=4)
    { I->Reason=8; return STATUS_INVALID_DEVICE_STATE; }
    const ULONG dwords[]={0x0842,0x0C01,0x8601,0x8E05,0x9E01,0xBE05,
                          0xA601,0x9601,0x0E00,0xAE01,0xB601,0xF810};
    const ULONG pages[]={0x02000,0x03000,0x21000,0x23000,0x27000,0x2F000,
                         0x29000,0x25000,0x2B000,0x2D000,0x3E000};
    const ULONG pageIndex[]={0,1,2,3,4,5,6,7,1,8,9,10};
    PVOID maps[RTL_NUMBER_OF(pages)]={};
    GMU_GX_REG_TRACE guard={};
    const BOOLEAN readyToMap=GmuGxRegReady(Cc,S,T,&guard);
    I->Gdscr=guard.Gdscr; I->GxHm=guard.GxHm;
    if (!readyToMap) { I->Reason=guard.Reason; return STATUS_INVALID_DEVICE_STATE; }
    NTSTATUS st=STATUS_SUCCESS;
    for (ULONG i=0;i<RTL_NUMBER_OF(pages);++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+pages[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { I->Reason=GX_REG_MAP_FAILED; st=STATUS_INSUFFICIENT_RESOURCES; break; }
    }
    // Save every original in this stage before writing any ADDR_MODE register.
    for (ULONG phase=0;phase<2 && NT_SUCCESS(st);++phase)
    {
        for (ULONG i=0;i<RTL_NUMBER_OF(dwords);++i)
        {
            st=GmuStep(Params,(phase==0 ? 0x820 : 0x830)+i);
            if (!NT_SUCCESS(st)) { I->Reason=GX_REG_BREADCRUMB_FAILED; break; }
            const BOOLEAN ready=GmuGxRegReady(Cc,S,T,&guard);
            I->Gdscr=guard.Gdscr; I->GxHm=guard.GxHm;
            if (!ready) { I->Reason=guard.Reason; st=STATUS_INVALID_DEVICE_STATE; break; }
            volatile ULONG* reg=static_cast<volatile ULONG*>(maps[pageIndex[i]])+((dwords[i]*4)&0xFFF)/4;
            if (phase==0) { I->Before[4+i]=READ_REGISTER_ULONG(reg); continue; }
            I->Attempted=1;
            WRITE_REGISTER_ULONG(reg,1);
            I->After[4+i]=READ_REGISTER_ULONG(reg);
            I->Count=5+i;
            if (I->After[4+i]!=1) { I->Reason=7; st=STATUS_IO_DEVICE_ERROR; break; }
        }
    }
    if (NT_SUCCESS(st)) { I->Reason=0; }
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i)
    { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_GX_HWCG_TRACE {
    ULONG Attempted, Reason, Gdscr, GxHm, Count;
    NTSTATUS Status;
    ULONG Skipped, CgcBefore[3], CgcAfter[3], ClockCntlBefore, ClockCntlAfter;
    ULONG SptpracBefore, SptpracAfterClear, SptpracAfterSet, Before[49], After[49], CgcRestored[3];
    NTSTATUS CgcRestoreStatus;
};
static_assert(sizeof(GMU_GX_HWCG_TRACE)==480,"GX hwcg diagnostic ABI");
static const ULONG GmuHwcgTable[][2]={
    {0x0B0,0x02222222},{0x0B4,0x02222220},{0x0B8,0x00000080},{0x0BC,0x0000F3CF},
    {0x0C0,0x22222222},{0x0C4,0x22222222},{0x0C8,0x22222222},{0x0CC,0x00022222},
    {0x0D0,0x11111111},{0x0D4,0x11111111},{0x0D8,0x11111111},{0x0DC,0x00011111},
    {0x0E0,0x77777777},{0x0E4,0x77777777},{0x0E8,0x77777777},{0x0EC,0x00077777},
    {0x0F0,0x22222222},{0x0F4,0x01002222},{0x0F8,0x00002220},{0x100,0x00040F00},
    {0x104,0x25222022},{0x105,0x00005555},{0x106,0x00000011},{0x107,0x00445044},
    {0x108,0x04222222},{0x111,0x00002222},{0x114,0x00222222},{0x117,0x00000002},
    {0x11B,0x00002222},{0x109,0x00004000},{0x112,0x00002222},{0x115,0x00000200},
    {0x11C,0x00000000},{0x10A,0x00000000},{0x113,0x00000000},{0x116,0x04104004},
    {0x11D,0x00000000},{0x120,0x00000222},{0x121,0x00000111},{0x122,0x00000000},
    {0x10B,0x22222222},{0x110,0x00000004},{0x10F,0x00000002},{0x533,0x00000182},
    {0x044,0x00000000},{0x042,0x00000000},{0x118,0x00000222},{0x119,0x00000111},
    {0x11A,0x00000555}
};
static_assert(RTL_NUMBER_OF(GmuHwcgTable)==49,"hwcg table count");
static const ULONG GmuHwcgCgc[]={0x20202,0x10111,0x5555};
static VOID GmuGxHwcgTraceInit(GMU_GX_HWCG_TRACE* t)
{

    RtlFillMemory(t,sizeof(*t),0xFF);
    t->Attempted=0; t->Reason=GX_REG_NOT_REACHED; t->Count=0; t->Skipped=0;
    t->Status=STATUS_NOT_FOUND; t->CgcRestoreStatus=STATUS_NOT_FOUND;

}
static NTSTATUS GmuGxHwcgGuard(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                                const GMU_GX_TRACE* T,GMU_GX_HWCG_TRACE* H,ULONG Step)
{
    const NTSTATUS st=GmuStep(Params,Step);
    if (!NT_SUCCESS(st)) { H->Reason=GX_REG_BREADCRUMB_FAILED; return st; }
    GMU_GX_REG_TRACE guard={};
    const BOOLEAN ready=GmuGxRegReady(Cc,S,T,&guard);
    H->Gdscr=guard.Gdscr; H->GxHm=guard.GxHm;
    if (!ready) { H->Reason=guard.Reason; return STATUS_INVALID_DEVICE_STATE; }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuGxHwcg(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                           const GMU_GX_TRACE* T,GMU_GX_HWCG_TRACE* H)
{
    PVOID maps[3]={};
    const ULONGLONG physical[]={EPI_GMU_GPU_BASE,EPI_GMU_GPU_BASE+0x1000,EPI_GMU_GPU_BASE+0x6A000};
    NTSTATUS st=GmuGxHwcgGuard(S,Params,Cc,T,H,0x840);
    if (!NT_SUCCESS(st)) { return st; }
    for (ULONG i=0;i<3;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=physical[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; H->Reason=GX_REG_MAP_FAILED; break; }
    }
    volatile ULONG* regs[54]={};
    ULONG* before[54]={};
    BOOLEAN spTouched=FALSE;
    if (NT_SUCCESS(st))
    {
        for (ULONG i=0;i<3;++i) { regs[i]=GmuReg(S,0x23B09+i); before[i]=&H->CgcBefore[i]; }
        regs[3]=static_cast<volatile ULONG*>(maps[0])+0xAE; before[3]=&H->ClockCntlBefore;
        regs[4]=static_cast<volatile ULONG*>(maps[2])+0x200/4; before[4]=&H->SptpracBefore;
        for (ULONG i=0;i<49;++i)
        {
            const ULONG off=GmuHwcgTable[i][0];
            regs[5+i]=static_cast<volatile ULONG*>(maps[off>=0x400 ? 1 : 0])+(off&0x3FF);
            before[5+i]=&H->Before[i];
        }
        for (ULONG i=0;i<54;++i)
        {
            st=GmuGxHwcgGuard(S,Params,Cc,T,H,0x840+i);
            if (!NT_SUCCESS(st)) { break; }
            *before[i]=READ_REGISTER_ULONG(regs[i]);
        }
    }
    if (NT_SUCCESS(st) && H->ClockCntlBefore==0x8AA8AA82)
    { H->Skipped=1; H->Reason=0; goto done; }
    for (ULONG i=0;i<3 && NT_SUCCESS(st);++i)
    {
        st=GmuGxHwcgGuard(S,Params,Cc,T,H,0x880+i); if (!NT_SUCCESS(st)) { break; }
        H->Attempted=1;
        WRITE_REGISTER_ULONG(regs[i],GmuHwcgCgc[i]);
        H->CgcAfter[i]=READ_REGISTER_ULONG(regs[i]); ++H->Count;
        if (H->CgcAfter[i]!=GmuHwcgCgc[i]) { H->Reason=7; st=STATUS_IO_DEVICE_ERROR; }
    }
    if (NT_SUCCESS(st))
    {
        st=GmuGxHwcgGuard(S,Params,Cc,T,H,0x883);
        if (NT_SUCCESS(st))
        {
            const ULONG value=READ_REGISTER_ULONG(regs[4])&~1UL;
            spTouched=TRUE; // publication attempt requires unconditional re-enable below
            WRITE_REGISTER_ULONG(regs[4],value);
            H->SptpracAfterClear=READ_REGISTER_ULONG(regs[4]);
            // SPTPRAC readback is diagnostic only; continue even if it differs.
        }
    }
    for (ULONG i=0;i<49 && NT_SUCCESS(st);++i)
    {
        st=GmuGxHwcgGuard(S,Params,Cc,T,H,0x890+i); if (!NT_SUCCESS(st)) { break; }
        WRITE_REGISTER_ULONG(regs[5+i],GmuHwcgTable[i][1]);
        H->After[i]=READ_REGISTER_ULONG(regs[5+i]); ++H->Count;
        // Some table bits may not read back as written. Record all 49 without comparing.
    }
    if (spTouched)
    {
        // Recovery is mandatory even if the breadcrumb/guard fails; do not let that skip SP re-enable.
        const ULONG priorReason=H->Reason;
        const NTSTATUS recovery=GmuGxHwcgGuard(S,Params,Cc,T,H,0x8C1);
        const ULONG value=READ_REGISTER_ULONG(regs[4])|1UL;
        WRITE_REGISTER_ULONG(regs[4],value);
        H->SptpracAfterSet=READ_REGISTER_ULONG(regs[4]);
        // Keep the first failure; neither SP readback nor a later recovery guard replaces it.
        if (!NT_SUCCESS(st)) { H->Reason=priorReason; }
        else if (!NT_SUCCESS(recovery)) { st=recovery; }
    }
    if (NT_SUCCESS(st))
    {
        st=GmuGxHwcgGuard(S,Params,Cc,T,H,0x8C2);
        if (NT_SUCCESS(st))
        {
            WRITE_REGISTER_ULONG(regs[3],0x8AA8AA82);
            H->ClockCntlAfter=READ_REGISTER_ULONG(regs[3]); ++H->Count;
            if (H->ClockCntlAfter!=0x8AA8AA82) { H->Reason=7; st=STATUS_IO_DEVICE_ERROR; }
        }
    }
    if (NT_SUCCESS(st)) { H->Reason=0; }
done:
    for (ULONG i=0;i<3;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
static NTSTATUS GmuGxHwcgRestore(GMU_TEST* S,HANDLE Params,GMU_GX_HWCG_TRACE* H)
{
    NTSTATUS result=STATUS_SUCCESS;
    for (ULONG i=0;i<3 && i<H->Count;++i)
    {
        const NTSTATUS step=GmuStep(Params,0x8D0+i);
        WRITE_REGISTER_ULONG(GmuReg(S,0x23B09+i),H->CgcBefore[i]);
        H->CgcRestored[i]=READ_REGISTER_ULONG(GmuReg(S,0x23B09+i));
        const NTSTATUS st=H->CgcRestored[i]==H->CgcBefore[i] ? step : STATUS_IO_DEVICE_ERROR;
        if (!NT_SUCCESS(st) && NT_SUCCESS(result)) { result=st; }
    }
    return result;
}
struct GMU_GX_CFG_TRACE {
    ULONG Attempted, Reason;
    NTSTATUS Status;
    ULONG Count, Gdscr, GxHm;
    struct ENTRY { ULONG Reg, Value, Before, After; } Writes[30];
    ULONG CxBefore[3], CxRestored[3];
    NTSTATUS CxRestoreStatus;
    ULONG CxWritten;
};
static_assert(sizeof(GMU_GX_CFG_TRACE)==536,"GX stage D diagnostic ABI");
static const ULONG GmuGxCfgTable[][2]={
    {0x011,3},{0x50B,0xFFFFFFFF},
    {0xE05,0xFFFFFFC0},{0xE06,0x0001FFFF},{0xE09,0xFFFFF000},{0xE0A,0x0001FFFF},
    {0xE07,0xFFFFF000},{0xE08,0x0001FFFF},{0xE18,0x804},{0xE17,4},
    {0x8C2,0x02000140},{0x8C1,0x8040362C},{0xB34,0x20},{0x8C3,0x80},{0x9E00,0x00200200},
    // CP_AHB_CNTL (0x98D) is deliberately omitted, including original reads.
    {0x500,1},{0x8D0,0},{0x01F,0x401FFFFF},{0xE19,0x81},
    {0xB608,0},{0xB609,0x3FE05FF4},{0xB60A,0x3FA0EBEE},{0xB60B,0x3F5193ED},{0xB60C,0x3F0243F0},
    {0x23B0E,0xFF000000},{0x1F841,0x20},{0x1F840,1},{0x841,1},{0x011,0},{0x844,0x6E}
};
static_assert(RTL_NUMBER_OF(GmuGxCfgTable)==30,"stage D count");
static const ULONG GmuGxCfgCx[]={0x23B0E,0x1F841,0x1F840};
static VOID GmuGxCfgTraceInit(GMU_GX_CFG_TRACE* t)
{

    RtlFillMemory(t,sizeof(*t),0xFF);
    t->Attempted=0; t->Reason=GX_REG_NOT_REACHED; t->Status=STATUS_NOT_FOUND; t->Count=0; t->CxWritten=0;
    t->CxRestoreStatus=STATUS_NOT_FOUND;
    for (ULONG i=0;i<RTL_NUMBER_OF(t->Writes);++i)
    { t->Writes[i].Reg=GmuGxCfgTable[i][0]; t->Writes[i].Value=GmuGxCfgTable[i][1]; }

}
static NTSTATUS GmuGxCfgGuard(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                               const GMU_GX_TRACE* T,GMU_GX_CFG_TRACE* D,ULONG Step)
{
    const NTSTATUS st=GmuStep(Params,Step);
    if (!NT_SUCCESS(st)) { D->Reason=GX_REG_BREADCRUMB_FAILED; return st; }
    GMU_GX_REG_TRACE guard={};
    const BOOLEAN ready=GmuGxRegReady(Cc,S,T,&guard);
    D->Gdscr=guard.Gdscr; D->GxHm=guard.GxHm;
    if (!ready) { D->Reason=guard.Reason; return STATUS_INVALID_DEVICE_STATE; }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuGxCfg(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                          const GMU_GX_TRACE* T,GMU_GX_CFG_TRACE* D)
{
    const ULONG pages[]={0,0x1000,0x2000,0x3000,0x27000,0x2D000};
    PVOID maps[RTL_NUMBER_OF(pages)]={};
    volatile ULONG* regs[30]={};
    NTSTATUS st=GmuGxCfgGuard(S,Params,Cc,T,D,0x900);
    if (!NT_SUCCESS(st)) { return st; }
    for (ULONG i=0;i<RTL_NUMBER_OF(pages);++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+pages[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { D->Reason=GX_REG_MAP_FAILED; st=STATUS_INSUFFICIENT_RESOURCES; break; }
    }
    if (NT_SUCCESS(st))
    {
        for (ULONG i=0;i<RTL_NUMBER_OF(D->Writes);++i)
        {
            const ULONG reg=D->Writes[i].Reg;
            if (reg>=0x1F800) { regs[i]=GmuReg(S,reg); }
            else for (ULONG j=0;j<RTL_NUMBER_OF(pages);++j)
            { if ((reg*4&~0xFFFUL)==pages[j]) { regs[i]=static_cast<volatile ULONG*>(maps[j])+(reg&0x3FF); break; } }
            NT_ASSERT(regs[i]!=NULL);
            st=GmuGxCfgGuard(S,Params,Cc,T,D,0x900+i); if (!NT_SUCCESS(st)) { break; }
            D->Writes[i].Before=READ_REGISTER_ULONG(regs[i]);
            for (ULONG j=0;j<3;++j) { if (reg==GmuGxCfgCx[j]) { D->CxBefore[j]=D->Writes[i].Before; } }
            if (reg==0x1F841) { D->Writes[i].Value=(D->Writes[i].Before&~0xFFUL)|0x20; }
        }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(D->Writes) && NT_SUCCESS(st);++i)
    {
        st=GmuGxCfgGuard(S,Params,Cc,T,D,0x940+i); if (!NT_SUCCESS(st)) { break; }
        D->Attempted=1;
        for (ULONG j=0;j<3;++j) { if (D->Writes[i].Reg==GmuGxCfgCx[j]) { D->CxWritten|=1UL<<j; } }
        WRITE_REGISTER_ULONG(regs[i],D->Writes[i].Value);
        D->Writes[i].After=READ_REGISTER_ULONG(regs[i]); D->Count=i+1;
        // All stage D readbacks, including the two QOS writes, are diagnostic only.
    }
    if (NT_SUCCESS(st)) { D->Reason=0; }
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
static NTSTATUS GmuGxCfgRestore(GMU_TEST* S,HANDLE Params,GMU_GX_CFG_TRACE* D,GMU_GX_TRACE* T)
{
    NTSTATUS result=STATUS_SUCCESS;
    for (ULONG i=0;i<3;++i)
    {
        if (!(D->CxWritten&(1UL<<i))) { continue; }
        const NTSTATUS step=GmuStep(Params,0x9A0+i);
        volatile ULONG* reg=GmuReg(S,GmuGxCfgCx[i]);
        // ENABLE is normally already cleared by slumber. Avoid a duplicate write when it matches.
        if (READ_REGISTER_ULONG(reg)!=D->CxBefore[i]) { WRITE_REGISTER_ULONG(reg,D->CxBefore[i]); }
        D->CxRestored[i]=READ_REGISTER_ULONG(reg);
        const NTSTATUS st=D->CxRestored[i]==D->CxBefore[i] ? step : STATUS_IO_DEVICE_ERROR;
        if (i==2) { T->PowerCounterRestoreStatus=st; }
        if (!NT_SUCCESS(st) && NT_SUCCESS(result)) { result=st; }
    }
    return result;
}
struct GMU_GX_STAGE_E_TRACE {
    ULONG Attempted, Reason;
    NTSTATUS Status;
    ULONG Count, Gdscr, GxHm;
    struct ENTRY { ULONG Reg, Value, Before, After; } Writes[47];
};
static_assert(sizeof(GMU_GX_STAGE_E_TRACE)==776,"GX stage E diagnostic ABI");
static const ULONG GmuGxStageETable[][2]={
    {0x8E08,0x12},{0xB604,2},{0xAE02,0x22},{0xE01,0x00200000},{0x534,1},{0x84F,0xB},
    {0x850,0x13FC0000},{0x851,0x00140501},{0x852,0x0BD0050B},{0x853,0x8000050E},
    {0x854,0x80000510},{0x855,0x80000534},{0x856,0x82080800},{0x857,0x802008A0},
    {0x858,0x809008AB},{0x859,0x02B808DE},{0x85A,0x81340900},{0x85B,0x89C8098D},
    {0x85C,0x80040E00},{0x85D,0x80300E03},{0x85E,0x830C3C00},{0x85F,0x7FFC3CC4},
    {0x860,0x873C8630},{0x861,0x80008E00},{0x862,0x80008E08},{0x863,0x807C8E50},
    {0x864,0x89FC8E80},{0x865,0x876C9624},{0x866,0x80449E60},{0x867,0x861C9E78},
    {0x868,0x873CA630},{0x869,0x8000AE02},{0x86A,0x84BCAE50},{0x86B,0x8000B604},
    {0x86C,0x8018B608},{0x86D,0x8004BE02},{0x86E,0x857CBE20},{0x86F,0x97FCD000},
    {0x870,0xAFFCF000},{0x871,0x7FFCFC00},{0x872,0xFFFD8400},{0x873,0xFFFDA400},
    {0x874,0x910DF400},{0x875,0x01EDF844},{0x876,0x8001F860},{0x877,0x806DF887},
    {0x87F,0x8001F8C0}
};
static_assert(RTL_NUMBER_OF(GmuGxStageETable)==47,"stage E count");
static VOID GmuGxStageETraceInit(GMU_GX_STAGE_E_TRACE* t)
{

    RtlFillMemory(t,sizeof(*t),0xFF);
    t->Attempted=0; t->Reason=GX_REG_NOT_REACHED; t->Status=STATUS_NOT_FOUND; t->Count=0;
    for (ULONG i=0;i<RTL_NUMBER_OF(t->Writes);++i)
    { t->Writes[i].Reg=GmuGxStageETable[i][0]; t->Writes[i].Value=GmuGxStageETable[i][1]; }

}
static NTSTATUS GmuGxStageEGuard(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                                  const GMU_GX_TRACE* T,GMU_GX_STAGE_E_TRACE* E,ULONG Step)
{
    const NTSTATUS st=GmuStep(Params,Step);
    if (!NT_SUCCESS(st)) { E->Reason=GX_REG_BREADCRUMB_FAILED; return st; }
    GMU_GX_REG_TRACE guard={};
    const BOOLEAN ready=GmuGxRegReady(Cc,S,T,&guard);
    E->Gdscr=guard.Gdscr; E->GxHm=guard.GxHm;
    if (!ready) { E->Reason=guard.Reason; return STATUS_INVALID_DEVICE_STATE; }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuGxStageE(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                             const GMU_GX_TRACE* T,GMU_GX_STAGE_E_TRACE* E)
{
    const ULONG pages[]={0x1000,0x2000,0x3000,0x23000,0x2B000,0x2D000};
    PVOID maps[RTL_NUMBER_OF(pages)]={};
    volatile ULONG* regs[47]={};
    NTSTATUS st=GmuGxStageEGuard(S,Params,Cc,T,E,0xA00);
    if (!NT_SUCCESS(st)) { return st; }
    for (ULONG i=0;i<RTL_NUMBER_OF(pages);++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+pages[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { E->Reason=GX_REG_MAP_FAILED; st=STATUS_INSUFFICIENT_RESOURCES; break; }
    }
    if (NT_SUCCESS(st))
    {
        for (ULONG i=0;i<RTL_NUMBER_OF(E->Writes);++i)
        {
            const ULONG reg=E->Writes[i].Reg;
            for (ULONG j=0;j<RTL_NUMBER_OF(pages);++j)
            { if (((reg*4)&~0xFFFUL)==pages[j]) { regs[i]=static_cast<volatile ULONG*>(maps[j])+(reg&0x3FF); break; } }
            NT_ASSERT(regs[i]!=NULL);
            st=GmuGxStageEGuard(S,Params,Cc,T,E,0xA00+i); if (!NT_SUCCESS(st)) { break; }
            E->Writes[i].Before=READ_REGISTER_ULONG(regs[i]);
        }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(E->Writes) && NT_SUCCESS(st);++i)
    {
        st=GmuGxStageEGuard(S,Params,Cc,T,E,0xA40+i); if (!NT_SUCCESS(st)) { break; }
        E->Attempted=1;
        WRITE_REGISTER_ULONG(regs[i],E->Writes[i].Value);
        E->Writes[i].After=READ_REGISTER_ULONG(regs[i]); E->Count=i+1;
    }
    if (NT_SUCCESS(st)) { E->Reason=0; }
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_GX_STAGE_F_TRACE {
    ULONG Attempted, Reason;
    NTSTATUS Status;
    ULONG Count, Gdscr, GxHm, Before, After, Check[4];
};
static_assert(sizeof(GMU_GX_STAGE_F_TRACE)==48,"GX stage F diagnostic ABI");
static VOID GmuGxStageFTraceInit(GMU_GX_STAGE_F_TRACE* t)
{

    RtlFillMemory(t,sizeof(*t),0xFF);
    t->Attempted=0; t->Reason=GX_REG_NOT_REACHED; t->Status=STATUS_NOT_FOUND; t->Count=0;

}
static NTSTATUS GmuGxStageFGuard(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                                  const GMU_GX_TRACE* T,GMU_GX_STAGE_F_TRACE* F,ULONG Step)
{
    const NTSTATUS st=GmuStep(Params,Step);
    if (!NT_SUCCESS(st)) { F->Reason=GX_REG_BREADCRUMB_FAILED; return st; }
    GMU_GX_REG_TRACE guard={};
    const BOOLEAN ready=GmuGxRegReady(Cc,S,T,&guard);
    F->Gdscr=guard.Gdscr; F->GxHm=guard.GxHm;
    if (!ready) { F->Reason=guard.Reason; return STATUS_INVALID_DEVICE_STATE; }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuGxStageF(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                             const GMU_GX_TRACE* T,GMU_GX_STAGE_F_TRACE* F)
{
    PVOID maps[4]={};
    NTSTATUS st=GmuGxStageFGuard(S,Params,Cc,T,F,0xB00);
    if (!NT_SUCCESS(st)) { return st; }
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+i*PAGE_SIZE;
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { F->Reason=GX_REG_MAP_FAILED; st=STATUS_INSUFFICIENT_RESOURCES; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* ahb=static_cast<volatile ULONG*>(maps[2])+(0x98D&0x3FF);
        st=GmuGxStageFGuard(S,Params,Cc,T,F,0xB01);
        if (NT_SUCCESS(st)) { F->Before=READ_REGISTER_ULONG(ahb); }
        if (NT_SUCCESS(st))
        {
            st=GmuGxStageFGuard(S,Params,Cc,T,F,0xB02);
            if (NT_SUCCESS(st))
            {
                F->Attempted=1;
                WRITE_REGISTER_ULONG(ahb,1);
                F->After=READ_REGISTER_ULONG(ahb);
            }
        }
        const ULONG regs[]={0x011,0x500,0xE17,0x84F};
        for (ULONG i=0;i<RTL_NUMBER_OF(regs) && NT_SUCCESS(st);++i)
        {
            st=GmuGxStageFGuard(S,Params,Cc,T,F,0xB10+i); if (!NT_SUCCESS(st)) { break; }
            F->Check[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(maps[regs[i]/0x400])+(regs[i]&0x3FF));
            F->Count=i+1; // count only completed post-write diagnostic reads
        }
    }
    if (NT_SUCCESS(st)) { F->Reason=0; }
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
static NTSTATUS GmuG1Registers(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                                const GMU_GX_TRACE* T,GMU_G1_TRACE* G)
{
    const ULONG regs[]={0x830,0x831,0x800,0x801,0x802,0x804,0x805};
    PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+0x2000;
    GMU_GX_REG_TRACE guard={};
    const BOOLEAN ready=GmuGxRegReady(Cc,S,T,&guard);
    G->Gdscr=guard.Gdscr; G->GxHm=guard.GxHm;
    if (!ready) { G->Reason=guard.Reason; return STATUS_INVALID_DEVICE_STATE; }
    PVOID map=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
    if (map==NULL) { G->Reason=GX_REG_MAP_FAILED; return STATUS_INSUFFICIENT_RESOURCES; }
    NTSTATUS st=STATUS_SUCCESS;
    for (ULONG phase=0;phase<2 && NT_SUCCESS(st);++phase)
    {
        for (ULONG i=0;i<7;++i)
        {
            st=GmuStep(Params,(phase==0 ? 0xC10 : 0xC20)+i);
            if (!NT_SUCCESS(st)) { G->Reason=GX_REG_BREADCRUMB_FAILED; break; }
            const BOOLEAN live=GmuGxRegReady(Cc,S,T,&guard);
            G->Gdscr=guard.Gdscr; G->GxHm=guard.GxHm;
            if (!live) { G->Reason=guard.Reason; st=STATUS_INVALID_DEVICE_STATE; break; }
            volatile ULONG* reg=static_cast<volatile ULONG*>(map)+(regs[i]&0x3FF);
            if (phase==0) { G->Before[i]=READ_REGISTER_ULONG(reg); continue; }
            WRITE_REGISTER_ULONG(reg,G->Value[i]);
            G->After[i]=READ_REGISTER_ULONG(reg); G->Count=i+1;
        }
    }
    if (NT_SUCCESS(st)) { G->Reason=0; }
    MmUnmapIoSpace(map,PAGE_SIZE); return st;
}
struct GMU_G2_TRACE {
    ULONG Requested, Attempted, Reason;
    NTSTATUS Status;
    ULONG SqeWritten, WptrWrites, FenceStatus, Polls, ShadowBefore, ShadowAfter;
    ULONG SqeBefore, SqeAfter, WptrBefore, WptrAfter, Rptr, RbbmStatus, IntStatus, CpHwFault, CxBusy, Gdscr, GxHm;
    NTSTATUS FaultStatus;
    ULONG Fsr, FarLow, FarHigh, Fsynr, Packet[9];
};
static_assert(sizeof(GMU_G2_TRACE)==140,"G-2 diagnostic ABI");
static VOID GmuG2TraceInit(GMU_G2_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF);
    t->Requested=1; t->Attempted=0; t->Reason=1; t->Status=STATUS_NOT_FOUND;
    t->SqeWritten=0; t->WptrWrites=0; t->FaultStatus=STATUS_NOT_FOUND;

}
static BOOLEAN GmuG2Live(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_G2_TRACE* C)
{
    GMU_GX_REG_TRACE guard={};
    const BOOLEAN ready=GmuGxRegReady(Cc,S,T,&guard);
    C->Gdscr=guard.Gdscr; C->GxHm=guard.GxHm;
    if (!ready) { C->Reason=guard.Reason; }
    return ready;
}
static NTSTATUS GmuG2Guard(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                             const GMU_GX_TRACE* T,GMU_G2_TRACE* C,ULONG Step)
{
    const NTSTATUS st=GmuStep(Params,Step);
    if (!NT_SUCCESS(st)) { C->Reason=GX_REG_BREADCRUMB_FAILED; return st; }
    return GmuG2Live(S,Cc,T,C) ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
}
static NTSTATUS GmuCpFenceWait(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,
                                 GMU_G2_TRACE* C,volatile ULONG* Wptr,ULONG Target,
                                 volatile ULONG* Mask=NULL,ULONG* MaskReadback=NULL)
{
    for (ULONG i=0;;++i)
    {
        C->FenceStatus=READ_REGISTER_ULONG(GmuReg(S,0x23B13));
        if (!(C->FenceStatus&1)) { return STATUS_SUCCESS; }
        if (i==200) { C->Reason=10; return STATUS_IO_TIMEOUT; }
        KeStallExecutionProcessor(10);
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        if (Mask!=NULL)
        {
            const ULONG mask=READ_REGISTER_ULONG(Mask);
            if (MaskReadback!=NULL) { *MaskReadback=mask; }
            if (mask!=0) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
        }
        WRITE_REGISTER_ULONG(Wptr,Target); KeMemoryBarrier(); ++C->WptrWrites;
    }
}
static NTSTATUS GmuCpIdleWait(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,
                                GMU_G2_TRACE* C,volatile ULONG* Shadow,volatile ULONG* Gpu,
                                ULONG Target,ULONG ErrorMask=0)
{
    for (ULONG i=0;;++i)
    {
        C->Polls=i;
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        KeMemoryBarrier(); C->ShadowAfter=*Shadow;
        C->RbbmStatus=READ_REGISTER_ULONG(Gpu+0x210);
        C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201);
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
        if (C->IntStatus&ErrorMask) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
        if (C->ShadowAfter==Target && (C->RbbmStatus&~1UL)==0 && (C->CxBusy&(1UL<<23))==0) { return STATUS_SUCCESS; }
        if (i==10000) { C->Reason=9; return STATUS_IO_TIMEOUT; }
        KeStallExecutionProcessor(100);
    }
}
static NTSTATUS GmuG2Start(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                             const GMU_GX_TRACE* T,GMU_G2_TRACE* C)
{
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0xD00); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    GMU_REGION* sqe=GmuFind(S,GmuG1Iova[0],GmuG1Size[0]);
    if (S->Smmu==NULL || sqe==NULL || ring==NULL || shadow==NULL ||
        sqe->Va==NULL || ring->Va==NULL || shadow->Va==NULL || !sqe->Mapped || !ring->Mapped || !shadow->Mapped ||
        sqe->Flags!=GmuG1Flags[0] || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2])
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0xD01);
        if (NT_SUCCESS(st))
        {
            KeMemoryBarrier(); C->ShadowBefore=*rptr;
            C->SqeBefore=READ_REGISTER_ULONG(cp+8); C->WptrBefore=READ_REGISTER_ULONG(cp+7);
            BOOLEAN empty=C->ShadowBefore==0 && C->WptrBefore==0;
            for (ULONG i=0;i<9;++i) { if (static_cast<volatile ULONG*>(ring->Va)[i]!=0) { empty=FALSE; } }
            if (!empty) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xD02);
            if (NT_SUCCESS(st))
            {
                C->Attempted=1; C->SqeWritten=1;
                WRITE_REGISTER_ULONG(cp+8,1);
                KeMemoryBarrier(); C->SqeAfter=READ_REGISTER_ULONG(cp+8);
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xD03);
            if (NT_SUCCESS(st))
            {
                const ULONG packet[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                RtlCopyMemory(ring->Va,packet,sizeof(packet)); KeMemoryBarrier();
                RtlCopyMemory(C->Packet,ring->Va,sizeof(C->Packet));
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xD04);
            if (NT_SUCCESS(st)) { WRITE_REGISTER_ULONG(cp+7,9); KeMemoryBarrier(); ++C->WptrWrites; }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xD05);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,9); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xD06);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,9); }
        }
        // Only sample GX registers while the OOB lease and power guard still hold. Preserve the first error.
        const ULONG firstReason=C->Reason;
        const NTSTATUS sampled=GmuG2Guard(S,Params,Cc,T,C,0xD07);
        if (NT_SUCCESS(sampled))
        {
            C->ShadowAfter=*rptr; C->Rptr=READ_REGISTER_ULONG(cp+6); C->WptrAfter=READ_REGISTER_ULONG(cp+7);
            C->CpHwFault=READ_REGISTER_ULONG(cp+0x21);
            C->RbbmStatus=READ_REGISTER_ULONG(gpu+0x210); C->IntStatus=READ_REGISTER_ULONG(gpu+0x201);
            C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
    }
    if (S->Smmu!=NULL)
    {
        ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
        C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
        if (NT_SUCCESS(C->FaultStatus))
        { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_G3_TRACE {
    GMU_G2_TRACE Cp;
    ULONG ScratchBefore, ScratchAfter, DataBefore[4], DataAfter[4], Packet[11];
    ULONG CpInterrupt, CpProtect, CpStatus1, Checks, Prepared;
};
static_assert(sizeof(GMU_G3_TRACE)==244,"G-3 diagnostic ABI");
static const ULONG GmuG3Packet[]={
    0x48088501,0xCAFE0001,
    0x703D8003,0x70017000,0,0xCAFE0002,
    0x703E8003,0x00040885,0x70017008,0,
    0x70268000
};
static_assert(RTL_NUMBER_OF(GmuG3Packet)==11,"G-3 packet count");
static VOID GmuG3TraceInit(GMU_G3_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF);
    GmuG2TraceInit(&t->Cp); t->Checks=0; t->Prepared=0;
}
static NTSTATUS GmuG3Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                              const GMU_GX_TRACE* T,GMU_G3_TRACE* G)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0xE00); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x7010 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* data=reinterpret_cast<volatile ULONG*>(static_cast<UCHAR*>(ring->Va)+0x7000);
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0xE01);
        if (NT_SUCCESS(st))
        {
            KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
            G->ScratchBefore=READ_REGISTER_ULONG(cp+0x85);
            const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
            BOOLEAN previous=C->ShadowBefore==9 && C->WptrBefore==9;
            for (ULONG i=0;i<9;++i)
            { if (static_cast<volatile ULONG*>(ring->Va)[i]!=meInit[i]) { previous=FALSE; } }
            if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xE02);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=0;i<4;++i) { data[i]=0xFFFFFFFF; G->DataBefore[i]=data[i]; }
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+9,GmuG3Packet,sizeof(GmuG3Packet));
                KeMemoryBarrier();
                RtlCopyMemory(G->Packet,static_cast<ULONG*>(ring->Va)+9,sizeof(G->Packet)); G->Prepared=1;
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xE03);
            if (NT_SUCCESS(st))
            { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,20); KeMemoryBarrier(); ++C->WptrWrites; }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xE04);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,20); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xE05);
            if (NT_SUCCESS(st))
            { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,20,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        // Each diagnostic GX read gets its own flushed breadcrumb; RAM remains safe to sample separately.
        volatile ULONG* diagRegs[]={cp+0x85,cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,gpu+0x210,gpu+0x201};
        ULONG* diagValues[]={&G->ScratchAfter,&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,
                             &G->CpProtect,&G->CpStatus1,&C->RbbmStatus,&C->IntStatus};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0xE10+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<4;++i) { G->DataAfter[i]=data[i]; }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        if (G->Prepared)
        {
            G->Checks=(G->ScratchAfter==0xCAFE0001 ? 1UL : 0) | (G->DataAfter[0]==0xCAFE0002 ? 2UL : 0) |
                      (G->DataAfter[2]==0xCAFE0001 ? 4UL : 0) | (G->DataAfter[1]==0xFFFFFFFF ? 8UL : 0) |
                      (G->DataAfter[3]==0xFFFFFFFF ? 16UL : 0);
        }
        if (NT_SUCCESS(st) && G->Checks!=0x1F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    }
    if (S->Smmu!=NULL)
    {
        ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
        C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
        if (NT_SUCCESS(C->FaultStatus))
        { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_G3B_TRACE {
    GMU_G2_TRACE Cp;
    ULONG ScratchBefore, ScratchAfter, DataBefore[6], DataAfter[6], Packet[5];
    ULONG CpInterrupt, CpProtect, CpStatus1, Checks, Prepared;
};
static_assert(sizeof(GMU_G3B_TRACE)==236,"G-3b diagnostic ABI");
static const ULONG GmuG3bPacket[]={0x70460004,0x00000004,0x70017010,0,0xCAFE0003};
static VOID GmuG3bTraceInit(GMU_G3B_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF);
    GmuG2TraceInit(&t->Cp); t->Checks=0; t->Prepared=0;
}
static NTSTATUS GmuG3bSubmit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                               const GMU_GX_TRACE* T,GMU_G3B_TRACE* G)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0xF00); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x7018 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* data=reinterpret_cast<volatile ULONG*>(static_cast<UCHAR*>(ring->Va)+0x7000);
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0xF01);
        if (NT_SUCCESS(st))
        {
            KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
            G->ScratchBefore=READ_REGISTER_ULONG(cp+0x85);
            const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF};
            const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
            BOOLEAN previous=C->ShadowBefore==20 && C->WptrBefore==20;
            for (ULONG i=0;i<4;++i)
            { G->DataBefore[i]=data[i]; if (G->DataBefore[i]!=expected[i]) { previous=FALSE; } }
            for (ULONG i=0;i<9;++i)
            { if (static_cast<volatile ULONG*>(ring->Va)[i]!=meInit[i]) { previous=FALSE; } }
            for (ULONG i=0;i<11;++i)
            { if (static_cast<volatile ULONG*>(ring->Va)[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
            if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xF02);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=4;i<6;++i) { data[i]=0xFFFFFFFF; G->DataBefore[i]=data[i]; }
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+20,GmuG3bPacket,sizeof(GmuG3bPacket));
                KeMemoryBarrier();
                RtlCopyMemory(G->Packet,static_cast<ULONG*>(ring->Va)+20,sizeof(G->Packet)); G->Prepared=1;
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xF03);
            if (NT_SUCCESS(st))
            { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,25); KeMemoryBarrier(); ++C->WptrWrites; }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xF04);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,25); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0xF05);
            if (NT_SUCCESS(st))
            { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,25,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        volatile ULONG* diagRegs[]={cp+0x85,cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,gpu+0x210,gpu+0x201};
        ULONG* diagValues[]={&G->ScratchAfter,&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,
                             &G->CpProtect,&G->CpStatus1,&C->RbbmStatus,&C->IntStatus};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0xF10+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<6;++i) { G->DataAfter[i]=data[i]; }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        if (G->Prepared)
        {
            for (ULONG i=0;i<4;++i) { if (G->DataAfter[i]==G->DataBefore[i]) { G->Checks|=1UL<<i; } }
            if (G->DataAfter[4]==0xCAFE0003) { G->Checks|=1UL<<4; }
            if (G->DataAfter[5]==0xFFFFFFFF) { G->Checks|=1UL<<5; }
        }
        if (NT_SUCCESS(st) && G->Checks!=0x3F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    }
    if (S->Smmu!=NULL)
    {
        ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
        C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
        if (NT_SUCCESS(C->FaultStatus))
        { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_G3C_TRACE {
    GMU_G2_TRACE Cp;
    ULONG ScratchBefore, ScratchAfter, DataBefore[8], DataAfter[8], Packet[5];
    ULONG CpInterrupt, CpProtect, CpStatus1, Checks, Prepared;
    ULONG MaskBefore, MaskAtSubmit, MaskAfter, StatusBefore, UnmaskedBefore, IrqPolls;
    ULONG StatusFirst, UnmaskedFirst, StatusLast, UnmaskedLast, ClearAttempted, StatusAfterClear, UnmaskedAfterClear;
};
static_assert(sizeof(GMU_G3C_TRACE)==304,"G-3c diagnostic ABI");
static const ULONG GmuG3cPacket[]={0x70460004,0x80000004,0x70017018,0,0xCAFE0004};
static VOID GmuG3cTraceInit(GMU_G3C_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF);
    GmuG2TraceInit(&t->Cp); t->Checks=0; t->Prepared=0; t->ClearAttempted=0;
}
static NTSTATUS GmuG3cSubmit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                               const GMU_GX_TRACE* T,GMU_G3C_TRACE* G)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1000); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x7020 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* data=reinterpret_cast<volatile ULONG*>(static_cast<UCHAR*>(ring->Va)+0x7000);
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000}; BOOLEAN maskVerified=FALSE;
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        volatile ULONG* initialRegs[]={gpu+0x38,gpu+0x201,GmuReg(S,0x23B15)};
        ULONG* initialValues[]={&G->MaskBefore,&G->StatusBefore,&G->UnmaskedBefore};
        for (ULONG i=0;i<3;++i)
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1001+i); if (!NT_SUCCESS(st)) { break; }
            *initialValues[i]=READ_REGISTER_ULONG(initialRegs[i]);
        }
        if (NT_SUCCESS(st))
        {
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else { maskVerified=TRUE; }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1004);
            if (NT_SUCCESS(st))
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                G->ScratchBefore=READ_REGISTER_ULONG(cp+0x85);
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,0xCAFE0003,0xFFFFFFFF};
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                BOOLEAN previous=C->ShadowBefore==25 && C->WptrBefore==25;
                for (ULONG i=0;i<6;++i)
                { G->DataBefore[i]=data[i]; if (G->DataBefore[i]!=expected[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i)
                { if (static_cast<volatile ULONG*>(ring->Va)[i]!=meInit[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i)
                { if (static_cast<volatile ULONG*>(ring->Va)[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (static_cast<volatile ULONG*>(ring->Va)[20+i]!=GmuG3bPacket[i]) { previous=FALSE; } }
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1005);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=6;i<8;++i) { data[i]=0xFFFFFFFF; G->DataBefore[i]=data[i]; }
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+25,GmuG3cPacket,sizeof(GmuG3cPacket));
                KeMemoryBarrier();
                RtlCopyMemory(G->Packet,static_cast<ULONG*>(ring->Va)+25,sizeof(G->Packet)); G->Prepared=1;
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1006);
            if (NT_SUCCESS(st))
            {
                G->MaskAtSubmit=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAtSubmit!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,30); KeMemoryBarrier(); ++C->WptrWrites; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1007);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,30,gpu+0x38,&G->MaskAtSubmit); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1008);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,30,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1009);
            if (NT_SUCCESS(st)) for (ULONG i=0;;++i)
            {
                G->IrqPolls=i;
                if (!GmuG2Live(S,Cc,T,C)) { st=STATUS_INVALID_DEVICE_STATE; break; }
                G->UnmaskedLast=READ_REGISTER_ULONG(GmuReg(S,0x23B15));
                G->StatusLast=READ_REGISTER_ULONG(gpu+0x201);
                if ((G->StatusLast&(1UL<<20)) && G->StatusFirst==0xFFFFFFFF) { G->StatusFirst=i; }
                if ((G->UnmaskedLast&(1UL<<20)) && G->UnmaskedFirst==0xFFFFFFFF) { G->UnmaskedFirst=i; }
                if (G->StatusLast&(1UL<<23)) { C->Reason=11; st=STATUS_IO_DEVICE_ERROR; break; }
                if (G->StatusLast&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; st=STATUS_IO_DEVICE_ERROR; break; }
                if ((G->StatusFirst!=0xFFFFFFFF && G->UnmaskedFirst!=0xFFFFFFFF) || i==100) { break; }
                KeStallExecutionProcessor(10);
            }
        }
        if (C->Attempted)
        {
            // Clear only bit20 after any publication attempt, even if persistence fails; never access GX when unsafe.
            const ULONG firstReason=C->Reason;
            const NTSTATUS step=GmuStep(Params,0x100A);
            const BOOLEAN live=GmuG2Live(S,Cc,T,C);
            if (live)
            { G->ClearAttempted=1; WRITE_REGISTER_ULONG(gpu+0x37,1UL<<20); KeMemoryBarrier(); }
            if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
            else if (!NT_SUCCESS(step)) { C->Reason=GX_REG_BREADCRUMB_FAILED; st=step; }
            else if (!live) { st=STATUS_INVALID_DEVICE_STATE; }
            volatile ULONG* afterRegs[]={gpu+0x201,GmuReg(S,0x23B15)};
            ULONG* afterValues[]={&G->StatusAfterClear,&G->UnmaskedAfterClear};
            for (ULONG i=0;i<2;++i)
            {
                const ULONG prior=C->Reason;
                const NTSTATUS sampled=GmuG2Guard(S,Params,Cc,T,C,0x100B+i);
                if (NT_SUCCESS(sampled)) { *afterValues[i]=READ_REGISTER_ULONG(afterRegs[i]); }
                if (!NT_SUCCESS(st)) { C->Reason=prior; }
                else if (!NT_SUCCESS(sampled)) { st=sampled; }
                if (!NT_SUCCESS(sampled)) { break; }
            }
        }
        if (maskVerified)
        {
            const ULONG prior=C->Reason;
            const NTSTATUS sampled=GmuG2Guard(S,Params,Cc,T,C,0x100D);
            if (NT_SUCCESS(sampled)) { G->MaskAfter=READ_REGISTER_ULONG(gpu+0x38); }
            if (!NT_SUCCESS(st)) { C->Reason=prior; }
            else if (!NT_SUCCESS(sampled)) { st=sampled; }
            volatile ULONG* diagRegs[]={cp+0x85,cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,gpu+0x210,gpu+0x201};
            ULONG* diagValues[]={&G->ScratchAfter,&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,
                                 &G->CpProtect,&G->CpStatus1,&C->RbbmStatus,&C->IntStatus};
            const ULONG firstReason=C->Reason;
            NTSTATUS sampledDiag=STATUS_SUCCESS;
            for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
            {
                sampledDiag=GmuG2Guard(S,Params,Cc,T,C,0x1010+i); if (!NT_SUCCESS(sampledDiag)) { break; }
                *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
            }
            C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
            KeMemoryBarrier(); C->ShadowAfter=*rptr;
            for (ULONG i=0;i<8;++i) { G->DataAfter[i]=data[i]; }
            if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
            else if (!NT_SUCCESS(sampledDiag)) { st=sampledDiag; }
            if (G->Prepared)
            {
                for (ULONG i=0;i<6;++i) { if (G->DataAfter[i]==G->DataBefore[i]) { G->Checks|=1UL<<i; } }
                if (G->DataAfter[6]==0xCAFE0004) { G->Checks|=1UL<<6; }
                if (G->DataAfter[7]==0xFFFFFFFF) { G->Checks|=1UL<<7; }
            }
            if (NT_SUCCESS(st) && G->Checks!=0xFF) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
        }
    }
    if (S->Smmu!=NULL)
    {
        ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
        C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
        if (NT_SUCCESS(C->FaultStatus))
        { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_G4_TRACE {
    GMU_G2_TRACE Cp;
    ULONG ScratchBefore, ScratchAfter, Scratch2Before, Scratch2After;
    ULONG DataBefore[12], DataAfter[12], Packet[9], Ib[6];
    ULONG CpInterrupt, CpProtect, CpStatus1, IbBaseLow, IbBaseHigh, IbRemaining, Checks, Prepared;
    ULONG MaskBefore, MaskAtSubmit, MaskAfter;
};
static_assert(sizeof(GMU_G4_TRACE)==356,"G-4 diagnostic ABI");
static const ULONG GmuG4Ib[]={0x48088601,0xCAFE0005,0x703D8003,0x70017020,0,0xCAFE0006};
static const ULONG GmuG4Packet[]={0x70BF8003,0x70016000,0,6,0x70460004,4,0x70017024,0,0xCAFE0007};
static VOID GmuG4TraceInit(GMU_G4_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF);
    GmuG2TraceInit(&t->Cp); t->Checks=0; t->Prepared=0;
}
static NTSTATUS GmuG4Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                              const GMU_GX_TRACE* T,GMU_G4_TRACE* G)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1100); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x7030 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* data=reinterpret_cast<volatile ULONG*>(static_cast<UCHAR*>(ring->Va)+0x7000);
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0x1101);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                G->ScratchBefore=READ_REGISTER_ULONG(cp+0x86); G->Scratch2Before=READ_REGISTER_ULONG(cp+0x85);
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,
                                        0xCAFE0003,0xFFFFFFFF,0xCAFE0004,0xFFFFFFFF};
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                BOOLEAN previous=C->ShadowBefore==30 && C->WptrBefore==30 && G->Scratch2Before==0xCAFE0001;
                for (ULONG i=0;i<8;++i)
                { G->DataBefore[i]=data[i]; if (G->DataBefore[i]!=expected[i]) { previous=FALSE; } }
                volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
                for (ULONG i=0;i<9;++i) { if (words[i]!=meInit[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1102);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=8;i<12;++i) { data[i]=0xFFFFFFFF; G->DataBefore[i]=data[i]; }
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x6000,GmuG4Ib,sizeof(GmuG4Ib));
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+30,GmuG4Packet,sizeof(GmuG4Packet));
                KeMemoryBarrier();
                RtlCopyMemory(G->Ib,static_cast<UCHAR*>(ring->Va)+0x6000,sizeof(G->Ib));
                RtlCopyMemory(G->Packet,static_cast<ULONG*>(ring->Va)+30,sizeof(G->Packet)); G->Prepared=1;
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1103);
            if (NT_SUCCESS(st))
            {
                G->MaskAtSubmit=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAtSubmit!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,39); KeMemoryBarrier(); ++C->WptrWrites; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1104);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,39,gpu+0x38,&G->MaskAtSubmit); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1105);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,39,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        volatile ULONG* diagRegs[]={cp+0x86,cp+0x85,cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&G->ScratchAfter,&G->Scratch2After,&C->Rptr,&C->WptrAfter,&C->CpHwFault,
                             &G->CpInterrupt,&G->CpProtect,&G->CpStatus1,&C->RbbmStatus,&C->IntStatus,
                             &G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1110+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<12;++i) { G->DataAfter[i]=data[i]; }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        if (G->Prepared)
        {
            G->Checks=(G->ScratchAfter==0xCAFE0005 ? 1UL : 0) | (G->DataAfter[8]==0xCAFE0006 ? 2UL : 0) |
                      (G->DataAfter[9]==0xCAFE0007 ? 4UL : 0) |
                      (G->DataAfter[10]==0xFFFFFFFF && G->DataAfter[11]==0xFFFFFFFF ? 8UL : 0);
            BOOLEAN unchanged=G->Scratch2Before==0xCAFE0001 && G->Scratch2After==G->Scratch2Before;
            for (ULONG i=0;i<8;++i) { if (G->DataAfter[i]!=G->DataBefore[i]) { unchanged=FALSE; } }
            if (unchanged) { G->Checks|=16; }
        }
        if (NT_SUCCESS(st) && G->Checks!=0x1F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    }
    if (S->Smmu!=NULL)
    {
        ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
        C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
        if (NT_SUCCESS(C->FaultStatus))
        { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_G5_TRACE {
    GMU_G2_TRACE Cp;
    ULONG CcuBefore, CcuAfter, DataBefore[12], DataAfter[12], Packet[9], Ib[36];
    ULONG CpInterrupt, CpProtect, CpStatus1, IbBaseLow, IbBaseHigh, IbRemaining, Checks, Prepared;
    ULONG MaskBefore, MaskAtSubmit, MaskAfter;
    ULONG PixelMatches, PixelFirstBad, PixelBadValue, Pixels[4], Sentinel[16], SentinelMatches;
};
static_assert(sizeof(GMU_G5_TRACE)==564,"G-5 diagnostic ABI");
static const ULONG GmuG5Ib[]={
    0x70460001,25, 0x70460001,24, 0x70268000,
    0x408E0701,0x10000000, // RB_CCU_CNTL bypass offset is provisional; record the original before publication.
    0x488C0101,0, 0x408C0001,0x10F03080, 0x48840001,0x10F03080, 0x48ACC001,0x0000F180,
    0x408C1704,0x30,0x70017400,0,1, 0x488C2C04,0x12,0x34,0x56,0x78,
    0x48840502,0,0x000F000F, 0x702C0001,3,
    0x70460004,29,0x70017028,0,0xCAFE0008, 0x70268000
};
static_assert(sizeof(GmuG5Ib)==36*sizeof(ULONG),"G-5 IB length");
static const ULONG GmuG5Packet[]={0x70BF8003,0x70016100,0,36,0x70460004,4,0x7001702C,0,0xCAFE0009};
static VOID GmuG5TraceInit(GMU_G5_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF);
    GmuG2TraceInit(&t->Cp); t->Checks=0; t->Prepared=0;
}
static NTSTATUS GmuG5Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                              const GMU_GX_TRACE* T,GMU_G5_TRACE* G)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1200); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x7840 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    volatile ULONG* data=words+0x7000/4;
    volatile ULONG* pixels=words+0x7400/4;
    volatile ULONG* sentinel=words+0x7800/4;
    PVOID maps[3]={}; const ULONG offsets[]={0,0x2000,0x23000};
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        volatile ULONG* ccu=static_cast<volatile ULONG*>(maps[2])+0x81C/4;
        st=GmuG2Guard(S,Params,Cc,T,C,0x1201);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,
                                        0xCAFE0003,0xFFFFFFFF,0xCAFE0004,0xFFFFFFFF,
                                        0xCAFE0006,0xCAFE0007,0xFFFFFFFF,0xFFFFFFFF};
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                BOOLEAN previous=C->ShadowBefore==39 && C->WptrBefore==39;
                for (ULONG i=0;i<12;++i)
                { G->DataBefore[i]=data[i]; if (G->DataBefore[i]!=expected[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1202);
            if (NT_SUCCESS(st)) { G->CcuBefore=READ_REGISTER_ULONG(ccu); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1203);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=0;i<256;++i) { pixels[i]=0xFFFFFFFF; }
                for (ULONG i=0;i<16;++i) { sentinel[i]=0xFFFFFFFF; }
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x6100,GmuG5Ib,sizeof(GmuG5Ib));
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+39,GmuG5Packet,sizeof(GmuG5Packet));
                KeMemoryBarrier();
                RtlCopyMemory(G->Ib,static_cast<UCHAR*>(ring->Va)+0x6100,sizeof(G->Ib));
                RtlCopyMemory(G->Packet,static_cast<ULONG*>(ring->Va)+39,sizeof(G->Packet)); G->Prepared=1;
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1204);
            if (NT_SUCCESS(st))
            {
                G->MaskAtSubmit=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAtSubmit!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,48); KeMemoryBarrier(); ++C->WptrWrites; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1205);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,48,gpu+0x38,&G->MaskAtSubmit); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1206);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,48,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        volatile ULONG* diagRegs[]={ccu,cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&G->CcuAfter,&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,
                             &G->CpProtect,&G->CpStatus1,&C->RbbmStatus,&C->IntStatus,
                             &G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1210+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<12;++i) { G->DataAfter[i]=data[i]; }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        if (G->Prepared)
        {
            G->PixelMatches=0; G->SentinelMatches=0;
            for (ULONG i=0;i<256;++i)
            {
                const ULONG value=pixels[i];
                if (value==0x78563412) { ++G->PixelMatches; }
                else if (G->PixelFirstBad==0xFFFFFFFF) { G->PixelFirstBad=i; G->PixelBadValue=value; }
            }
            const ULONG indices[]={0,15,16,255};
            for (ULONG i=0;i<4;++i) { G->Pixels[i]=pixels[indices[i]]; }
            for (ULONG i=0;i<16;++i)
            { G->Sentinel[i]=sentinel[i]; if (G->Sentinel[i]==0xFFFFFFFF) { ++G->SentinelMatches; } }
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            G->Checks=(C->ShadowAfter==48 && C->Rptr==48 && C->WptrAfter==48 &&
                       !(C->IntStatus&errors) && !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23)) ? 1UL : 0) |
                      (G->DataAfter[10]==0xCAFE0008 ? 2UL : 0) | (G->DataAfter[11]==0xCAFE0009 ? 4UL : 0) |
                      (G->PixelMatches==256 ? 8UL : 0) | (G->SentinelMatches==16 ? 16UL : 0);
        }
        if (NT_SUCCESS(st) && G->Checks!=0x1F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    }
    if (S->Smmu!=NULL)
    {
        ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
        C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
        if (NT_SUCCESS(C->FaultStatus))
        { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_G5B_TRACE {
    GMU_G5_TRACE Draw;
    ULONG Data12Before, Data12After, Packet[13], Ib[10], MagicBefore[4], MagicAfter[4];
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4], GlobalAfter[4], ScanCount[3], ScanFirst[3], ScanDwords;
};
static_assert(sizeof(GMU_G5B_TRACE)==764,"G-5b diagnostic ABI");
static const ULONG GmuG5bIb[]={0x70460001,49,0x70268000,0x408E0401,0x04100000,
                             0x408E0101,0,0x400E1201,1,0x70268000};
static const ULONG GmuG5bPacket[]={0x70BF8003,0x70016200,0,10,0x70BF8003,0x70016100,0,36,
                                 0x70460004,4,0x70017030,0,0xCAFE000A};
static VOID GmuG5bTraceInit(GMU_G5B_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG5TraceInit(&t->Draw);
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
}
static BOOLEAN GmuG5bCanFollow(const GMU_G5_TRACE* G)
{
    return G!=NULL && G->Prepared==1 && G->Cp.Attempted==1 && (G->Checks==0x17 || G->Checks==0x1F) &&
           (NT_SUCCESS(G->Cp.Status) || (G->Cp.Status==STATUS_IO_DEVICE_ERROR && G->Cp.Reason==13));
}
static NTSTATUS GmuG5bSubmit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                              const GMU_GX_TRACE* T,GMU_G5B_TRACE* B)
{
    GMU_G5_TRACE* G=&B->Draw;
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1300); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x7840 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    volatile ULONG* data=words+0x7000/4;
    volatile ULONG* pixels=words+0x7400/4;
    volatile ULONG* sentinel=words+0x7800/4;
    PVOID maps[4]={}; const ULONG offsets[]={0,0x2000,0x23000,0x3000};
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        volatile ULONG* ccu=static_cast<volatile ULONG*>(maps[2])+0x81C/4;
        st=GmuG2Guard(S,Params,Cc,T,C,0x1301);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,
                                        0xCAFE0003,0xFFFFFFFF,0xCAFE0004,0xFFFFFFFF,
                                        0xCAFE0006,0xCAFE0007,0xCAFE0008,0xCAFE0009};
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                BOOLEAN previous=C->ShadowBefore==48 && C->WptrBefore==48;
                for (ULONG i=0;i<12;++i)
                { G->DataBefore[i]=data[i]; if (G->DataBefore[i]!=expected[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i) { if (words[39+i]!=GmuG5Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<36;++i) { if (words[0x6100/4+i]!=GmuG5Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<256;++i) { if (pixels[i]!=0xFFFFFFFF) { previous=FALSE; } }
                for (ULONG i=0;i<16;++i) { if (sentinel[i]!=0xFFFFFFFF) { previous=FALSE; } }
                B->Data12Before=data[12];
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        volatile ULONG* magic[]={static_cast<volatile ULONG*>(maps[2])+0x810/4,
                                 static_cast<volatile ULONG*>(maps[2])+0x804/4,
                                 static_cast<volatile ULONG*>(maps[3])+0x848/4,
                                 static_cast<volatile ULONG*>(maps[3])+0x864/4};
        if (NT_SUCCESS(st)) for (ULONG i=0;i<4;++i)
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1302+i); if (!NT_SUCCESS(st)) { break; }
            B->MagicBefore[i]=READ_REGISTER_ULONG(magic[i]);
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1306);
            if (NT_SUCCESS(st)) { G->CcuBefore=READ_REGISTER_ULONG(ccu); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1307);
            if (NT_SUCCESS(st)) { B->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,B->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1308);
            if (NT_SUCCESS(st))
            {
                data[12]=0xFFFFFFFF;
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x6200,GmuG5bIb,sizeof(GmuG5bIb));
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+48,GmuG5bPacket,sizeof(GmuG5bPacket));
                KeMemoryBarrier();
                RtlCopyMemory(B->Ib,static_cast<UCHAR*>(ring->Va)+0x6200,sizeof(B->Ib));
                RtlCopyMemory(B->Packet,static_cast<ULONG*>(ring->Va)+48,sizeof(B->Packet));
                RtlCopyMemory(G->Ib,static_cast<UCHAR*>(ring->Va)+0x6100,sizeof(G->Ib)); G->Prepared=1;
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1309);
            if (NT_SUCCESS(st))
            {
                G->MaskAtSubmit=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAtSubmit!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,61); KeMemoryBarrier(); ++C->WptrWrites; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x130A);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,61,gpu+0x38,&G->MaskAtSubmit); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x130B);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,61,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        volatile ULONG* diagRegs[]={ccu,cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&G->CcuAfter,&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,
                             &G->CpProtect,&G->CpStatus1,&C->RbbmStatus,&C->IntStatus,
                             &G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1310+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        for (ULONG i=0;i<4;++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1320+i); if (!NT_SUCCESS(sampled)) { break; }
            B->MagicAfter[i]=READ_REGISTER_ULONG(magic[i]);
        }
        if (NT_SUCCESS(sampled))
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1324);
            if (NT_SUCCESS(sampled)) { B->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,B->GlobalAfter); }
        }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<12;++i) { G->DataAfter[i]=data[i]; }
        B->Data12After=data[12];
        if (G->Prepared)
        {
            G->PixelMatches=0; G->SentinelMatches=0;
            for (ULONG i=0;i<256;++i)
            {
                const ULONG value=pixels[i];
                if (value==0x78563412) { ++G->PixelMatches; }
                else if (G->PixelFirstBad==0xFFFFFFFF) { G->PixelFirstBad=i; G->PixelBadValue=value; }
            }
            const ULONG indices[]={0,15,16,255};
            for (ULONG i=0;i<4;++i) { G->Pixels[i]=pixels[indices[i]]; }
            for (ULONG i=0;i<16;++i)
            { G->Sentinel[i]=sentinel[i]; if (G->Sentinel[i]==0xFFFFFFFF) { ++G->SentinelMatches; } }
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            G->Checks=(C->ShadowAfter==61 && C->Rptr==61 && C->WptrAfter==61 &&
                       !(C->IntStatus&errors) && !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23)) ? 1UL : 0) |
                      (B->Data12After==0xCAFE000A ? 2UL : 0) |
                      (G->PixelMatches==256 ? 4UL : 0) | (G->SentinelMatches==16 ? 8UL : 0);
        }
        if (NT_SUCCESS(st) && G->Checks!=0xF) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    }
    // CPU-only scan also runs after map/precondition failures; do not change memory while diagnosing it.
    KeMemoryBarrier(); B->ScanDwords=0;
    const ULONG colors[]={0x78563412,0x12345678,0x00000012};
    for (ULONG j=0;j<3;++j) { B->ScanCount[j]=0; }
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if (off<61*4 || (off>=0x6000 && off<0x6240) || (off>=0x7000 && off<0x7040) ||
            (off>=0x7400 && off<0x7840)) { continue; }
        ++B->ScanDwords; const ULONG value=words[off/4];
        for (ULONG j=0;j<3;++j) if (value==colors[j])
        { ++B->ScanCount[j]; if (B->ScanFirst[j]==0xFFFFFFFF) { B->ScanFirst[j]=off; } }
    }
    if (S->Smmu!=NULL)
    {
        ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
        C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
        if (NT_SUCCESS(C->FaultStatus))
        { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_Z1_TRACE {
    GMU_G5B_TRACE Gpu;
    ULONG Reserved;
    EPI_ZAP_TRACE Pas;
    ULONG Data12Before,Data12After;
};
static_assert(sizeof(EPI_ZAP_TRACE)==592,"Z-1 PAS diagnostic ABI");
static_assert(sizeof(GMU_Z1_TRACE)==1368,"Z-1 combined diagnostic ABI");
static_assert(offsetof(GMU_Z1_TRACE,Pas)==768,"Z-1c PAS snapshot offset must not change");
static const ULONG GmuZ1Packet[]={0x70E60001,0,0x70268000,0x70BF8003,0x70016100,0,36,
                                0x70460004,4,0x70017034,0,0xCAFE000B};
static VOID GmuZ1TraceInit(GMU_Z1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG5bTraceInit(&t->Gpu);
    t->Reserved=0; t->Pas.Status=STATUS_NOT_FOUND;
}
struct GMU_Z1_GUARD { GMU_TEST* S; HANDLE Params; volatile ULONG* Cc; const GMU_GX_TRACE* T; GMU_G2_TRACE* C; };
static NTSTATUS GmuZ1Checkpoint(PVOID Context,ULONG Step)
{
    GMU_Z1_GUARD* g=static_cast<GMU_Z1_GUARD*>(Context);
    return GmuG2Guard(g->S,g->Params,g->Cc,g->T,g->C,Step);
}
static BOOLEAN GmuZ1CanFollow(const GMU_G5B_TRACE* B)
{
    return B!=NULL && B->Draw.Prepared==1 && B->Draw.Cp.Attempted==1 && B->Draw.Checks==0xB &&
           B->Draw.Cp.Reason==13 && B->Draw.Cp.Status==STATUS_IO_DEVICE_ERROR;
}
static BOOLEAN GmuZ1PasSucceeded(const EPI_ZAP_TRACE* P)
{
    for (ULONG i=0;i<4;++i)
    {
        if (P->Call[i].Completed!=1 || P->Call[i].Result[0]!=0 || (i!=0 && P->Call[i].Result[1]!=0))
        { return FALSE; }
    }
    return TRUE;
}
static NTSTATUS GmuZ1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,
                              const GMU_GX_TRACE* T,GMU_Z1_TRACE* Z,HANDLE Diag,const UCHAR* Zap,ULONG ZapLen)
{
    GMU_G5B_TRACE* B=&Z->Gpu;
    GMU_G5_TRACE* G=&B->Draw;
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1500); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x7840 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    volatile ULONG* data=words+0x7000/4;
    volatile ULONG* pixels=words+0x7400/4;
    volatile ULONG* sentinel=words+0x7800/4;
    PVOID maps[4]={}; const ULONG offsets[]={0,0x2000,0x23000,0x3000};
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        volatile ULONG* ccu=static_cast<volatile ULONG*>(maps[2])+0x81C/4;
        st=GmuG2Guard(S,Params,Cc,T,C,0x1501);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,
                                        0xCAFE0003,0xFFFFFFFF,0xCAFE0004,0xFFFFFFFF,
                                        0xCAFE0006,0xCAFE0007,0xCAFE0008,0xCAFE0009};
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                BOOLEAN previous=C->ShadowBefore==61 && C->WptrBefore==61;
                for (ULONG i=0;i<12;++i)
                { G->DataBefore[i]=data[i]; if (G->DataBefore[i]!=expected[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i) { if (words[39+i]!=GmuG5Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<36;++i) { if (words[0x6100/4+i]!=GmuG5Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<256;++i) { if (pixels[i]!=0xFFFFFFFF) { previous=FALSE; } }
                for (ULONG i=0;i<16;++i) { if (sentinel[i]!=0xFFFFFFFF) { previous=FALSE; } }
                if (data[12]!=0xCAFE000A) { previous=FALSE; }
                for (ULONG i=0;i<13;++i) { if (words[48+i]!=GmuG5bPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<10;++i) { if (words[0x6200/4+i]!=GmuG5bIb[i]) { previous=FALSE; } }
                Z->Data12Before=data[12]; B->Data12Before=data[13];
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        volatile ULONG* magic[]={static_cast<volatile ULONG*>(maps[2])+0x810/4,
                                 static_cast<volatile ULONG*>(maps[2])+0x804/4,
                                 static_cast<volatile ULONG*>(maps[3])+0x848/4,
                                 static_cast<volatile ULONG*>(maps[3])+0x864/4};
        if (NT_SUCCESS(st)) for (ULONG i=0;i<4;++i)
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1502+i); if (!NT_SUCCESS(st)) { break; }
            B->MagicBefore[i]=READ_REGISTER_ULONG(magic[i]);
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1506);
            if (NT_SUCCESS(st)) { G->CcuBefore=READ_REGISTER_ULONG(ccu); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1507);
            if (NT_SUCCESS(st)) { B->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,B->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            GMU_Z1_GUARD guard={S,Params,Cc,T,C};
            st=EpiZapLoad(Params,Diag,Zap,ZapLen,&Z->Pas,GmuZ1Checkpoint,&guard);
            if (!NT_SUCCESS(st) && C->Reason==1) { C->Reason=16; }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1508);
            if (NT_SUCCESS(st) && (*rptr!=61 || READ_REGISTER_ULONG(cp+7)!=61))
            { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            if (NT_SUCCESS(st))
            {
                data[13]=0xFFFFFFFF;
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+61,GmuZ1Packet,sizeof(GmuZ1Packet));
                KeMemoryBarrier();
                RtlCopyMemory(B->Ib,static_cast<UCHAR*>(ring->Va)+0x6200,sizeof(B->Ib));
                RtlCopyMemory(B->Packet,static_cast<ULONG*>(ring->Va)+61,sizeof(GmuZ1Packet));
                RtlCopyMemory(G->Ib,static_cast<UCHAR*>(ring->Va)+0x6100,sizeof(G->Ib)); G->Prepared=1;
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1509);
            if (NT_SUCCESS(st))
            {
                G->MaskAtSubmit=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAtSubmit!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,73); KeMemoryBarrier(); ++C->WptrWrites; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x150A);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,73,gpu+0x38,&G->MaskAtSubmit); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x150B);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,73,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        volatile ULONG* diagRegs[]={ccu,cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&G->CcuAfter,&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,
                             &G->CpProtect,&G->CpStatus1,&C->RbbmStatus,&C->IntStatus,
                             &G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1510+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        for (ULONG i=0;i<4;++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1520+i); if (!NT_SUCCESS(sampled)) { break; }
            B->MagicAfter[i]=READ_REGISTER_ULONG(magic[i]);
        }
        if (NT_SUCCESS(sampled))
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1524);
            if (NT_SUCCESS(sampled)) { B->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,B->GlobalAfter); }
        }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<12;++i) { G->DataAfter[i]=data[i]; }
        Z->Data12After=data[12]; B->Data12After=data[13];
        if (G->Prepared)
        {
            G->PixelMatches=0; G->SentinelMatches=0;
            for (ULONG i=0;i<256;++i)
            {
                const ULONG value=pixels[i];
                if (value==0x78563412) { ++G->PixelMatches; }
                else if (G->PixelFirstBad==0xFFFFFFFF) { G->PixelFirstBad=i; G->PixelBadValue=value; }
            }
            const ULONG indices[]={0,15,16,255};
            for (ULONG i=0;i<4;++i) { G->Pixels[i]=pixels[indices[i]]; }
            for (ULONG i=0;i<16;++i)
            { G->Sentinel[i]=sentinel[i]; if (G->Sentinel[i]==0xFFFFFFFF) { ++G->SentinelMatches; } }
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            G->Checks=(GmuZ1PasSucceeded(&Z->Pas) ? 1UL : 0) |
                      (C->ShadowAfter==73 && C->Rptr==73 && C->WptrAfter==73 &&
                       !(C->IntStatus&errors) && !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23)) ? 2UL : 0) |
                      (B->Data12After==0xCAFE000B ? 4UL : 0) |
                      (G->PixelMatches==256 ? 8UL : 0) | (G->SentinelMatches==16 ? 16UL : 0);
        }
        if (NT_SUCCESS(st) && G->Checks!=0x1F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    }
    if (GmuZ1PasSucceeded(&Z->Pas)) { G->Checks|=1; }
    // CPU-only scan also runs after map/precondition failures; do not change memory while diagnosing it.
    KeMemoryBarrier(); B->ScanDwords=0;
    const ULONG colors[]={0x78563412,0x12345678,0x00000012};
    for (ULONG j=0;j<3;++j) { B->ScanCount[j]=0; }
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if (off<73*4 || (off>=0x6000 && off<0x6240) || (off>=0x7000 && off<0x7040) ||
            (off>=0x7400 && off<0x7840)) { continue; }
        ++B->ScanDwords; const ULONG value=words[off/4];
        for (ULONG j=0;j<3;++j) if (value==colors[j])
        { ++B->ScanCount[j]; if (B->ScanFirst[j]==0xFFFFFFFF) { B->ScanFirst[j]=off; } }
    }
    if (S->Smmu!=NULL)
    {
        ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
        C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
        if (NT_SUCCESS(C->FaultStatus))
        { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<RTL_NUMBER_OF(maps);++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_S1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG ZChecks,ZReason,Packet[9],Ib[53],Shader[32],OutputBefore[16],OutputAfter[16],Data14Before,Data14After;
    ULONG CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAtSubmit,MaskAfter,ScanCount,ScanFirst,ScanDwords,Checks,Prepared;
};
static_assert(sizeof(GMU_S1_TRACE)==756,"S-1 diagnostic ABI");
static const ULONG GmuS1Shader[32]={
    0x70017C00,0x204CC000, 0,0x204CC001, 0xC0DE5EED,0x204CC004,
    0,0x00000500, 0x01800008,0xC0C60100, 0,0x03000000
}; // Remaining ten instructions are zero/nop; all 64-bit instructions are low DWORD first.
static const ULONG GmuS1Ib[]={
    0x40AB0001,5, 0x48AE0401,0, 0x40BB0801,0xBF, 0x40B98701,0x101,
    0x48A9BB02,0x100,1, 0x40A9B001,0x80000100, 0x48A9B101,0x41, 0x48B9D001,0x41,
    0x48B99702,0xFCFCFCFC,0xFC, 0x40A9C202,0xFCFCFCFC,0xFC,
    0x40A9B383,0,0x70016800,0, 0x48AA0001,0,
    0x70348003,0x00760000,0x70016800,0, 0x70E50001,8,
    0x40B99007,3,1,0,1,0,1,0, 0x40B99983,1,1,1,
    0x70B30004,0,1,1,1, 0x70268000
};
static_assert(sizeof(GmuS1Ib)==53*sizeof(ULONG),"S-1 IB length");
static const ULONG GmuS1Packet[]={0x70BF8003,0x70016300,0,53,0x70460004,4,0x70017038,0,0xCAFE000C};
static VOID GmuS1TraceInit(GMU_S1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp); t->Checks=0; t->Prepared=0;
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
}
static BOOLEAN GmuS1CanFollow(const GMU_Z1_TRACE* Z)
{
    return Z!=NULL && NT_SUCCESS(Z->Gpu.Draw.Cp.Status) && Z->Gpu.Draw.Cp.Reason==0 &&
           Z->Gpu.Draw.Checks==0x1F && Z->Gpu.Draw.Cp.WptrAfter==73 && Z->Gpu.Data12After==0xCAFE000B;
}
static NTSTATUS GmuS1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_S1_TRACE* G)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1700); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    volatile ULONG* output=words+0x7C00/4;
    volatile ULONG* data=words+0x7000/4;
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0x1701);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                BOOLEAN previous=C->ShadowBefore==73 && C->WptrBefore==73 && C->Rptr==73 && data[13]==0xCAFE000B;
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i] || words[39+i]!=GmuG5Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<13;++i) { if (words[48+i]!=GmuG5bPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<12;++i) { if (words[61+i]!=GmuZ1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<36;++i) { if (words[0x6100/4+i]!=GmuG5Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<10;++i) { if (words[0x6200/4+i]!=GmuG5bIb[i]) { previous=FALSE; } }
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1702);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1703);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=0;i<16;++i) { output[i]=0xFFFFFFFF; G->OutputBefore[i]=output[i]; }
                data[14]=0xFFFFFFFF; G->Data14Before=data[14];
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x6300,GmuS1Ib,sizeof(GmuS1Ib));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x6800,GmuS1Shader,sizeof(GmuS1Shader));
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+73,GmuS1Packet,sizeof(GmuS1Packet));
                KeMemoryBarrier();
                RtlCopyMemory(G->Ib,static_cast<UCHAR*>(ring->Va)+0x6300,sizeof(G->Ib));
                RtlCopyMemory(G->Shader,static_cast<UCHAR*>(ring->Va)+0x6800,sizeof(G->Shader));
                RtlCopyMemory(G->Packet,static_cast<ULONG*>(ring->Va)+73,sizeof(G->Packet)); G->Prepared=1;
                for (ULONG i=0;i<53;++i) { if (G->Ib[i]!=GmuS1Ib[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<32;++i) { if (G->Shader[i]!=GmuS1Shader[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                if (!NT_SUCCESS(st)) { C->Reason=13; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1704);
            if (NT_SUCCESS(st))
            {
                G->MaskAtSubmit=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAtSubmit!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,82); KeMemoryBarrier(); ++C->WptrWrites; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1705);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,82,gpu+0x38,&G->MaskAtSubmit); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1706);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,82,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1710+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1720);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr; G->Data14After=data[14];
        for (ULONG i=0;i<16;++i) { G->OutputAfter[i]=output[i]; }
        if (G->Prepared)
        {
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (C->ShadowAfter==82 && C->Rptr==82 && C->WptrAfter==82 && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }
            if (G->Data14After==0xCAFE000C) { G->Checks|=4; }
            if (G->OutputAfter[0]==0xC0DE5EED) { G->Checks|=8; }
            BOOLEAN intact=TRUE;
            for (ULONG i=1;i<16;++i) { if (G->OutputAfter[i]!=0xFFFFFFFF) { intact=FALSE; } }
            if (intact) { G->Checks|=16; }
        }
        if (NT_SUCCESS(st) && G->Checks!=0x1F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    }
    KeMemoryBarrier(); G->ScanCount=0; G->ScanDwords=0;
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if (off<82*4 || (off>=0x6000 && off<0x6240) || (off>=0x6300 && off<0x6400) ||
            (off>=0x6800 && off<0x6880) || (off>=0x7000 && off<0x7040) ||
            (off>=0x7400 && off<0x7840) || (off>=0x7C00 && off<0x7C40)) { continue; }
        ++G->ScanDwords;
        if (words[off/4]==0xC0DE5EED) { ++G->ScanCount; if (G->ScanFirst==0xFFFFFFFF) { G->ScanFirst=off; } }
    }
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_R1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG SChecks,SReason,Packet[9],Vs[32],Fs[32],Vertices[8],IbBadCount,IbFirstBad,IbBadValue;
    ULONG Target[256],Sentinel[16],InsideMatches,OutsideMatches,WhiteCount,OtherCount,OtherFirst,OtherValue;
    ULONG DataBefore[2],DataAfter[2],CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAtSubmit,MaskAfter,ScanCount[2],ScanFirst[2],ScanDwords,Checks,Prepared;
};
static_assert(sizeof(GMU_R1_TRACE)==1716,"R-1 diagnostic ABI");
// Facts transcribed from section 9.21.34's 103-packet table and checked against the supplied DWORD list.
static const ULONG GmuR1Ib[]={
    0x70460001,0x00000031,0x70268000,0x40AE0001,0x00000006,0x40AE0302,0x00001400,0x00000000,
    0x40AE0F01,0x0000003F,0x40B60001,0x05008000,0x40B60501,0x00000044,0x48BE0002,0x00000080,
    0x00000000,0x40BE0401,0x00000000,0x48960001,0x02000000,0x40860001,0x00000000,0x48AB2001,
    0x00000000,0x48B18202,0x00000000,0x00000000,0x40A9A802,0x00000000,0x00000000,0x40AB0001,
    0x00000005,0x40A00783,0x00000000,0x00000000,0x00000001,0x48980402,0x0000001F,0x00000001,
    0x40811001,0x00000000,0x48881807,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x4888F001,0x00000000,0x40923601,0x00000000,0x40809901,0x00000000,
    0x4080AF01,0x00000000,0x40960201,0x00000000,0x409E7201,0x00000000,0x40B30901,0x000000A2,
    0x40886402,0x00000000,0xFFFF0000,0x70438003,0x00040000,0x00000000,0x00000000,0x4080F002,
    0x00000000,0x000F000F,0x48840A02,0x00000000,0x000F000F,0x48889002,0x00000000,0x00000001,
    0x4088D302,0x00000000,0x00000000,0x48B4D101,0x00000000,0x48B30701,0x00000000,0x48880004,
    0x00C00000,0x00000000,0x00000000,0x00000004,0x4080A004,0x00000002,0x00C00000,0x00000000,
    0x00000004,0x40B30002,0x00000000,0x00000004,0x70E50001,0x00000001,0x709D0001,0x00000000,
    0x70640001,0x00000001,0x70E30001,0x00000000,0x40BB0801,0x000000BF,0x48A82383,0x00000100,
    0x00000001,0x00000000,0x48A83B01,0x00000000,0x40A86301,0x00000000,0x48A89401,0x00000000,
    0x48AB0402,0x00000100,0x00000001,0x48B80004,0x00000101,0x00000000,0x00000000,0x00000000,
    0x40BB1002,0x00000101,0x00000000,0x40A80001,0x00100080,0x40A81B07,0x00000000,0x70014800,
    0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x70328003,0x00620000,0x70014800,
    0x00000000,0x40A98001,0x81000080,0x48A98207,0x00000000,0x70014880,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000000,0x70348003,0x00720000,0x70014880,0x00000000,0x409B0002,
    0x00000000,0x00000004,0x48A80202,0x00000001,0x00000F00,0x48A81301,0x00000000,0x48930002,
    0x00000000,0x00FF0004,0x48910101,0x00FFFF00,0x48931101,0x00FFFF00,0x40800002,0x00000381,
    0x00000000,0x48910401,0x0000FFFF,0x48931401,0x0000FFFF,0x48809B01,0x00000000,0x48910701,
    0x00000000,0x40920001,0x00000000,0x48920801,0x00000000,0x48921007,0x00000000,0x00000000,
    0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0x00000000,0x40930483,0xFF00FF00,0x00000000,
    0x00000001,0x40980601,0x00000000,0x48980801,0x00000000,0x40998001,0x00000000,0x489B0701,
    0x00000000,0x48A00007,0x00000101,0xFCFCFCFC,0x0000FCFC,0xFCFCFCFC,0x000000FC,0x0000FCFC,
    0x00000000,0x40A00E02,0x00000000,0x00000000,0x40A01004,0x70014900,0x00000000,0x00000020,
    0x00000010,0x48A09002,0xC8200000,0x00000001,0x40A0D001,0x0000000F,0x40A99E01,0x00000008,
    0x48B98001,0x00000000,0x40B98285,0x00000007,0xFCFCFCFC,0xFCFCFCFC,0xFCFCFCFC,0x0000FCFC,
    0x40800501,0x00000000,0x48880907,0x00000000,0x00000000,0x00000000,0x00000001,0x0000000F,
    0x00000000,0x00000000,0x40881002,0x00000000,0x00000010,0x48810002,0x00000000,0x00000000,
    0x48810901,0x00000000,0x40A98986,0x00000000,0x00000000,0x0000000F,0xFCFCFC00,0x00000001,
    0x00000000,0x48A99601,0x00000030,0x40809001,0x00000000,0x4880D002,0x00000000,0x7FFF7FFF,
    0x4880B002,0x00000000,0x7FFF7FFF,0x48809401,0x00000000,0x48809801,0x00000000,0x48811402,
    0x00000000,0x00000000,0x40882008,0x00000780,0x00000000,0x00000030,0x00000001,0x00000000,
    0x70014C00,0x00000000,0x00000000,0x40887083,0x00000000,0x00000000,0x00000000,0x40888002,
    0x00000000,0x00000000,0x48888783,0x00000000,0x00000000,0x00000000,0x40889801,0x00000000,
    0x70388003,0x00000088,0x00000001,0x00000002,0x70460004,0x0000001D,0x7001703C,0x00000000,
    0xCAFE000D,0x70268000,
};
static_assert(sizeof(GmuR1Ib)==314*sizeof(ULONG),"R-1 IB length");
static const ULONG GmuR1Vs[32]={0,0,0,0x03000000};
static const ULONG GmuR1Fs[32]={0x3F800000,0x204CC000,0x3E4CCCCD,0x204CC001,0,0x204CC002,
                              0,0x204CC003,0,0x500,0,0x03000000};
static const ULONG GmuR1Vertices[]={0x40800000,0x40800000,0,0x3F800000,0x41400000,0x41400000,0,0x3F800000};
static const ULONG GmuR1Packet[]={0x70BF8003,0x70014000,0,314,0x70460004,4,0x70017040,0,0xCAFE000E};
static VOID GmuR1TraceInit(GMU_R1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp); t->Checks=0; t->Prepared=0;
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
}
static BOOLEAN GmuR1CanFollow(const GMU_S1_TRACE* P)
{
    return P!=NULL && NT_SUCCESS(P->Cp.Status) && P->Cp.Reason==0 && P->Checks==0x1F &&
           P->Cp.WptrAfter==82 && P->Data14After==0xCAFE000C && P->OutputAfter[0]==0xC0DE5EED;
}
static NTSTATUS GmuR1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_R1_TRACE* G)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1800); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    volatile ULONG* pixels=words+0x4C00/4;
    volatile ULONG* sentinel=words+0x5000/4;
    volatile ULONG* data=words+0x7000/4;
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0x1801);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                BOOLEAN previous=C->ShadowBefore==82 && C->WptrBefore==82 && C->Rptr==82;
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i] || words[39+i]!=GmuG5Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<13;++i) { if (words[48+i]!=GmuG5bPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<12;++i) { if (words[61+i]!=GmuZ1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<36;++i) { if (words[0x6100/4+i]!=GmuG5Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<10;++i) { if (words[0x6200/4+i]!=GmuG5bIb[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i) { if (words[73+i]!=GmuS1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<53;++i) { if (words[0x6300/4+i]!=GmuS1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x6800/4+i]!=GmuS1Shader[i]) { previous=FALSE; } }
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,0xCAFE0003,0xFFFFFFFF,
                    0xCAFE0004,0xFFFFFFFF,0xCAFE0006,0xCAFE0007,0xCAFE0008,0xCAFE0009,0xCAFE000A,0xCAFE000B,0xCAFE000C};
                for (ULONG i=0;i<15;++i) { if (data[i]!=expected[i]) { previous=FALSE; } }
                if (words[0x7C00/4]!=0xC0DE5EED) { previous=FALSE; }
                for (ULONG i=1;i<16;++i) { if (words[0x7C00/4+i]!=0xFFFFFFFF) { previous=FALSE; } }
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1802);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1803);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=0;i<256;++i) { pixels[i]=0xFFFFFFFF; }
                for (ULONG i=0;i<16;++i) { sentinel[i]=0xFFFFFFFF; }
                for (ULONG i=0;i<2;++i) { data[15+i]=0xFFFFFFFF; G->DataBefore[i]=data[15+i]; }
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x4000,GmuR1Ib,sizeof(GmuR1Ib));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x4800,GmuR1Vs,sizeof(GmuR1Vs));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x4880,GmuR1Fs,sizeof(GmuR1Fs));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x4900,GmuR1Vertices,sizeof(GmuR1Vertices));
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+82,GmuR1Packet,sizeof(GmuR1Packet));
                KeMemoryBarrier();
                RtlCopyMemory(G->Vs,static_cast<UCHAR*>(ring->Va)+0x4800,sizeof(G->Vs));
                RtlCopyMemory(G->Fs,static_cast<UCHAR*>(ring->Va)+0x4880,sizeof(G->Fs));
                RtlCopyMemory(G->Vertices,static_cast<UCHAR*>(ring->Va)+0x4900,sizeof(G->Vertices));
                RtlCopyMemory(G->Packet,static_cast<ULONG*>(ring->Va)+82,sizeof(G->Packet)); G->Prepared=1;
                G->IbBadCount=0;
                for (ULONG i=0;i<314;++i)
                {
                    const ULONG v=words[0x4000/4+i];
                    if (v!=GmuR1Ib[i])
                    { ++G->IbBadCount; if (G->IbFirstBad==0xFFFFFFFF) { G->IbFirstBad=i; G->IbBadValue=v; } }
                }
                for (ULONG i=0;i<32;++i)
                { if (G->Vs[i]!=GmuR1Vs[i] || G->Fs[i]!=GmuR1Fs[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<8;++i) { if (G->Vertices[i]!=GmuR1Vertices[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                if (G->IbBadCount!=0) { st=STATUS_IO_DEVICE_ERROR; }
                if (!NT_SUCCESS(st)) { C->Reason=13; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1804);
            if (NT_SUCCESS(st))
            {
                G->MaskAtSubmit=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAtSubmit!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,91); KeMemoryBarrier(); ++C->WptrWrites; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1805);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,91,gpu+0x38,&G->MaskAtSubmit); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1806);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,91,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1810+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1820);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<2;++i) { G->DataAfter[i]=data[15+i]; }
        for (ULONG i=0;i<256;++i) { G->Target[i]=pixels[i]; }
        for (ULONG i=0;i<16;++i) { G->Sentinel[i]=sentinel[i]; }
        if (G->Prepared)
        {
            G->InsideMatches=0; G->OutsideMatches=0; G->WhiteCount=0; G->OtherCount=0;
            BOOLEAN outsideClean=TRUE,sentinelClean=TRUE;
            for (ULONG i=0;i<256;++i)
            {
                const ULONG value=G->Target[i],x=i%16,y=i/16;
                const BOOLEAN inside=x>=4 && x<12 && y>=4 && y<12;
                if (value==0x000033FF) { if (inside) { ++G->InsideMatches; } else { ++G->OutsideMatches; } }
                else if (value==0xFFFFFFFF) { ++G->WhiteCount; }
                else { ++G->OtherCount; if (G->OtherFirst==0xFFFFFFFF) { G->OtherFirst=i; G->OtherValue=value; } }
                if (!inside && value!=0xFFFFFFFF) { outsideClean=FALSE; }
            }
            for (ULONG i=0;i<16;++i) { if (G->Sentinel[i]!=0xFFFFFFFF) { sentinelClean=FALSE; } }
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (C->ShadowAfter==91 && C->Rptr==91 && C->WptrAfter==91 && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }
            if (G->DataAfter[0]==0xCAFE000D && G->DataAfter[1]==0xCAFE000E) { G->Checks|=4; }
            if (G->InsideMatches==64) { G->Checks|=8; }
            if (outsideClean && sentinelClean) { G->Checks|=16; }
        }
        if (NT_SUCCESS(st) && G->Checks!=0x1F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    }
    KeMemoryBarrier(); G->ScanCount[0]=0; G->ScanCount[1]=0; G->ScanDwords=0;
    const ULONG colors[]={0x000033FF,0xFF330000};
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if (off<91*4 || (off>=0x4000 && off<0x44E8) || (off>=0x4800 && off<0x4920) ||
            (off>=0x4C00 && off<0x5040) || (off>=0x6000 && off<0x6240) || (off>=0x6300 && off<0x6400) ||
            (off>=0x6800 && off<0x6880) || (off>=0x7000 && off<0x7044) ||
            (off>=0x7400 && off<0x7840) || (off>=0x7C00 && off<0x7C40)) { continue; }
        ++G->ScanDwords;
        const ULONG value=words[off/4];
        for (ULONG j=0;j<2;++j) if (value==colors[j])
        { ++G->ScanCount[j]; if (G->ScanFirst[j]==0xFFFFFFFF) { G->ScanFirst[j]=off; } }
    }
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_D1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG RChecks,RReason,Packet[9],Fs[32],VertA[8],VertB[8],IbBadCount,IbFirstBad,IbBadValue,Disp[6];
    ULONG InsideEven,InsideOdd,InsideOther,InsideFirst,InsideValue,OutsideChanged,OutsideFirst,OutsideOld,OutsideNew,Edge[12];
    ULONG DataBefore[3],DataAfter[3],CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAtSubmit,MaskAfter,ScanCount,ScanFirst,ScanDwords;
    ULONG RestoreMismatch,RestoreFirst,BandMapped,HoldMs,Checks,Prepared;
    NTSTATUS BackupStatus,HoldStatus,RestoreStatus;
};
static_assert(sizeof(GMU_D1_TRACE)==644,"D-1 diagnostic ABI");
static const ULONG GmuD1Ib[]={
    0x40BB0801,0x000000BF,0x48A98207,0x00000000,0x70014A00,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x70348003,0x00720000,0x70014A00,0x00000000,0x4080F002,0x0000019C,
    0x007F029B,0x48840A02,0x0000019C,0x007F029B,0x4880B002,0x0000019C,0x007F029B,0x40A01004,
    0x70014A80,0x00000000,0x00000020,0x00000010,0x40882304,0x00000087,0x00000000,0x71008A00,
    0x00000000,0x70388003,0x00000088,0x00000001,0x00000002,0x70460004,0x0000001D,0x70017044,
    0x00000000,0xCAFE000F,0x70268000,0x4080F002,0x000001A4,0x007F02A3,0x48840A02,0x000001A4,
    0x007F02A3,0x4880B002,0x000001A4,0x007F02A3,0x40A01004,0x70014AA0,0x00000000,0x00000020,
    0x00000010,0x40882304,0x00000087,0x00000000,0x71009AC0,0x00000000,0x70388003,0x00000088,
    0x00000001,0x00000002,0x70460004,0x0000001D,0x70017048,0x00000000,0xCAFE0010,0x70268000,
};
static_assert(sizeof(GmuD1Ib)==72*sizeof(ULONG),"D-1 IB length");
static const ULONG GmuD1Fs[32]={0,0x204CC000,0x3E4CCCCD,0x204CC001,0x3F800000,0x204CC002,
                              0x3F800000,0x204CC003,0,0x500,0,0x03000000};
static const ULONG GmuD1VertA[]={0x43CE0000,0,0,0x3F800000,0x44270000,0x43000000,0,0x3F800000};
static const ULONG GmuD1VertB[]={0x43D20000,0,0,0x3F800000,0x44290000,0x43000000,0,0x3F800000};
static const ULONG GmuD1Packet[]={0x70BF8003,0x70014500,0,72,0x70460004,4,0x7001704C,0,0xCAFE0011};
static VOID GmuD1TraceInit(GMU_D1_TRACE* t, const ULONG* Disp)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp); t->Checks=0; t->Prepared=0;
    t->BandMapped=0; t->HoldMs=0; t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
    t->BackupStatus=t->HoldStatus=t->RestoreStatus=STATUS_NOT_FOUND;
    if (Disp!=NULL) { RtlCopyMemory(t->Disp,Disp,sizeof(t->Disp)); }
}
static BOOLEAN GmuD1CanFollow(const GMU_R1_TRACE* P)
{
    return P!=NULL && NT_SUCCESS(P->Cp.Status) && P->Cp.Reason==0 && P->Checks==0x1F &&
           P->Cp.WptrAfter==91 && P->DataAfter[0]==0xCAFE000D && P->DataAfter[1]==0xCAFE000E && P->InsideMatches==64;
}
static NTSTATUS GmuD1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_D1_TRACE* G,HANDLE Diag)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1900); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    PVOID band=NULL; ULONG* backup=NULL;
    volatile ULONG* data=words+0x7000/4;
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0x1901);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                BOOLEAN previous=C->ShadowBefore==91 && C->WptrBefore==91 && C->Rptr==91;
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i] || words[39+i]!=GmuG5Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<13;++i) { if (words[48+i]!=GmuG5bPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<12;++i) { if (words[61+i]!=GmuZ1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<36;++i) { if (words[0x6100/4+i]!=GmuG5Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<10;++i) { if (words[0x6200/4+i]!=GmuG5bIb[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i) { if (words[73+i]!=GmuS1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<53;++i) { if (words[0x6300/4+i]!=GmuS1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x6800/4+i]!=GmuS1Shader[i]) { previous=FALSE; } }
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,0xCAFE0003,0xFFFFFFFF,
                    0xCAFE0004,0xFFFFFFFF,0xCAFE0006,0xCAFE0007,0xCAFE0008,0xCAFE0009,0xCAFE000A,0xCAFE000B,0xCAFE000C};
                for (ULONG i=0;i<15;++i) { if (data[i]!=expected[i]) { previous=FALSE; } }
                if (words[0x7C00/4]!=0xC0DE5EED) { previous=FALSE; }
                for (ULONG i=1;i<16;++i) { if (words[0x7C00/4+i]!=0xFFFFFFFF) { previous=FALSE; } }
                if (!EpiGmuD1DisplayMatches(G->Disp)) { previous=FALSE; }
                GMU_REGION* ext=GmuFind(S,0x71000000,0x120000);
                if (ext==NULL || !ext->External || !ext->Mapped || ext->Pa.QuadPart!=0xE1462000 ||
                    ext->Size!=0x120000 || ext->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE)) { previous=FALSE; }
                if (data[15]!=0xCAFE000D || data[16]!=0xCAFE000E) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[82+i]!=GmuR1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<314;++i) { if (words[0x4000/4+i]!=GmuR1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i)
                { if (words[0x4800/4+i]!=GmuR1Vs[i] || words[0x4880/4+i]!=GmuR1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<8;++i) { if (words[0x4900/4+i]!=GmuR1Vertices[i]) { previous=FALSE; } }
                for (ULONG i=0;i<256;++i)
                {
                    const BOOLEAN inside=i%16>=4 && i%16<12 && i/16>=4 && i/16<12;
                    if (words[0x4C00/4+i]!=(inside ? 0x000033FFUL : 0xFFFFFFFFUL)) { previous=FALSE; }
                }
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1902);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1908);
            if (NT_SUCCESS(st))
            {
                backup=static_cast<ULONG*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,0x120000,EPI_GMU_TAG));
                if (backup==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                else
                {
                    PHYSICAL_ADDRESS pa; pa.QuadPart=0xE1462000;
                    band=MmMapIoSpaceEx(pa,0x120000,PAGE_READWRITE|PAGE_NOCACHE);
                    if (band==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                    else
                    {
                        G->BandMapped=1;
                        for (ULONG i=0;i<0x120000/4;++i) { backup[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i); }
                        KeMemoryBarrier();
                    }
                }
                G->BackupStatus=st;
                if (!NT_SUCCESS(st)) { C->Reason=GX_REG_MAP_FAILED; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1903);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=0;i<3;++i) { data[17+i]=0xFFFFFFFF; G->DataBefore[i]=data[17+i]; }
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x4500,GmuD1Ib,sizeof(GmuD1Ib));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x4A00,GmuD1Fs,sizeof(GmuD1Fs));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x4A80,GmuD1VertA,sizeof(GmuD1VertA));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x4AA0,GmuD1VertB,sizeof(GmuD1VertB));
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+91,GmuD1Packet,sizeof(GmuD1Packet));
                KeMemoryBarrier();
                RtlCopyMemory(G->Fs,static_cast<UCHAR*>(ring->Va)+0x4A00,sizeof(G->Fs));
                RtlCopyMemory(G->VertA,static_cast<UCHAR*>(ring->Va)+0x4A80,sizeof(G->VertA));
                RtlCopyMemory(G->VertB,static_cast<UCHAR*>(ring->Va)+0x4AA0,sizeof(G->VertB));
                RtlCopyMemory(G->Packet,static_cast<ULONG*>(ring->Va)+91,sizeof(G->Packet)); G->Prepared=1;
                G->IbBadCount=0;
                for (ULONG i=0;i<3;++i) { if (G->DataBefore[i]!=0xFFFFFFFF) { st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<72;++i)
                {
                    const ULONG v=words[0x4500/4+i];
                    if (v!=GmuD1Ib[i])
                    { ++G->IbBadCount; if (G->IbFirstBad==0xFFFFFFFF) { G->IbFirstBad=i; G->IbBadValue=v; } }
                }
                for (ULONG i=0;i<32;++i) { if (G->Fs[i]!=GmuD1Fs[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<8;++i)
                { if (G->VertA[i]!=GmuD1VertA[i] || G->VertB[i]!=GmuD1VertB[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<9;++i) { if (G->Packet[i]!=GmuD1Packet[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                if (G->IbBadCount!=0) { st=STATUS_IO_DEVICE_ERROR; }
                if (!NT_SUCCESS(st)) { C->Reason=13; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1904);
            if (NT_SUCCESS(st))
            {
                G->MaskAtSubmit=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAtSubmit!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,100); KeMemoryBarrier(); ++C->WptrWrites; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1905);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,100,gpu+0x38,&G->MaskAtSubmit); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1906);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,100,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1910+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1920);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<3;++i) { G->DataAfter[i]=data[17+i]; }
        if (G->Prepared)
        {
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (C->ShadowAfter==100 && C->Rptr==100 && C->WptrAfter==100 && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }
            if (G->DataAfter[0]==0xCAFE000F && G->DataAfter[1]==0xCAFE0010 && G->DataAfter[2]==0xCAFE0011) { G->Checks|=4; }
        }
        if (band!=NULL && NT_SUCCESS(G->BackupStatus))
        {
            G->InsideEven=0; G->InsideOdd=0; G->InsideOther=0; G->OutsideChanged=0;
            for (ULONG i=0;i<0x120000/4;++i)
            {
                const ULONG offset=i*4,fb=0x462000+offset,y=fb/4320,x=(fb%4320)/4;
                const ULONG value=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i);
                if (x>=412 && x<668 && y>=1072 && y<1328)
                {
                    if (value==0xFFFF3300) { if (y&1) { ++G->InsideOdd; } else { ++G->InsideEven; } }
                    else { ++G->InsideOther; if (G->InsideFirst==0xFFFFFFFF) { G->InsideFirst=offset; G->InsideValue=value; } }
                }
                else if (value!=backup[i])
                {
                    ++G->OutsideChanged;
                    if (G->OutsideFirst==0xFFFFFFFF) { G->OutsideFirst=offset; G->OutsideOld=backup[i]; G->OutsideNew=value; }
                }
            }
            const ULONG edge[][2]={{411,1072},{412,1072},{667,1072},{668,1072},{412,1071},{412,1327},
                                   {412,1328},{667,1327},{412,1073},{667,1073},{411,1073},{668,1073}};
            for (ULONG i=0;i<12;++i)
            { G->Edge[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+(edge[i][1]*4320+edge[i][0]*4-0x462000)/4); }
            if (G->InsideEven==32768 && G->InsideOdd==32768) { G->Checks|=8; }
            if (G->OutsideChanged==0) { G->Checks|=16; }
        }
    }
    KeMemoryBarrier(); G->ScanCount=0; G->ScanDwords=0;
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if (off<100*4 || (off>=0x4500 && off<0x4620) || (off>=0x4A00 && off<0x4AC0) || (off>=0x4000 && off<0x44E8) || (off>=0x4800 && off<0x4920) ||
            (off>=0x4C00 && off<0x5040) || (off>=0x6000 && off<0x6240) || (off>=0x6300 && off<0x6400) ||
            (off>=0x6800 && off<0x6880) || (off>=0x7000 && off<0x7050) ||
            (off>=0x7400 && off<0x7840) || (off>=0x7C00 && off<0x7C40)) { continue; }
        ++G->ScanDwords;
        if (words[off/4]==0xFFFF3300) { ++G->ScanCount; if (G->ScanFirst==0xFFFFFFFF) { G->ScanFirst=off; } }
    }
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (band!=NULL && NT_SUCCESS(G->BackupStatus))
    {
        if (C->Attempted)
        {
            C->Status=st;
            NTSTATUS held=GmuStep(Params,0x1930);
            if (NT_SUCCESS(held)) { held=EpiSetValue(Diag,L"GpuGmuD1",REG_BINARY,G,sizeof(*G)); }
            if (NT_SUCCESS(held)) { held=ZwFlushKey(Diag); }
            if (NT_SUCCESS(held))
            {
#ifdef EPI_GMU_HOST_TEST
                KeStallExecutionProcessor(5000000); held=STATUS_SUCCESS;
#else
                LARGE_INTEGER delay; delay.QuadPart=-50000000;
                held=KeDelayExecutionThread(KernelMode,FALSE,&delay);
#endif
                if (NT_SUCCESS(held)) { G->HoldMs=5000; }
            }
            G->HoldStatus=held;
            if (NT_SUCCESS(st) && !NT_SUCCESS(held)) { st=held; C->Reason=6; }
        }
        // Restoring display RAM is mandatory even when a breadcrumb or GX prerequisite failed.
        NTSTATUS restored=GmuStep(Params,0x1931);
        for (ULONG i=0;i<0x120000/4;++i) { WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i,backup[i]); }
        KeMemoryBarrier();
        const NTSTATUS verified=GmuStep(Params,0x1932);
        G->RestoreMismatch=0;
        for (ULONG i=0;i<0x120000/4;++i) if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i)!=backup[i])
        { ++G->RestoreMismatch; if (G->RestoreFirst==0xFFFFFFFF) { G->RestoreFirst=i*4; } }
        if (NT_SUCCESS(restored) && !NT_SUCCESS(verified)) { restored=verified; }
        if (G->RestoreMismatch==0) { G->Checks|=32; }
        else if (NT_SUCCESS(restored)) { restored=STATUS_IO_DEVICE_ERROR; }
        G->RestoreStatus=restored;
        if (NT_SUCCESS(st) && !NT_SUCCESS(restored)) { st=restored; C->Reason=13; }
    }
    if (band!=NULL) { MmUnmapIoSpace(band,0x120000); }
    if (backup!=NULL) { ExFreePoolWithTag(backup,EPI_GMU_TAG); }
    if (NT_SUCCESS(st) && G->Checks!=0x3F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_V1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG DChecks,DReason,Packet[9],Fs[32],VertA[24],VertB[24],IbBadCount,IbFirstBad,IbBadValue;
    ULONG CoreEven,CoreOdd,CoreBad,CoreFirst,CoreValue,CoreExpected,MaxErr[3],EdgeChanged,EdgeSame;
    ULONG OutsideChanged,OutsideFirst,OutsideOld,OutsideNew,Sample[12];
    ULONG DataBefore[3],DataAfter[3],CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAtSubmit,MaskAfter,ScanCount,ScanFirst,ScanDwords;
    ULONG RestoreMismatch,RestoreFirst,HoldMs,Checks,Prepared;
    NTSTATUS BackupStatus,HoldStatus,RestoreStatus;
};
static_assert(sizeof(GMU_V1_TRACE)==768,"V-1 diagnostic ABI");
static const ULONG GmuV1Ib[]={
    0x40BB0801,0x000000BF,0x40A80001,0x00100100,0x70328003,0x00620000,0x70014800,0x00000000,
    0x40A98001,0x81400100,0x48A98207,0x00000000,0x70015400,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x70348003,0x00720000,0x70015400,0x00000000,0x48A00001,0x00000201,
    0x40A09202,0xC8200200,0x00000001,0x48A0D101,0x0000004F,0x48A80202,0x00000002,0x0F000F04,
    0x48A81301,0x00000400,0x40930101,0x00FF0408,0x489B0101,0x00000008,0x40921204,0xFFFFFFF0,
    0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0x40930401,0xFF01FF04,0x40A99E01,0x00000000,0x48B98001,
    0x00000002,0x40B98401,0xFCFCFC00,0x40800501,0x00000001,0x48880901,0x00000401,0x48A98E01,
    0x00000004,0x4080F002,0x0000019C,0x007F029B,0x48840A02,0x0000019C,0x007F029B,0x4880B002,
    0x0000019C,0x007F029B,0x40A01004,0x70015480,0x00000000,0x00000060,0x00000020,0x40882304,
    0x00000087,0x00000000,0x71008A00,0x00000000,0x70388003,0x00000084,0x00000001,0x00000003,
    0x70460004,0x0000001D,0x70017050,0x00000000,0xCAFE0012,0x70268000,0x4080F002,0x000001A4,
    0x007F02A3,0x48840A02,0x000001A4,0x007F02A3,0x4880B002,0x000001A4,0x007F02A3,0x40A01004,
    0x70015500,0x00000000,0x00000060,0x00000020,0x40882304,0x00000087,0x00000000,0x71009AC0,
    0x00000000,0x70388003,0x00000084,0x00000001,0x00000003,0x70460004,0x0000001D,0x70017054,
    0x00000000,0xCAFE0013,0x70268000,
};
static_assert(sizeof(GmuV1Ib)==115*sizeof(ULONG),"V-1 IB length");
static const ULONG GmuV1Fs[32]={0x2000,0x47308B04,0,0x500,0,0x03000000};
static const ULONG GmuV1VertA[]={
    0x44070000,0,0,0x3F800000,0,0,0x3F800000,0x3F800000,
    0x43CE0000,0x43000000,0,0x3F800000,0,0x3F800000,0,0x3F800000,
    0x44270000,0x43000000,0,0x3F800000,0x3F800000,0,0,0x3F800000
};
static const ULONG GmuV1VertB[]={
    0x44090000,0,0,0x3F800000,0,0,0x3F800000,0x3F800000,
    0x43D20000,0x43000000,0,0x3F800000,0,0x3F800000,0,0x3F800000,
    0x44290000,0x43000000,0,0x3F800000,0x3F800000,0,0,0x3F800000
};
static const ULONG GmuV1Packet[]={0x70BF8003,0x70015100,0,115,0x70460004,4,0x70017058,0,0xCAFE0014};
// Signed edge functions after reversing the triangle winding; coordinates are doubled.
// With x in [822,1338], y in [-2,258], all intermediates including 510*w fit LONG.
static VOID GmuV1Weights(LONG X,LONG Y,LONG* W)
{
    W[0]=512*(256-Y); W[1]=256*(1080+Y-X); W[2]=256*(X+Y-1080);
}
enum { V1_EDGE=0,V1_CORE_IN=1,V1_CORE_OUT=2 };
static ULONG GmuV1Classify(ULONG Column,ULONG ViewRow)
{
    const LONG x=static_cast<LONG>(Column)*2,y=static_cast<LONG>(ViewRow)*2;
    ULONG allOutside=7; BOOLEAN allInside=TRUE;
    for (ULONG corner=0;corner<4;++corner)
    {
        LONG w[3]; GmuV1Weights(x+((corner&1) ? 4 : -2),y+((corner&2) ? 4 : -2),w);
        for (ULONG e=0;e<3;++e)
        { if (w[e]>=0) { allOutside&=~(1UL<<e); } else { allInside=FALSE; } }
    }
    return allInside ? V1_CORE_IN : (allOutside ? V1_CORE_OUT : V1_EDGE);
}
static ULONG GmuV1Expected(ULONG Column,ULONG ViewRow)
{
    LONG w[3]; GmuV1Weights(static_cast<LONG>(Column)*2+1,static_cast<LONG>(ViewRow)*2+1,w);
    const ULONG r=static_cast<ULONG>((510*w[0]+131072)/262144);
    const ULONG g=static_cast<ULONG>((510*w[1]+131072)/262144);
    const ULONG b=static_cast<ULONG>((510*w[2]+131072)/262144);
    return 0xFF000000UL|(r<<16)|(g<<8)|b;
}
static BOOLEAN GmuV1WithinTolerance(ULONG Value,ULONG Expected,ULONG* MaxErr)
{
    BOOLEAN good=(Value>>24)==0xFF;
    for (ULONG channel=0;channel<3;++channel)
    {
        const LONG delta=static_cast<LONG>((Value>>(channel*8))&0xFF)-static_cast<LONG>((Expected>>(channel*8))&0xFF);
        const ULONG error=static_cast<ULONG>(delta<0 ? -delta : delta);
        if (error>MaxErr[channel]) { MaxErr[channel]=error; }
        if (error>2) { good=FALSE; }
    }
    return good;
}
static VOID GmuV1TraceInit(GMU_V1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp); t->Checks=0; t->Prepared=0; t->HoldMs=0;
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
    t->BackupStatus=t->HoldStatus=t->RestoreStatus=STATUS_NOT_FOUND;
}
static BOOLEAN GmuV1CanFollow(const GMU_D1_TRACE* D)
{
    return D!=NULL && NT_SUCCESS(D->Cp.Status) && D->Cp.Reason==0 && D->Checks==0x3F && D->Cp.WptrAfter==100 &&
           D->DataAfter[0]==0xCAFE000F && D->DataAfter[1]==0xCAFE0010 && D->DataAfter[2]==0xCAFE0011 &&
           D->RestoreMismatch==0 && NT_SUCCESS(D->RestoreStatus);
}
static NTSTATUS GmuV1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_V1_TRACE* G,HANDLE Diag,const GMU_D1_TRACE* D)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1A00); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    PVOID band=NULL; ULONG* backup=NULL;
    volatile ULONG* data=words+0x7000/4;
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0x1A01);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                BOOLEAN previous=C->ShadowBefore==100 && C->WptrBefore==100 && C->Rptr==100;
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i] || words[39+i]!=GmuG5Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<13;++i) { if (words[48+i]!=GmuG5bPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<12;++i) { if (words[61+i]!=GmuZ1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<36;++i) { if (words[0x6100/4+i]!=GmuG5Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<10;++i) { if (words[0x6200/4+i]!=GmuG5bIb[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i) { if (words[73+i]!=GmuS1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<53;++i) { if (words[0x6300/4+i]!=GmuS1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x6800/4+i]!=GmuS1Shader[i]) { previous=FALSE; } }
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,0xCAFE0003,0xFFFFFFFF,
                    0xCAFE0004,0xFFFFFFFF,0xCAFE0006,0xCAFE0007,0xCAFE0008,0xCAFE0009,0xCAFE000A,0xCAFE000B,0xCAFE000C};
                for (ULONG i=0;i<15;++i) { if (data[i]!=expected[i]) { previous=FALSE; } }
                if (words[0x7C00/4]!=0xC0DE5EED) { previous=FALSE; }
                for (ULONG i=1;i<16;++i) { if (words[0x7C00/4+i]!=0xFFFFFFFF) { previous=FALSE; } }
                if (!(D!=NULL && EpiGmuD1DisplayMatches(D->Disp))) { previous=FALSE; }
                GMU_REGION* ext=GmuFind(S,0x71000000,0x120000);
                if (ext==NULL || !ext->External || !ext->Mapped || ext->Pa.QuadPart!=0xE1462000 ||
                    ext->Size!=0x120000 || ext->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE)) { previous=FALSE; }
                if (data[15]!=0xCAFE000D || data[16]!=0xCAFE000E) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[82+i]!=GmuR1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<314;++i) { if (words[0x4000/4+i]!=GmuR1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i)
                { if (words[0x4800/4+i]!=GmuR1Vs[i] || words[0x4880/4+i]!=GmuR1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<8;++i) { if (words[0x4900/4+i]!=GmuR1Vertices[i]) { previous=FALSE; } }
                for (ULONG i=0;i<256;++i)
                {
                    const BOOLEAN inside=i%16>=4 && i%16<12 && i/16>=4 && i/16<12;
                    if (words[0x4C00/4+i]!=(inside ? 0x000033FFUL : 0xFFFFFFFFUL)) { previous=FALSE; }
                }
                if (data[17]!=0xCAFE000F || data[18]!=0xCAFE0010 || data[19]!=0xCAFE0011) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[91+i]!=GmuD1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<72;++i) { if (words[0x4500/4+i]!=GmuD1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x4A00/4+i]!=GmuD1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<8;++i)
                { if (words[0x4A80/4+i]!=GmuD1VertA[i] || words[0x4AA0/4+i]!=GmuD1VertB[i]) { previous=FALSE; } }
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1A02);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1A08);
            if (NT_SUCCESS(st))
            {
                backup=static_cast<ULONG*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,0x120000,EPI_GMU_TAG));
                if (backup==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                else
                {
                    PHYSICAL_ADDRESS pa; pa.QuadPart=0xE1462000;
                    band=MmMapIoSpaceEx(pa,0x120000,PAGE_READWRITE|PAGE_NOCACHE);
                    if (band==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                    else
                    {
                        for (ULONG i=0;i<0x120000/4;++i) { backup[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i); }
                        KeMemoryBarrier();
                    }
                }
                G->BackupStatus=st;
                if (!NT_SUCCESS(st)) { C->Reason=GX_REG_MAP_FAILED; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1A03);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=0;i<3;++i) { data[20+i]=0xFFFFFFFF; G->DataBefore[i]=data[20+i]; }
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x5100,GmuV1Ib,sizeof(GmuV1Ib));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x5400,GmuV1Fs,sizeof(GmuV1Fs));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x5480,GmuV1VertA,sizeof(GmuV1VertA));
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x5500,GmuV1VertB,sizeof(GmuV1VertB));
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+100,GmuV1Packet,sizeof(GmuV1Packet));
                KeMemoryBarrier();
                RtlCopyMemory(G->Fs,static_cast<UCHAR*>(ring->Va)+0x5400,sizeof(G->Fs));
                RtlCopyMemory(G->VertA,static_cast<UCHAR*>(ring->Va)+0x5480,sizeof(G->VertA));
                RtlCopyMemory(G->VertB,static_cast<UCHAR*>(ring->Va)+0x5500,sizeof(G->VertB));
                RtlCopyMemory(G->Packet,static_cast<ULONG*>(ring->Va)+100,sizeof(G->Packet)); G->Prepared=1;
                G->IbBadCount=0;
                for (ULONG i=0;i<3;++i) { if (G->DataBefore[i]!=0xFFFFFFFF) { st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<115;++i)
                {
                    const ULONG v=words[0x5100/4+i];
                    if (v!=GmuV1Ib[i])
                    { ++G->IbBadCount; if (G->IbFirstBad==0xFFFFFFFF) { G->IbFirstBad=i; G->IbBadValue=v; } }
                }
                for (ULONG i=0;i<32;++i) { if (G->Fs[i]!=GmuV1Fs[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<24;++i)
                { if (G->VertA[i]!=GmuV1VertA[i] || G->VertB[i]!=GmuV1VertB[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<9;++i) { if (G->Packet[i]!=GmuV1Packet[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                if (G->IbBadCount!=0) { st=STATUS_IO_DEVICE_ERROR; }
                if (!NT_SUCCESS(st)) { C->Reason=13; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1A04);
            if (NT_SUCCESS(st))
            {
                G->MaskAtSubmit=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAtSubmit!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else { C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,109); KeMemoryBarrier(); ++C->WptrWrites; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1A05);
            if (NT_SUCCESS(st)) { st=GmuCpFenceWait(S,Cc,T,C,cp+7,109,gpu+0x38,&G->MaskAtSubmit); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1A06);
            if (NT_SUCCESS(st)) { st=GmuCpIdleWait(S,Cc,T,C,rptr,gpu,109,(1UL<<1)|(1UL<<9)|(1UL<<24)); }
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1A10+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1A20);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<3;++i) { G->DataAfter[i]=data[20+i]; }
        if (G->Prepared)
        {
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (C->ShadowAfter==109 && C->Rptr==109 && C->WptrAfter==109 && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }
            if (G->DataAfter[0]==0xCAFE0012 && G->DataAfter[1]==0xCAFE0013 && G->DataAfter[2]==0xCAFE0014) { G->Checks|=4; }
        }
        if (band!=NULL && NT_SUCCESS(G->BackupStatus))
        {
            G->CoreEven=0; G->CoreOdd=0; G->CoreBad=0; G->EdgeChanged=0; G->EdgeSame=0; G->OutsideChanged=0;
            for (ULONG channel=0;channel<3;++channel) { G->MaxErr[channel]=0; }
            for (ULONG i=0;i<0x120000/4;++i)
            {
                const ULONG offset=i*4,fb=0x462000+offset,y=fb/4320,x=(fb%4320)/4;
                const ULONG value=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i);
                const BOOLEAN inBox=x>=412 && x<668 && y>=1072 && y<1328;
                const ULONG kind=inBox ? GmuV1Classify(x,(y-1072)/2) : V1_CORE_OUT;
                if (kind==V1_CORE_IN)
                {
                    const ULONG expected=GmuV1Expected(x,(y-1072)/2);
                    if (GmuV1WithinTolerance(value,expected,G->MaxErr))
                    { if (y&1) { ++G->CoreOdd; } else { ++G->CoreEven; } }
                    else
                    {
                        ++G->CoreBad;
                        if (G->CoreFirst==0xFFFFFFFF) { G->CoreFirst=offset; G->CoreValue=value; G->CoreExpected=expected; }
                    }
                }
                else if (kind==V1_EDGE)
                { if (value==backup[i]) { ++G->EdgeSame; } else { ++G->EdgeChanged; } }
                else if (value!=backup[i])
                {
                    ++G->OutsideChanged;
                    if (G->OutsideFirst==0xFFFFFFFF) { G->OutsideFirst=offset; G->OutsideOld=backup[i]; G->OutsideNew=value; }
                }
            }
            const ULONG sample[][2]={{540,85},{540,6},{420,124},{660,124},{540,124},{476,100}};
            for (ULONG i=0;i<6;++i) for (ULONG parity=0;parity<2;++parity)
            { G->Sample[i*2+parity]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+((1072+sample[i][1]*2+parity)*4320+sample[i][0]*4-0x462000)/4); }
            if (G->CoreEven==15500 && G->CoreOdd==15500) { G->Checks|=8; }
            if (G->OutsideChanged==0) { G->Checks|=16; }
        }
    }
    KeMemoryBarrier(); G->ScanCount=0; G->ScanDwords=0;
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if (off<109*4 || (off>=0x5100 && off<0x52CC) || (off>=0x5400 && off<0x5560) || (off>=0x4500 && off<0x4620) || (off>=0x4A00 && off<0x4AC0) || (off>=0x4000 && off<0x44E8) || (off>=0x4800 && off<0x4920) ||
            (off>=0x4C00 && off<0x5040) || (off>=0x6000 && off<0x6240) || (off>=0x6300 && off<0x6400) ||
            (off>=0x6800 && off<0x6880) || (off>=0x7000 && off<0x705C) ||
            (off>=0x7400 && off<0x7840) || (off>=0x7C00 && off<0x7C40)) { continue; }
        ++G->ScanDwords;
        const ULONG value=words[off/4];
        if ((value&0xFF000000UL)==0xFF000000UL && value!=0xFFFFFFFF) { ++G->ScanCount; if (G->ScanFirst==0xFFFFFFFF) { G->ScanFirst=off; } }
    }
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (band!=NULL && NT_SUCCESS(G->BackupStatus))
    {
        if (C->Attempted)
        {
            C->Status=st;
            NTSTATUS held=GmuStep(Params,0x1A30);
            if (NT_SUCCESS(held)) { held=EpiSetValue(Diag,L"GpuGmuV1",REG_BINARY,G,sizeof(*G)); }
            if (NT_SUCCESS(held)) { held=ZwFlushKey(Diag); }
            if (NT_SUCCESS(held))
            {
#ifdef EPI_GMU_HOST_TEST
                KeStallExecutionProcessor(5000000); held=STATUS_SUCCESS;
#else
                LARGE_INTEGER delay; delay.QuadPart=-50000000;
                held=KeDelayExecutionThread(KernelMode,FALSE,&delay);
#endif
                if (NT_SUCCESS(held)) { G->HoldMs=5000; }
            }
            G->HoldStatus=held;
            if (NT_SUCCESS(st) && !NT_SUCCESS(held)) { st=held; C->Reason=6; }
        }
        // Restoring display RAM is mandatory even when a breadcrumb or GX prerequisite failed.
        NTSTATUS restored=GmuStep(Params,0x1A31);
        for (ULONG i=0;i<0x120000/4;++i) { WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i,backup[i]); }
        KeMemoryBarrier();
        const NTSTATUS verified=GmuStep(Params,0x1A32);
        G->RestoreMismatch=0;
        for (ULONG i=0;i<0x120000/4;++i) if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i)!=backup[i])
        { ++G->RestoreMismatch; if (G->RestoreFirst==0xFFFFFFFF) { G->RestoreFirst=i*4; } }
        if (NT_SUCCESS(restored) && !NT_SUCCESS(verified)) { restored=verified; }
        if (G->RestoreMismatch==0) { G->Checks|=32; }
        else if (NT_SUCCESS(restored)) { restored=STATUS_IO_DEVICE_ERROR; }
        G->RestoreStatus=restored;
        if (NT_SUCCESS(st) && !NT_SUCCESS(restored)) { st=restored; C->Reason=13; }
    }
    if (band!=NULL) { MmUnmapIoSpace(band,0x120000); }
    if (backup!=NULL) { ExFreePoolWithTag(backup,EPI_GMU_TAG); }
    if (NT_SUCCESS(st) && G->Checks!=0x3F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_A1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG VChecks,VReason,IbBadCount,IbFirstBad,IbBadValue;
    ULONG FramesDone,FailFrame,FailData25,FailRptr,FailWptr,FenceMaxUs,FenceTotalUs,ElapsedMs;
    ULONG SqGoodEven,SqGoodOdd,SqBad,SqFirst,SqValue,SqExpected;
    ULONG BgGoodEven,BgGoodOdd,BgBad,BgFirst,BgValue,MaxErr[3],OutsideChanged,OutsideFirst,OutsideOld,OutsideNew,Sample[18];
    ULONG DataAfter[3],CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAfter,ScanCount,ScanFirst,ScanDwords;
    ULONG RestoreMismatch,RestoreFirst,HoldMs,Checks,Prepared;
    NTSTATUS BackupStatus,HoldStatus,RestoreStatus;
};
static_assert(sizeof(GMU_A1_TRACE)==464,"A-1 diagnostic ABI");
static const ULONG GmuA1Frames=241,GmuA1WptrEnd=109+9*GmuA1Frames;
static_assert(GmuA1WptrEnd==2278 && GmuA1WptrEnd<4096,"A-1 must never wrap into the IB region");
#include "epi_gmu_a1_table.inc"
static_assert(sizeof(GmuA1Table)==120*12*sizeof(ULONG),"A-1 rotation table size");
static const ULONG GmuA1Ib[]={
    0x70460001,0x00000031,0x70268000,0x4080F002,0x0000019C,0x007F029B,0x48840A02,0x0000019C,
    0x007F029B,0x4880B002,0x0000019C,0x007F029B,0x40A01004,0x70015800,0x00000000,0x00000180,
    0x00000020,0x40882304,0x00000087,0x00000000,0x71008A00,0x00000000,0x70388003,0x00000084,
    0x00000001,0x0000000C,0x70460004,0x0000001D,0x7001705C,0x00000000,0xCAFE0015,0x70268000,
    0x4080F002,0x000001A4,0x007F02A3,0x48840A02,0x000001A4,0x007F02A3,0x4880B002,0x000001A4,
    0x007F02A3,0x40A01004,0x70015A00,0x00000000,0x00000180,0x00000020,0x40882304,0x00000087,
    0x00000000,0x71009AC0,0x00000000,0x70388003,0x00000084,0x00000001,0x0000000C,0x70460004,
    0x0000001D,0x70017060,0x00000000,0xCAFE0016,0x70268000,
};
static_assert(sizeof(GmuA1Ib)==61*sizeof(ULONG),"A-1 IB length");
static const ULONG GmuA1Packet[]={0x70BF8003,0x70015600,0,61,0x70460004,4,0x70017064,0};
static ULONG GmuA1VertexWord(ULONG Angle,BOOLEAN PassB,ULONG Index)
{
    static const ULONG order[]={0,1,2,0,2,3};
    static const ULONG bg[4][3]={{0x43CE0000,0x43D20000,0},{0x44270000,0x44290000,0},
        {0x44270000,0x44290000,0x43000000},{0x43CE0000,0x43D20000,0x43000000}};
    static const ULONG color[4][4]={{0,0,0x3F800000,0x3F800000},{0,0x3F800000,0,0x3F800000},
        {0x3F800000,0,0,0x3F800000},{0x3F800000,0x3F800000,0x3F800000,0x3F800000}};
    const ULONG vertex=Index/8,component=Index%8,corner=order[vertex%6];
    if (component==2) { return 0; }
    if (component==3 || component==7) { return 0x3F800000; }
    if (component>=4) { return vertex<6 ? 0x3E000000 : color[corner][component-4]; }
    if (vertex<6) { return bg[corner][component==1 ? 2 : (PassB ? 1 : 0)]; }
    return GmuA1Table[Angle][corner*3+(component==1 ? 2 : (PassB ? 1 : 0))];
}
static NTSTATUS GmuA1Vertices(volatile ULONG* Words,ULONG Angle,BOOLEAN Initial)
{
    for (ULONG pass=0;pass<2;++pass)
    {
        volatile ULONG* vb=Words+(pass ? 0x5A00 : 0x5800)/4;
        for (ULONG i=Initial ? 0 : 48;i<96;++i) { vb[i]=GmuA1VertexWord(Angle,pass!=0,i); }
    }
    KeMemoryBarrier();
    for (ULONG pass=0;pass<2;++pass)
    {
        volatile ULONG* vb=Words+(pass ? 0x5A00 : 0x5800)/4;
        for (ULONG i=0;i<96;++i) if (vb[i]!=GmuA1VertexWord(Angle,pass!=0,i)) { return STATUS_IO_DEVICE_ERROR; }
    }
    return STATUS_SUCCESS;
}
static ULONG GmuA1Expected(ULONG Column,ULONG ViewRow)
{
    // Both final triangles have doubled-coordinate area 51200. Their red/blue weights coincide;
    // green is the absolute difference across the diagonal (the other corners are red/blue/white).
    const LONG u=static_cast<LONG>(Column)*2+1-920,t=static_cast<LONG>(ViewRow)*2+1-48;
    const LONG diagonal=160*u-320*t;
    const ULONG r=static_cast<ULONG>((510*(51200-160*u)+51200)/102400);
    const ULONG g=static_cast<ULONG>((510*(diagonal<0 ? -diagonal : diagonal)+51200)/102400);
    const ULONG b=static_cast<ULONG>((510*(320*t)+51200)/102400);
    return 0xFF000000UL|(r<<16)|(g<<8)|b;
}
static NTSTATUS GmuA1Pause(ULONG Milliseconds)
{
#ifdef EPI_GMU_HOST_TEST
    KeStallExecutionProcessor(static_cast<int>(Milliseconds*1000)); return STATUS_SUCCESS;
#else
    LARGE_INTEGER interval; interval.QuadPart=-static_cast<LONGLONG>(Milliseconds)*10000;
    return KeDelayExecutionThread(KernelMode,FALSE,&interval);
#endif
}
static VOID GmuA1TraceInit(GMU_A1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp);
    t->FramesDone=0; t->FenceMaxUs=0; t->FenceTotalUs=0; t->HoldMs=0; t->Checks=0; t->Prepared=0;
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
    t->BackupStatus=t->HoldStatus=t->RestoreStatus=STATUS_NOT_FOUND;
}
static BOOLEAN GmuA1CanFollow(const GMU_V1_TRACE* V)
{
    return V!=NULL && NT_SUCCESS(V->Cp.Status) && V->Cp.Reason==0 && V->Checks==0x3F && V->Cp.WptrAfter==109 &&
           V->DataAfter[0]==0xCAFE0012 && V->DataAfter[1]==0xCAFE0013 && V->DataAfter[2]==0xCAFE0014 &&
           V->RestoreMismatch==0 && NT_SUCCESS(V->RestoreStatus);
}
static NTSTATUS GmuA1FrameWait(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_A1_TRACE* G,
                               volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow,
                               volatile ULONG* Data,ULONG Frame,ULONGLONG Begin)
{
    GMU_G2_TRACE* C=&G->Cp; const ULONG target=109+9*(Frame+1);
    NTSTATUS st=GmuCpFenceWait(S,Cc,T,C,Cp+7,target,Gpu+0x38,&G->MaskAfter);
    if (!NT_SUCCESS(st)) { return st; }
    for (ULONG poll=0;;++poll)
    {
        C->Polls=poll;
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
        if (G->MaskAfter!=0) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
        KeMemoryBarrier(); C->ShadowAfter=*Shadow;
        C->Rptr=READ_REGISTER_ULONG(Cp+6); C->WptrAfter=READ_REGISTER_ULONG(Cp+7);
        C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201); C->RbbmStatus=READ_REGISTER_ULONG(Gpu+0x210);
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
        if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
        if (KeQueryInterruptTime()-Begin>=1000000) { C->Reason=16; return STATUS_IO_TIMEOUT; }
        const BOOLEAN idle=Frame+1<GmuA1Frames || (!(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23)));
        if (Data[25]==(0xA1000000UL|Frame) && C->Rptr==target && C->WptrAfter==target && C->ShadowAfter==target && idle)
        { return STATUS_SUCCESS; }
        KeStallExecutionProcessor(100);
    }
}
static NTSTATUS GmuA1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_A1_TRACE* G,HANDLE Diag,const GMU_D1_TRACE* D)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1B00); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    PVOID band=NULL; ULONG* backup=NULL;
    volatile ULONG* data=words+0x7000/4;
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0x1B01);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                BOOLEAN previous=C->ShadowBefore==109 && C->WptrBefore==109 && C->Rptr==109;
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i] || words[39+i]!=GmuG5Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<13;++i) { if (words[48+i]!=GmuG5bPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<12;++i) { if (words[61+i]!=GmuZ1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<36;++i) { if (words[0x6100/4+i]!=GmuG5Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<10;++i) { if (words[0x6200/4+i]!=GmuG5bIb[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i) { if (words[73+i]!=GmuS1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<53;++i) { if (words[0x6300/4+i]!=GmuS1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x6800/4+i]!=GmuS1Shader[i]) { previous=FALSE; } }
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,0xCAFE0003,0xFFFFFFFF,
                    0xCAFE0004,0xFFFFFFFF,0xCAFE0006,0xCAFE0007,0xCAFE0008,0xCAFE0009,0xCAFE000A,0xCAFE000B,0xCAFE000C};
                for (ULONG i=0;i<15;++i) { if (data[i]!=expected[i]) { previous=FALSE; } }
                if (words[0x7C00/4]!=0xC0DE5EED) { previous=FALSE; }
                for (ULONG i=1;i<16;++i) { if (words[0x7C00/4+i]!=0xFFFFFFFF) { previous=FALSE; } }
                if (!(D!=NULL && EpiGmuD1DisplayMatches(D->Disp))) { previous=FALSE; }
                GMU_REGION* ext=GmuFind(S,0x71000000,0x120000);
                if (ext==NULL || !ext->External || !ext->Mapped || ext->Pa.QuadPart!=0xE1462000 ||
                    ext->Size!=0x120000 || ext->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE)) { previous=FALSE; }
                if (data[15]!=0xCAFE000D || data[16]!=0xCAFE000E) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[82+i]!=GmuR1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<314;++i) { if (words[0x4000/4+i]!=GmuR1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i)
                { if (words[0x4800/4+i]!=GmuR1Vs[i] || words[0x4880/4+i]!=GmuR1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<8;++i) { if (words[0x4900/4+i]!=GmuR1Vertices[i]) { previous=FALSE; } }
                for (ULONG i=0;i<256;++i)
                {
                    const BOOLEAN inside=i%16>=4 && i%16<12 && i/16>=4 && i/16<12;
                    if (words[0x4C00/4+i]!=(inside ? 0x000033FFUL : 0xFFFFFFFFUL)) { previous=FALSE; }
                }
                if (data[17]!=0xCAFE000F || data[18]!=0xCAFE0010 || data[19]!=0xCAFE0011) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[91+i]!=GmuD1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<72;++i) { if (words[0x4500/4+i]!=GmuD1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x4A00/4+i]!=GmuD1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<8;++i)
                { if (words[0x4A80/4+i]!=GmuD1VertA[i] || words[0x4AA0/4+i]!=GmuD1VertB[i]) { previous=FALSE; } }
                if (data[20]!=0xCAFE0012 || data[21]!=0xCAFE0013 || data[22]!=0xCAFE0014) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[100+i]!=GmuV1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<115;++i) { if (words[0x5100/4+i]!=GmuV1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x5400/4+i]!=GmuV1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<24;++i)
                { if (words[0x5480/4+i]!=GmuV1VertA[i] || words[0x5500/4+i]!=GmuV1VertB[i]) { previous=FALSE; } }
                for (ULONG i=109;i<GmuA1WptrEnd;++i) { if (words[i]!=0) { previous=FALSE; } }
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1B02);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1B08);
            if (NT_SUCCESS(st))
            {
                backup=static_cast<ULONG*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,0x120000,EPI_GMU_TAG));
                if (backup==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                else
                {
                    PHYSICAL_ADDRESS pa; pa.QuadPart=0xE1462000;
                    band=MmMapIoSpaceEx(pa,0x120000,PAGE_READWRITE|PAGE_NOCACHE);
                    if (band==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                    else
                    {
                        for (ULONG i=0;i<0x120000/4;++i) { backup[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i); }
                        KeMemoryBarrier();
                    }
                }
                G->BackupStatus=st;
                if (!NT_SUCCESS(st)) { C->Reason=GX_REG_MAP_FAILED; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1B03);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=0;i<3;++i) { data[23+i]=0xFFFFFFFF; }
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x5600,GmuA1Ib,sizeof(GmuA1Ib));
                st=GmuA1Vertices(words,0,TRUE); KeMemoryBarrier(); G->Prepared=1; G->IbBadCount=0;
                for (ULONG i=0;i<61;++i)
                {
                    const ULONG value=words[0x5600/4+i];
                    if (value!=GmuA1Ib[i])
                    { ++G->IbBadCount; if (G->IbFirstBad==0xFFFFFFFF) { G->IbFirstBad=i; G->IbBadValue=value; } }
                }
                for (ULONG i=0;i<3;++i) { if (data[23+i]!=0xFFFFFFFF) { st=STATUS_IO_DEVICE_ERROR; } }
                if (G->IbBadCount) { st=STATUS_IO_DEVICE_ERROR; }
                if (!NT_SUCCESS(st)) { C->Reason=13; }
            }
        }
        if (NT_SUCCESS(st))
        {
            C->ShadowAfter=C->ShadowBefore; C->WptrAfter=C->WptrBefore;
            ULONGLONG firstPublish=0;
            for (ULONG f=0;f<GmuA1Frames;++f)
            {
                // Only coarse progress writes; never flush the registry inside a frame's fence wait.
                if ((f%16)==0) { st=GmuG2Guard(S,Params,Cc,T,C,0x1B40+f/16); }
                else if (!GmuG2Live(S,Cc,T,C)) { st=STATUS_INVALID_DEVICE_STATE; }
                if (NT_SUCCESS(st))
                {
                    G->MaskAfter=READ_REGISTER_ULONG(gpu+0x38);
                    if (G->MaskAfter!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                }
                if (NT_SUCCESS(st) && f!=0)
                { st=GmuA1Vertices(words,f%120,FALSE); if (!NT_SUCCESS(st)) { C->Reason=13; } }
                if (NT_SUCCESS(st))
                {
                    KeMemoryBarrier();
                    for (ULONG i=0;i<9;++i) { words[109+9*f+i]=i<8 ? GmuA1Packet[i] : (0xA1000000UL|f); }
                    KeMemoryBarrier();
                    for (ULONG i=0;i<9;++i) if (words[109+9*f+i]!=(i<8 ? GmuA1Packet[i] : (0xA1000000UL|f)))
                    { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                if (NT_SUCCESS(st))
                {
                    const ULONGLONG begin=KeQueryInterruptTime();
                    if (!C->Attempted) { firstPublish=begin; }
                    C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,109+9*(f+1)); KeMemoryBarrier(); ++C->WptrWrites;
                    st=GmuA1FrameWait(S,Cc,T,G,cp,gpu,rptr,data,f,begin);
                    const ULONGLONG end=KeQueryInterruptTime();
                    const ULONG us=static_cast<ULONG>((end-begin+9)/10);
                    if (us>G->FenceMaxUs) { G->FenceMaxUs=us; } G->FenceTotalUs+=us;
                    G->ElapsedMs=static_cast<ULONG>((end-firstPublish)/10000);
                    if (NT_SUCCESS(st)) { ++G->FramesDone; }
                }
                if (!NT_SUCCESS(st))
                {
                    G->FailFrame=f; G->FailData25=data[25]; G->FailRptr=C->Rptr; G->FailWptr=C->WptrAfter; break;
                }
                st=GmuA1Pause(16);
                if (!NT_SUCCESS(st))
                { C->Reason=17; G->FailFrame=f; G->FailData25=data[25]; G->FailRptr=C->Rptr; G->FailWptr=C->WptrAfter; break; }
            }
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1B10+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1B20);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<3;++i) { G->DataAfter[i]=data[23+i]; }
        if (G->Prepared)
        {
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (G->FramesDone==GmuA1Frames && data[25]==0xA10000F0 && C->ShadowAfter==GmuA1WptrEnd && C->Rptr==GmuA1WptrEnd && C->WptrAfter==GmuA1WptrEnd && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }
            if (G->DataAfter[0]==0xCAFE0015 && G->DataAfter[1]==0xCAFE0016) { G->Checks|=4; }
        }
        if (band!=NULL && NT_SUCCESS(G->BackupStatus))
        {
            G->SqGoodEven=0; G->SqGoodOdd=0; G->SqBad=0; G->BgGoodEven=0; G->BgGoodOdd=0; G->BgBad=0; G->OutsideChanged=0;
            for (ULONG i=0;i<3;++i) { G->MaxErr[i]=0; }
            for (ULONG i=0;i<0x120000/4;++i)
            {
                const ULONG offset=i*4,fb=0x462000+offset,y=fb/4320,x=(fb%4320)/4;
                const ULONG value=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i);
                if (x>=412 && x<668 && y>=1072 && y<1328)
                {
                    const ULONG v=(y-1072)/2;
                    if (x>=461 && x<=618 && v>=25 && v<=102)
                    {
                        const ULONG expected=GmuA1Expected(x,v);
                        if (GmuV1WithinTolerance(value,expected,G->MaxErr))
                        { if (y&1) { ++G->SqGoodOdd; } else { ++G->SqGoodEven; } }
                        else
                        { ++G->SqBad; if (G->SqFirst==0xFFFFFFFF) { G->SqFirst=offset; G->SqValue=value; G->SqExpected=expected; } }
                    }
                    else if (x<=458 || x>=621 || v<=22 || v>=105)
                    {
                        if (GmuV1WithinTolerance(value,0xFF202020,G->MaxErr))
                        { if (y&1) { ++G->BgGoodOdd; } else { ++G->BgGoodEven; } }
                        else { ++G->BgBad; if (G->BgFirst==0xFFFFFFFF) { G->BgFirst=offset; G->BgValue=value; } }
                    }
                }
                else if (value!=backup[i])
                {
                    ++G->OutsideChanged;
                    if (G->OutsideFirst==0xFFFFFFFF) { G->OutsideFirst=offset; G->OutsideOld=backup[i]; G->OutsideNew=value; }
                }
            }
            const ULONG sample[][2]={{540,64},{463,27},{616,27},{616,100},{463,100},{500,44},{580,84},{414,2},{665,125}};
            for (ULONG i=0;i<9;++i) for (ULONG parity=0;parity<2;++parity)
            { G->Sample[i*2+parity]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+((1072+sample[i][1]*2+parity)*4320+sample[i][0]*4-0x462000)/4); }
            if (G->SqGoodEven==12324 && G->SqGoodOdd==12324) { G->Checks|=8; }
            if (G->BgGoodEven==19484 && G->BgGoodOdd==19484 && G->OutsideChanged==0) { G->Checks|=16; }
        }
    }
    KeMemoryBarrier(); G->ScanCount=0; G->ScanDwords=0;
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if (off<GmuA1WptrEnd*4 || (off>=0x5600 && off<0x5B80) || (off>=0x5100 && off<0x52CC) || (off>=0x5400 && off<0x5560) || (off>=0x4500 && off<0x4620) || (off>=0x4A00 && off<0x4AC0) || (off>=0x4000 && off<0x44E8) || (off>=0x4800 && off<0x4920) ||
            (off>=0x4C00 && off<0x5040) || (off>=0x6000 && off<0x6240) || (off>=0x6300 && off<0x6400) ||
            (off>=0x6800 && off<0x6880) || (off>=0x7000 && off<0x7068) ||
            (off>=0x7400 && off<0x7840) || (off>=0x7C00 && off<0x7C40)) { continue; }
        ++G->ScanDwords;
        const ULONG value=words[off/4];
        if ((value&0xFF000000UL)==0xFF000000UL && value!=0xFFFFFFFF) { ++G->ScanCount; if (G->ScanFirst==0xFFFFFFFF) { G->ScanFirst=off; } }
    }
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (band!=NULL && NT_SUCCESS(G->BackupStatus))
    {
        if (C->Attempted && G->FramesDone==GmuA1Frames && NT_SUCCESS(st))
        {
            C->Status=st;
            NTSTATUS held=GmuStep(Params,0x1B30);
            if (NT_SUCCESS(held)) { held=EpiSetValue(Diag,L"GpuGmuA1",REG_BINARY,G,sizeof(*G)); }
            if (NT_SUCCESS(held)) { held=ZwFlushKey(Diag); }
            if (NT_SUCCESS(held))
            {
                held=GmuA1Pause(2000);
                if (NT_SUCCESS(held)) { G->HoldMs=2000; }
            }
            G->HoldStatus=held;
            if (NT_SUCCESS(st) && !NT_SUCCESS(held)) { st=held; C->Reason=6; }
        }
        // Restoring display RAM is mandatory even when a breadcrumb or GX prerequisite failed.
        NTSTATUS restored=GmuStep(Params,0x1B31);
        for (ULONG i=0;i<0x120000/4;++i) { WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i,backup[i]); }
        KeMemoryBarrier();
        const NTSTATUS verified=GmuStep(Params,0x1B32);
        G->RestoreMismatch=0;
        for (ULONG i=0;i<0x120000/4;++i) if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i)!=backup[i])
        { ++G->RestoreMismatch; if (G->RestoreFirst==0xFFFFFFFF) { G->RestoreFirst=i*4; } }
        if (NT_SUCCESS(restored) && !NT_SUCCESS(verified)) { restored=verified; }
        if (G->RestoreMismatch==0) { G->Checks|=32; }
        else if (NT_SUCCESS(restored)) { restored=STATUS_IO_DEVICE_ERROR; }
        G->RestoreStatus=restored;
        if (NT_SUCCESS(st) && !NT_SUCCESS(restored)) { st=restored; C->Reason=13; }
    }
    if (band!=NULL) { MmUnmapIoSpace(band,0x120000); }
    if (backup!=NULL) { ExFreePoolWithTag(backup,EPI_GMU_TAG); }
    if (NT_SUCCESS(st) && G->Checks!=0x3F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_T1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG AChecks,AReason,IbBadCount,IbFirstBad,IbBadValue,DescBad;
    ULONG SqGoodEven,SqGoodOdd,SqBad,SqFirst,SqValue,SqExpected,MaxErr[3],TexelBad[2],SqZero,SqPad;
    ULONG BoxGoodEven,BoxGoodOdd,BoxBad,BoxFirst,BoxValue,BoxOld,OutsideChanged,OutsideFirst,OutsideOld,OutsideNew,Sample[20];
    ULONG DataAfter[3],CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAfter,FenceUs,ScanCount,ScanFirst,ScanDwords;
    ULONG RestoreMismatch,RestoreFirst,HoldMs,Checks,Prepared;
    NTSTATUS BackupStatus,HoldStatus,RestoreStatus;
};
static_assert(sizeof(GMU_T1_TRACE)==468,"T-1 diagnostic ABI");
static const ULONG GmuT1Wptr=GmuA1WptrEnd+9;
static_assert(GmuT1Wptr==2287 && GmuT1Wptr<4096,"T-1 must not wrap into IB memory");
static const ULONG GmuT1Ib[]={
    0x70460001,0x00000031,0x70268000,0x40BB0801,0x000000BF,0x48AB0402,0x00020300,0x00000001,
    0x40A98001,0x85400100,0x48A98207,0x00000000,0x70016880,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x70348003,0x00720000,0x70016880,0x00000000,0x48B30202,0x70016B00,
    0x00000000,0x40B18002,0x70016B00,0x00000000,0x70348003,0x00520000,0x70016AC0,0x00000000,
    0x40A9E002,0x70016AC0,0x00000000,0x70348003,0x00524000,0x70016A80,0x00000000,0x48A9E402,
    0x70016A80,0x00000000,0x40A9A701,0x00000001,0x40A99E02,0x00000001,0x23C40000,0x48A9A301,
    0x00000000,0x4080F002,0x0000019C,0x007F029B,0x48840A02,0x0000019C,0x007F029B,0x4880B002,
    0x0000019C,0x007F029B,0x40A01004,0x70016900,0x00000000,0x000000C0,0x00000020,0x40882304,
    0x00000087,0x00000000,0x71008A00,0x00000000,0x70388003,0x00000084,0x00000001,0x00000006,
    0x70460004,0x0000001D,0x70017068,0x00000000,0xCAFE0017,0x70268000,0x4080F002,0x000001A4,
    0x007F02A3,0x48840A02,0x000001A4,0x007F02A3,0x4880B002,0x000001A4,0x007F02A3,0x40A01004,
    0x700169C0,0x00000000,0x000000C0,0x00000020,0x40882304,0x00000087,0x00000000,0x71009AC0,
    0x00000000,0x70388003,0x00000084,0x00000001,0x00000006,0x70460004,0x0000001D,0x7001706C,
    0x00000000,0xCAFE0018,0x70268000,
};
static_assert(sizeof(GmuT1Ib)==107*sizeof(ULONG),"T-1 IB length");
static const ULONG GmuT1Packet[]={0x70BF8003,0x70016400,0,107,0x70460004,4,0x70017070,0,0xCAFE0019};
static const ULONG GmuT1Desc[16]={0x0C006880,0x00040008,0x20002000,0,0x70016C00,0};
static const ULONG GmuT1Sampler[4]={0x60920,0x60,0,0};
static const ULONG GmuT1Regions[][2]={{0x6880,0x80},{0x6900,0xC0},{0x69C0,0xC0},
    {0x6A80,0x40},{0x6AC0,0x10},{0x6B00,0x80},{0x6C00,0x200}};
static ULONG GmuT1Texel(ULONG I,ULONG J)
{
    return 0xFF000000UL|((32*I+16)<<16)|((32*J+16)<<8)|(((I+J)&1) ? 0xC0 : 0x40);
}
static ULONG GmuT1VertexWord(BOOLEAN PassB,ULONG Index)
{
    static const ULONG order[]={0,1,2,0,2,3};
    const ULONG corner=order[Index/8],component=Index%8;
    if (component==0) { return GmuA1Table[0][corner*3+(PassB ? 1 : 0)]; }
    if (component==1) { return GmuA1Table[0][corner*3+2]; }
    if (component==3 || component==7) { return 0x3F800000; }
    if (component==4) { return corner==1 || corner==2 ? 0x41000000 : 0; }
    if (component==5) { return corner==2 || corner==3 ? 0x41000000 : 0; }
    return 0;
}
static ULONG GmuT1Word(ULONG Offset)
{
    if (Offset>=0x6880 && Offset<0x6900) { return Offset==0x6884 ? 0x13000000 : 0; }
    if (Offset>=0x6900 && Offset<0x69C0) { return GmuT1VertexWord(FALSE,(Offset-0x6900)/4); }
    if (Offset>=0x69C0 && Offset<0x6A80) { return GmuT1VertexWord(TRUE,(Offset-0x69C0)/4); }
    if (Offset>=0x6A80 && Offset<0x6AC0) { return GmuT1Desc[(Offset-0x6A80)/4]; }
    if (Offset>=0x6AC0 && Offset<0x6AD0) { return GmuT1Sampler[(Offset-0x6AC0)/4]; }
    if (Offset>=0x6C00 && Offset<0x6E00)
    { const ULONG word=(Offset-0x6C00)/4; return word%16<8 ? GmuT1Texel(word%16,word/16) : 0xFFFF00FF; }
    return 0; // border color and unused gaps
}
static VOID GmuT1TraceInit(GMU_T1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp); t->HoldMs=0; t->Checks=0; t->Prepared=0;
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
    t->BackupStatus=t->HoldStatus=t->RestoreStatus=STATUS_NOT_FOUND;
}
static BOOLEAN GmuT1CanFollow(const GMU_A1_TRACE* A)
{
    return A!=NULL && NT_SUCCESS(A->Cp.Status) && A->Cp.Reason==0 && A->Checks==0x3F && A->FramesDone==241 &&
        A->Cp.WptrAfter==2278 && A->DataAfter[0]==0xCAFE0015 && A->DataAfter[1]==0xCAFE0016 &&
        A->DataAfter[2]==0xA10000F0 && A->RestoreMismatch==0 && NT_SUCCESS(A->RestoreStatus);
}
static NTSTATUS GmuT1Wait(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_T1_TRACE* G,
                         volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow,volatile ULONG* Data,ULONGLONG Begin)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuCpFenceWait(S,Cc,T,C,Cp+7,GmuT1Wptr,Gpu+0x38,&G->MaskAfter);
    if (!NT_SUCCESS(st)) { return st; }
    for (ULONG poll=0;;++poll)
    {
        C->Polls=poll;
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
        if (G->MaskAfter!=0) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
        KeMemoryBarrier(); C->ShadowAfter=*Shadow;
        C->Rptr=READ_REGISTER_ULONG(Cp+6); C->WptrAfter=READ_REGISTER_ULONG(Cp+7);
        C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201); C->RbbmStatus=READ_REGISTER_ULONG(Gpu+0x210);
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C)); G->IbRemaining=READ_REGISTER_ULONG(Cp+0x12A);
        if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
        if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
        if (KeQueryInterruptTime()-Begin>=1000000) { C->Reason=16; return STATUS_IO_TIMEOUT; }
        if (Data[28]==0xCAFE0019 && C->Rptr==GmuT1Wptr && C->WptrAfter==GmuT1Wptr && C->ShadowAfter==GmuT1Wptr &&
            !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23)) && G->IbRemaining==0) { return STATUS_SUCCESS; }
        KeStallExecutionProcessor(100);
    }
}
static NTSTATUS GmuT1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_T1_TRACE* G,HANDLE Diag,const GMU_D1_TRACE* D)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1C00); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    PVOID band=NULL; ULONG* backup=NULL;
    volatile ULONG* data=words+0x7000/4;
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0x1C01);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                BOOLEAN previous=C->ShadowBefore==2278 && C->WptrBefore==2278 && C->Rptr==2278;
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i] || words[39+i]!=GmuG5Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<13;++i) { if (words[48+i]!=GmuG5bPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<12;++i) { if (words[61+i]!=GmuZ1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<36;++i) { if (words[0x6100/4+i]!=GmuG5Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<10;++i) { if (words[0x6200/4+i]!=GmuG5bIb[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i) { if (words[73+i]!=GmuS1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<53;++i) { if (words[0x6300/4+i]!=GmuS1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x6800/4+i]!=GmuS1Shader[i]) { previous=FALSE; } }
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,0xCAFE0003,0xFFFFFFFF,
                    0xCAFE0004,0xFFFFFFFF,0xCAFE0006,0xCAFE0007,0xCAFE0008,0xCAFE0009,0xCAFE000A,0xCAFE000B,0xCAFE000C};
                for (ULONG i=0;i<15;++i) { if (data[i]!=expected[i]) { previous=FALSE; } }
                if (words[0x7C00/4]!=0xC0DE5EED) { previous=FALSE; }
                for (ULONG i=1;i<16;++i) { if (words[0x7C00/4+i]!=0xFFFFFFFF) { previous=FALSE; } }
                if (!(D!=NULL && EpiGmuD1DisplayMatches(D->Disp))) { previous=FALSE; }
                GMU_REGION* ext=GmuFind(S,0x71000000,0x120000);
                if (ext==NULL || !ext->External || !ext->Mapped || ext->Pa.QuadPart!=0xE1462000 ||
                    ext->Size!=0x120000 || ext->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE)) { previous=FALSE; }
                if (data[15]!=0xCAFE000D || data[16]!=0xCAFE000E) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[82+i]!=GmuR1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<314;++i) { if (words[0x4000/4+i]!=GmuR1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i)
                { if (words[0x4800/4+i]!=GmuR1Vs[i] || words[0x4880/4+i]!=GmuR1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<8;++i) { if (words[0x4900/4+i]!=GmuR1Vertices[i]) { previous=FALSE; } }
                for (ULONG i=0;i<256;++i)
                {
                    const BOOLEAN inside=i%16>=4 && i%16<12 && i/16>=4 && i/16<12;
                    if (words[0x4C00/4+i]!=(inside ? 0x000033FFUL : 0xFFFFFFFFUL)) { previous=FALSE; }
                }
                if (data[17]!=0xCAFE000F || data[18]!=0xCAFE0010 || data[19]!=0xCAFE0011) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[91+i]!=GmuD1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<72;++i) { if (words[0x4500/4+i]!=GmuD1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x4A00/4+i]!=GmuD1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<8;++i)
                { if (words[0x4A80/4+i]!=GmuD1VertA[i] || words[0x4AA0/4+i]!=GmuD1VertB[i]) { previous=FALSE; } }
                if (data[20]!=0xCAFE0012 || data[21]!=0xCAFE0013 || data[22]!=0xCAFE0014 ||
                    data[23]!=0xCAFE0015 || data[24]!=0xCAFE0016 || data[25]!=0xA10000F0) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[100+i]!=GmuV1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<115;++i) { if (words[0x5100/4+i]!=GmuV1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x5400/4+i]!=GmuV1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<24;++i)
                { if (words[0x5480/4+i]!=GmuV1VertA[i] || words[0x5500/4+i]!=GmuV1VertB[i]) { previous=FALSE; } }
                for (ULONG f=0;f<GmuA1Frames;++f) for (ULONG i=0;i<9;++i)
                { if (words[109+9*f+i]!=(i<8 ? GmuA1Packet[i] : (0xA1000000UL|f))) { previous=FALSE; } }
                for (ULONG i=0;i<61;++i) { if (words[0x5600/4+i]!=GmuA1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<96;++i)
                { if (words[0x5800/4+i]!=GmuA1VertexWord(0,FALSE,i) || words[0x5A00/4+i]!=GmuA1VertexWord(0,TRUE,i)) { previous=FALSE; } }
                for (ULONG i=2278;i<2287;++i) { if (words[i]!=0) { previous=FALSE; } }
                for (ULONG off=0x6400;off<0x6800;off+=4) { if (words[off/4]!=0) { previous=FALSE; } }
                for (ULONG off=0x6880;off<0x6E00;off+=4) { if (words[off/4]!=0) { previous=FALSE; } }
                if (!previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1C02);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1C08);
            if (NT_SUCCESS(st))
            {
                backup=static_cast<ULONG*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,0x120000,EPI_GMU_TAG));
                if (backup==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                else
                {
                    PHYSICAL_ADDRESS pa; pa.QuadPart=0xE1462000;
                    band=MmMapIoSpaceEx(pa,0x120000,PAGE_READWRITE|PAGE_NOCACHE);
                    if (band==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                    else
                    {
                        for (ULONG i=0;i<0x120000/4;++i) { backup[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i); }
                        KeMemoryBarrier();
                    }
                }
                G->BackupStatus=st;
                if (!NT_SUCCESS(st)) { C->Reason=GX_REG_MAP_FAILED; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1C03);
            if (NT_SUCCESS(st))
            {
                for (ULONG i=0;i<3;++i) { data[26+i]=0xFFFFFFFF; }
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x6400,GmuT1Ib,sizeof(GmuT1Ib));
                for (ULONG r=0;r<RTL_NUMBER_OF(GmuT1Regions);++r)
                    for (ULONG off=GmuT1Regions[r][0];off<GmuT1Regions[r][0]+GmuT1Regions[r][1];off+=4) { words[off/4]=GmuT1Word(off); }
                RtlCopyMemory(static_cast<ULONG*>(ring->Va)+2278,GmuT1Packet,sizeof(GmuT1Packet));
                KeMemoryBarrier(); G->Prepared=1; G->IbBadCount=0; G->DescBad=0;
                for (ULONG i=0;i<107;++i)
                {
                    const ULONG value=words[0x6400/4+i];
                    if (value!=GmuT1Ib[i])
                    { ++G->IbBadCount; if (G->IbFirstBad==0xFFFFFFFF) { G->IbFirstBad=i; G->IbBadValue=value; } }
                }
                for (ULONG r=0;r<RTL_NUMBER_OF(GmuT1Regions);++r)
                    for (ULONG off=GmuT1Regions[r][0];off<GmuT1Regions[r][0]+GmuT1Regions[r][1];off+=4)
                        if (words[off/4]!=GmuT1Word(off)) { ++G->DescBad; }
                for (ULONG i=0;i<9;++i) { if (words[2278+i]!=GmuT1Packet[i]) { st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<3;++i) { if (data[26+i]!=0xFFFFFFFF) { st=STATUS_IO_DEVICE_ERROR; } }
                if (G->IbBadCount || G->DescBad) { st=STATUS_IO_DEVICE_ERROR; }
                if (!NT_SUCCESS(st)) { C->Reason=13; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1C04);
            if (NT_SUCCESS(st))
            {
                G->MaskAfter=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAfter!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                else
                {
                    const ULONGLONG begin=KeQueryInterruptTime();
                    C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,GmuT1Wptr); KeMemoryBarrier(); ++C->WptrWrites;
                    st=GmuT1Wait(S,Cc,T,G,cp,gpu,rptr,data,begin);
                    G->FenceUs=static_cast<ULONG>((KeQueryInterruptTime()-begin+9)/10);
                }
            }
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1C10+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
        }
        if (!NT_SUCCESS(st)) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1C20);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        for (ULONG i=0;i<3;++i) { G->DataAfter[i]=data[26+i]; }
        if (G->Prepared)
        {
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (G->DataAfter[2]==0xCAFE0019 && G->IbRemaining==0 && C->ShadowAfter==2287 && C->Rptr==2287 && C->WptrAfter==2287 && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }
            if (G->DataAfter[0]==0xCAFE0017 && G->DataAfter[1]==0xCAFE0018) { G->Checks|=4; }
        }
        if (band!=NULL && NT_SUCCESS(G->BackupStatus))
        {
            G->SqGoodEven=0; G->SqGoodOdd=0; G->SqBad=0; G->SqZero=0; G->SqPad=0;
            G->TexelBad[0]=0; G->TexelBad[1]=0; G->BoxGoodEven=0; G->BoxGoodOdd=0; G->BoxBad=0; G->OutsideChanged=0;
            for (ULONG i=0;i<3;++i) { G->MaxErr[i]=0; }
            for (ULONG i=0;i<0x120000/4;++i)
            {
                const ULONG offset=i*4,fb=0x462000+offset,y=fb/4320,x=(fb%4320)/4;
                const ULONG value=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i);
                if (x>=412 && x<668 && y>=1072 && y<1328)
                {
                    const ULONG v=(y-1072)/2;
                    if (x>=460 && x<620 && v>=24 && v<104)
                    {
                        const ULONG ti=(x-460)/20,tj=(v-24)/10,texel=tj*8+ti,expected=GmuT1Texel(ti,tj);
                        GmuV1WithinTolerance(value,expected,G->MaxErr); // diagnostic only; acceptance is exact
                        if (value==0 || value==0xFF000000) { ++G->SqZero; }
                        if (value==0xFFFF00FF) { ++G->SqPad; }
                        if (value==expected) { if (y&1) { ++G->SqGoodOdd; } else { ++G->SqGoodEven; } }
                        else
                        {
                            ++G->SqBad; G->TexelBad[texel/32]|=1UL<<(texel%32);
                            if (G->SqFirst==0xFFFFFFFF) { G->SqFirst=offset; G->SqValue=value; G->SqExpected=expected; }
                        }
                    }
                    else if (value==backup[i]) { if (y&1) { ++G->BoxGoodOdd; } else { ++G->BoxGoodEven; } }
                    else { ++G->BoxBad; if (G->BoxFirst==0xFFFFFFFF) { G->BoxFirst=offset; G->BoxValue=value; G->BoxOld=backup[i]; } }
                }
                else if (value!=backup[i])
                {
                    ++G->OutsideChanged;
                    if (G->OutsideFirst==0xFFFFFFFF) { G->OutsideFirst=offset; G->OutsideOld=backup[i]; G->OutsideNew=value; }
                }
            }
            const ULONG sample[][2]={{470,29},{610,29},{610,99},{470,99},{530,59},{550,74},{479,33},{480,34},{414,2},{665,125}};
            for (ULONG i=0;i<10;++i) for (ULONG parity=0;parity<2;++parity)
            { G->Sample[i*2+parity]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+((1072+sample[i][1]*2+parity)*4320+sample[i][0]*4-0x462000)/4); }
            if (G->SqGoodEven==12800 && G->SqGoodOdd==12800) { G->Checks|=8; }
            if (G->BoxGoodEven==19968 && G->BoxGoodOdd==19968 && G->OutsideChanged==0) { G->Checks|=16; }
        }
    }
    KeMemoryBarrier(); G->ScanCount=0; G->ScanDwords=0;
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if (off<GmuT1Wptr*4 || (off>=0x5600 && off<0x5B80) || (off>=0x6400 && off<0x6800) || (off>=0x6880 && off<0x6E00) || (off>=0x5100 && off<0x52CC) || (off>=0x5400 && off<0x5560) || (off>=0x4500 && off<0x4620) || (off>=0x4A00 && off<0x4AC0) || (off>=0x4000 && off<0x44E8) || (off>=0x4800 && off<0x4920) ||
            (off>=0x4C00 && off<0x5040) || (off>=0x6000 && off<0x6240) || (off>=0x6300 && off<0x6400) ||
            (off>=0x6800 && off<0x6880) || (off>=0x7000 && off<0x7074) ||
            (off>=0x7400 && off<0x7840) || (off>=0x7C00 && off<0x7C40)) { continue; }
        ++G->ScanDwords;
        const ULONG value=words[off/4];
        if ((value&0xFF000000UL)==0xFF000000UL && value!=0xFFFFFFFF) { ++G->ScanCount; if (G->ScanFirst==0xFFFFFFFF) { G->ScanFirst=off; } }
    }
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (band!=NULL && NT_SUCCESS(G->BackupStatus))
    {
        if (C->Attempted && NT_SUCCESS(st))
        {
            C->Status=st;
            NTSTATUS held=GmuStep(Params,0x1C30);
            if (NT_SUCCESS(held)) { held=EpiSetValue(Diag,L"GpuGmuT1",REG_BINARY,G,sizeof(*G)); }
            if (NT_SUCCESS(held)) { held=ZwFlushKey(Diag); }
            if (NT_SUCCESS(held))
            {
                held=GmuA1Pause(5000);
                if (NT_SUCCESS(held)) { G->HoldMs=5000; }
            }
            G->HoldStatus=held;
            if (NT_SUCCESS(st) && !NT_SUCCESS(held)) { st=held; C->Reason=6; }
        }
        // Restoring display RAM is mandatory even when a breadcrumb or GX prerequisite failed.
        NTSTATUS restored=GmuStep(Params,0x1C31);
        for (ULONG i=0;i<0x120000/4;++i) { WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i,backup[i]); }
        KeMemoryBarrier();
        const NTSTATUS verified=GmuStep(Params,0x1C32);
        G->RestoreMismatch=0;
        for (ULONG i=0;i<0x120000/4;++i) if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i)!=backup[i])
        { ++G->RestoreMismatch; if (G->RestoreFirst==0xFFFFFFFF) { G->RestoreFirst=i*4; } }
        if (NT_SUCCESS(restored) && !NT_SUCCESS(verified)) { restored=verified; }
        if (G->RestoreMismatch==0) { G->Checks|=32; }
        else if (NT_SUCCESS(restored)) { restored=STATUS_IO_DEVICE_ERROR; }
        G->RestoreStatus=restored;
        if (NT_SUCCESS(st) && !NT_SUCCESS(restored)) { st=restored; C->Reason=13; }
    }
    if (band!=NULL) { MmUnmapIoSpace(band,0x120000); }
    if (backup!=NULL) { ExFreePoolWithTag(backup,EPI_GMU_TAG); }
    if (NT_SUCCESS(st) && G->Checks!=0x3F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_W1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG TChecks,TReason,RbCntlG1,RbCntlLive;
    ULONG PubsDone,PacketsDone,FailPub,FailPacket,FailData29,FailRptr,FailWptr,MaxRptrSeen;
    ULONG WrapCount,ZeroWptrPub,ZeroSeenRptr,StraddlePub,WaitMaxUs,WaitTotalUs,ElapsedMs;
    ULONG RingBad,RingFirst,RingValue,RingExpected,OutsideRingBad,OutsideRingFirst,OutsideRingOld,OutsideRingNew;
    ULONG SqGoodEven,SqGoodOdd,SqBad,SqFirst,SqValue,SqExpected,MaxErr[3],TexelBad[2],SqZero,SqPad;
    ULONG BoxGoodEven,BoxGoodOdd,BoxBad,BoxFirst,BoxValue,BoxOld,OutsideChanged,OutsideFirst,OutsideOld,OutsideNew,Sample[20];
    ULONG DataAfter[4],CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAfter,ScanCount,ScanFirst,ScanDwords;
    ULONG RestoreMismatch,RestoreFirst,HoldMs,Checks,Prepared;
    NTSTATUS BackupStatus,HoldStatus,RestoreStatus;
};
static_assert(sizeof(GMU_W1_TRACE)==552,"W-1 diagnostic ABI");
static const ULONG GmuW1Size=4096,GmuW1Packets=1000,GmuW1Pubs=650,GmuW1End=3095;
static_assert((2287+9*GmuW1Packets)%GmuW1Size==GmuW1End,"W-1 final write pointer");
static const ULONG GmuW1Packet[]={0x70BF8003,0x70016400,0,107,0x70460004,4,0x70017074,0};
static ULONG GmuW1Word(ULONG Packet,ULONG Word)
{
    return Word<8 ? GmuW1Packet[Word] : (0xB1000000UL|Packet);
}
static ULONG GmuW1Expected(ULONG Position)
{
    // Last absolute write <= 11286 congruent to Position modulo 4096.
    const ULONG absolute=Position+((2287+9*GmuW1Packets-1-Position)/GmuW1Size)*GmuW1Size;
    const ULONG relative=absolute-2287;
    return GmuW1Word(relative/9,relative%9);
}
static VOID GmuW1TraceInit(GMU_W1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp);
    t->PubsDone=0; t->PacketsDone=0; t->MaxRptrSeen=0; t->WrapCount=0; t->WaitMaxUs=0; t->WaitTotalUs=0;
    t->HoldMs=0; t->Checks=0; t->Prepared=0; t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
    t->BackupStatus=t->HoldStatus=t->RestoreStatus=STATUS_NOT_FOUND;
}
static BOOLEAN GmuW1CanFollow(const GMU_T1_TRACE* T)
{
    return T!=NULL && NT_SUCCESS(T->Cp.Status) && T->Cp.Reason==0 && T->Checks==0x3F && T->Cp.WptrAfter==2287 &&
        T->DataAfter[0]==0xCAFE0017 && T->DataAfter[1]==0xCAFE0018 && T->DataAfter[2]==0xCAFE0019 &&
        T->RestoreMismatch==0 && NT_SUCCESS(T->RestoreStatus);
}
static BOOLEAN GmuW1Pointer(GMU_W1_TRACE* G,ULONG Value)
{
    if (Value>G->MaxRptrSeen) { G->MaxRptrSeen=Value; }
    if (Value>=GmuW1Size) { G->Cp.Reason=18; return FALSE; }
    return TRUE;
}
static BOOLEAN GmuW1Pointers(GMU_W1_TRACE* G)
{
    return GmuW1Pointer(G,G->Cp.Rptr) && GmuW1Pointer(G,G->Cp.ShadowAfter);
}
static NTSTATUS GmuW1Wait(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_W1_TRACE* G,
                         volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow,volatile ULONG* Data,
                         ULONG Target,ULONG Last,BOOLEAN Final,ULONGLONG Begin)
{
    GMU_G2_TRACE* C=&G->Cp;
    // Check RPTR before every AHB retry too; do not hide a size mismatch inside the 2-ms retry loop.
    for (ULONG retry=0;;++retry)
    {
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
        if (G->MaskAfter) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
        KeMemoryBarrier(); C->ShadowAfter=*Shadow; C->Rptr=READ_REGISTER_ULONG(Cp+6);
        if (!GmuW1Pointers(G)) { return STATUS_IO_DEVICE_ERROR; }
        C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201);
        if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
        if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
        if (retry) { WRITE_REGISTER_ULONG(Cp+7,Target); KeMemoryBarrier(); ++C->WptrWrites; }
        C->FenceStatus=READ_REGISTER_ULONG(GmuReg(S,0x23B13));
        if (!(C->FenceStatus&1)) { break; }
        if (retry==200 || KeQueryInterruptTime()-Begin>=20000) { C->Reason=10; return STATUS_IO_TIMEOUT; }
        KeStallExecutionProcessor(10);
    }
    for (ULONG poll=0;;++poll)
    {
        C->Polls=poll;
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
        if (G->MaskAfter) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
        KeMemoryBarrier(); C->ShadowAfter=*Shadow; C->Rptr=READ_REGISTER_ULONG(Cp+6);
        if (!GmuW1Pointers(G)) { return STATUS_IO_DEVICE_ERROR; }
        C->WptrAfter=READ_REGISTER_ULONG(Cp+7); C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201);
        C->RbbmStatus=READ_REGISTER_ULONG(Gpu+0x210); C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        G->IbRemaining=READ_REGISTER_ULONG(Cp+0x12A);
        if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
        if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
        if (KeQueryInterruptTime()-Begin>=1000000) { C->Reason=16; return STATUS_IO_TIMEOUT; }
        const BOOLEAN idle=!Final || (!(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23)) && G->IbRemaining==0);
        if (Data[29]==(0xB1000000UL|Last) && C->Rptr==Target && C->WptrAfter==Target && C->ShadowAfter==Target && idle)
        { return STATUS_SUCCESS; }
        KeStallExecutionProcessor(100);
    }
}
static NTSTATUS GmuW1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_W1_TRACE* G,HANDLE Diag,const GMU_D1_TRACE* D)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1D00); if (!NT_SUCCESS(st)) { return st; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    PVOID band=NULL; ULONG* backup=NULL;
    volatile ULONG* data=words+0x7000/4;
    PVOID maps[2]={}; const ULONG offsets[]={0,0x2000};
    for (ULONG i=0;i<2;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        st=GmuG2Guard(S,Params,Cc,T,C,0x1D01);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                C->ShadowAfter=C->ShadowBefore; C->WptrAfter=C->WptrBefore;
                if (!GmuW1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; }
                if (NT_SUCCESS(st)) { G->RbCntlLive=READ_REGISTER_ULONG(cp+2); }
                BOOLEAN previous=C->ShadowBefore==2287 && C->WptrBefore==2287 && C->Rptr==2287;
                const ULONG meInit[]={0x70C80008,0x2F,3,0x20000000,0,0,0,0,0};
                for (ULONG i=0;i<9;++i)
                { if (words[i]!=meInit[i] || words[30+i]!=GmuG4Packet[i] || words[39+i]!=GmuG5Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<11;++i) { if (words[9+i]!=GmuG3Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<5;++i)
                { if (words[20+i]!=GmuG3bPacket[i] || words[25+i]!=GmuG3cPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<13;++i) { if (words[48+i]!=GmuG5bPacket[i]) { previous=FALSE; } }
                for (ULONG i=0;i<12;++i) { if (words[61+i]!=GmuZ1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<6;++i) { if (words[0x6000/4+i]!=GmuG4Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<36;++i) { if (words[0x6100/4+i]!=GmuG5Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<10;++i) { if (words[0x6200/4+i]!=GmuG5bIb[i]) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i) { if (words[73+i]!=GmuS1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<53;++i) { if (words[0x6300/4+i]!=GmuS1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x6800/4+i]!=GmuS1Shader[i]) { previous=FALSE; } }
                const ULONG expected[]={0xCAFE0002,0xFFFFFFFF,0xCAFE0001,0xFFFFFFFF,0xCAFE0003,0xFFFFFFFF,
                    0xCAFE0004,0xFFFFFFFF,0xCAFE0006,0xCAFE0007,0xCAFE0008,0xCAFE0009,0xCAFE000A,0xCAFE000B,0xCAFE000C};
                for (ULONG i=0;i<15;++i) { if (data[i]!=expected[i]) { previous=FALSE; } }
                if (words[0x7C00/4]!=0xC0DE5EED) { previous=FALSE; }
                for (ULONG i=1;i<16;++i) { if (words[0x7C00/4+i]!=0xFFFFFFFF) { previous=FALSE; } }
                if (!(D!=NULL && EpiGmuD1DisplayMatches(D->Disp))) { previous=FALSE; }
                GMU_REGION* ext=GmuFind(S,0x71000000,0x120000);
                if (ext==NULL || !ext->External || !ext->Mapped || ext->Pa.QuadPart!=0xE1462000 ||
                    ext->Size!=0x120000 || ext->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE)) { previous=FALSE; }
                if (data[15]!=0xCAFE000D || data[16]!=0xCAFE000E) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[82+i]!=GmuR1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<314;++i) { if (words[0x4000/4+i]!=GmuR1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i)
                { if (words[0x4800/4+i]!=GmuR1Vs[i] || words[0x4880/4+i]!=GmuR1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<8;++i) { if (words[0x4900/4+i]!=GmuR1Vertices[i]) { previous=FALSE; } }
                for (ULONG i=0;i<256;++i)
                {
                    const BOOLEAN inside=i%16>=4 && i%16<12 && i/16>=4 && i/16<12;
                    if (words[0x4C00/4+i]!=(inside ? 0x000033FFUL : 0xFFFFFFFFUL)) { previous=FALSE; }
                }
                if (data[17]!=0xCAFE000F || data[18]!=0xCAFE0010 || data[19]!=0xCAFE0011) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[91+i]!=GmuD1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<72;++i) { if (words[0x4500/4+i]!=GmuD1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x4A00/4+i]!=GmuD1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<8;++i)
                { if (words[0x4A80/4+i]!=GmuD1VertA[i] || words[0x4AA0/4+i]!=GmuD1VertB[i]) { previous=FALSE; } }
                if (data[20]!=0xCAFE0012 || data[21]!=0xCAFE0013 || data[22]!=0xCAFE0014 ||
                    data[23]!=0xCAFE0015 || data[24]!=0xCAFE0016 || data[25]!=0xA10000F0) { previous=FALSE; }
                for (ULONG i=0;i<9;++i) { if (words[100+i]!=GmuV1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<115;++i) { if (words[0x5100/4+i]!=GmuV1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<32;++i) { if (words[0x5400/4+i]!=GmuV1Fs[i]) { previous=FALSE; } }
                for (ULONG i=0;i<24;++i)
                { if (words[0x5480/4+i]!=GmuV1VertA[i] || words[0x5500/4+i]!=GmuV1VertB[i]) { previous=FALSE; } }
                for (ULONG f=0;f<GmuA1Frames;++f) for (ULONG i=0;i<9;++i)
                { if (words[109+9*f+i]!=(i<8 ? GmuA1Packet[i] : (0xA1000000UL|f))) { previous=FALSE; } }
                for (ULONG i=0;i<61;++i) { if (words[0x5600/4+i]!=GmuA1Ib[i]) { previous=FALSE; } }
                for (ULONG i=0;i<96;++i)
                { if (words[0x5800/4+i]!=GmuA1VertexWord(0,FALSE,i) || words[0x5A00/4+i]!=GmuA1VertexWord(0,TRUE,i)) { previous=FALSE; } }
                for (ULONG i=0;i<9;++i) { if (words[2278+i]!=GmuT1Packet[i]) { previous=FALSE; } }
                for (ULONG i=0;i<0x400/4;++i)
                    { if (words[0x6400/4+i]!=(i<107 ? GmuT1Ib[i] : 0)) { previous=FALSE; } }
                for (ULONG off=0x6880;off<0x6E00;off+=4)
                    { if (words[off/4]!=GmuT1Word(off)) { previous=FALSE; } }
                if (data[26]!=0xCAFE0017 || data[27]!=0xCAFE0018 || data[28]!=0xCAFE0019 || data[29]!=0 ||
                    G->RbCntlLive!=0x20B || G->RbCntlG1!=0x20B) { previous=FALSE; }
                if (NT_SUCCESS(st) && !previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
                if (NT_SUCCESS(st)) { G->Checks|=1; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1D02);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1D08);
            if (NT_SUCCESS(st))
            {
                backup=static_cast<ULONG*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,0x124000,EPI_GMU_TAG));
                if (backup==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                else
                {
                    PHYSICAL_ADDRESS pa; pa.QuadPart=0xE1462000;
                    band=MmMapIoSpaceEx(pa,0x120000,PAGE_READWRITE|PAGE_NOCACHE);
                    if (band==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                    else
                    {
                        for (ULONG i=0;i<0x120000/4;++i) { backup[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i); }
                        for (ULONG i=0;i<0x4000/4;++i) { backup[0x120000/4+i]=words[0x4000/4+i]; }
                        KeMemoryBarrier();
                    }
                }
                G->BackupStatus=st;
                if (!NT_SUCCESS(st)) { C->Reason=GX_REG_MAP_FAILED; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1D03);
            if (NT_SUCCESS(st))
            {
                data[29]=0xFFFFFFFF; KeMemoryBarrier(); G->Prepared=1;
                if (data[29]!=0xFFFFFFFF) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
            }
        }
        if (NT_SUCCESS(st))
        {
            ULONGLONG firstPublish=0;
            for (ULONG k=0;k<GmuW1Pubs;++k)
            {
                const ULONG first=k<600 ? k : 600+(k-600)*8,count=k<600 ? 1 : 8,last=first+count-1;
                const ULONG absolute=2287+9*first,target=(absolute+9*count)%GmuW1Size;
                if ((k%64)==0) { st=GmuG2Guard(S,Params,Cc,T,C,0x1D40+k/64); }
                else if (!GmuG2Live(S,Cc,T,C)) { st=STATUS_INVALID_DEVICE_STATE; }
                if (NT_SUCCESS(st))
                {
                    G->MaskAfter=READ_REGISTER_ULONG(gpu+0x38);
                    if (G->MaskAfter) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                }
                if (NT_SUCCESS(st))
                {
                    for (ULONG i=first;i<=last;++i) for (ULONG j=0;j<9;++j)
                        { words[(2287+9*i+j)%GmuW1Size]=GmuW1Word(i,j); }
                    KeMemoryBarrier();
                    for (ULONG i=first;i<=last;++i) for (ULONG j=0;j<9;++j)
                        if (words[(2287+9*i+j)%GmuW1Size]!=GmuW1Word(i,j)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                    KeMemoryBarrier();
                }
                if (NT_SUCCESS(st))
                {
                    const ULONGLONG begin=KeQueryInterruptTime();
                    if (!C->Attempted) { firstPublish=begin; }
                    C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,target); KeMemoryBarrier(); ++C->WptrWrites;
                    st=GmuW1Wait(S,Cc,T,G,cp,gpu,rptr,data,target,last,k+1==GmuW1Pubs,begin);
                    const ULONGLONG end=KeQueryInterruptTime(); const ULONG us=static_cast<ULONG>((end-begin+9)/10);
                    if (us>G->WaitMaxUs) { G->WaitMaxUs=us; } G->WaitTotalUs+=us;
                    G->ElapsedMs=static_cast<ULONG>((end-firstPublish)/10000);
                    if (NT_SUCCESS(st))
                    {
                        ++G->PubsDone; G->PacketsDone+=count;
                        G->WrapCount+=(absolute+9*count)/GmuW1Size-absolute/GmuW1Size;
                        if (target==0) { G->ZeroWptrPub=k; G->ZeroSeenRptr=C->Rptr; }
                        if (first<=656 && last>=656 && (2287+9*656)%GmuW1Size==4095) { G->StraddlePub=k; }
                    }
                }
                if (!NT_SUCCESS(st))
                { G->FailPub=k; G->FailPacket=last; G->FailData29=data[29]; G->FailRptr=C->Rptr; G->FailWptr=C->WptrAfter; break; }
            }
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            if (C->Reason==18) { break; }
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1D10+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
            if (i==0)
            {
                KeMemoryBarrier(); C->ShadowAfter=*rptr;
                if (!GmuW1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; break; }
            }
        }
        if (!NT_SUCCESS(st) && C->Reason!=18) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1D20);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        if (!GmuW1Pointer(G,C->ShadowAfter)) { st=STATUS_IO_DEVICE_ERROR; }
        for (ULONG i=0;i<4;++i) { G->DataAfter[i]=data[26+i]; }
        if (G->Prepared)
        {
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (G->PubsDone==650 && G->PacketsDone==1000 && G->WrapCount==2 && G->ZeroWptrPub==200 && G->ZeroSeenRptr==0 &&
                G->StraddlePub==607 && G->DataAfter[3]==0xB10003E7 && G->IbRemaining==0 && C->ShadowAfter==GmuW1End && C->Rptr==GmuW1End && C->WptrAfter==GmuW1End && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }

        }
        if (band!=NULL && NT_SUCCESS(G->BackupStatus))
        {
            G->RingBad=0; G->OutsideRingBad=0;
            for (ULONG i=0;i<GmuW1Size;++i)
            {
                const ULONG expected=GmuW1Expected(i),value=words[i];
                if (value!=expected)
                { ++G->RingBad; if (G->RingFirst==0xFFFFFFFF) { G->RingFirst=i; G->RingValue=value; G->RingExpected=expected; } }
            }
            for (ULONG off=0x4000;off<0x8000;off+=4)
            {
                if (off==0x7068 || off==0x706C || off==0x7074) { continue; }
                const ULONG old=backup[0x120000/4+(off-0x4000)/4],value=words[off/4];
                if (old!=value)
                { ++G->OutsideRingBad; if (G->OutsideRingFirst==0xFFFFFFFF) { G->OutsideRingFirst=off; G->OutsideRingOld=old; G->OutsideRingNew=value; } }
            }
            if (G->RingBad==0 && G->OutsideRingBad==0) { G->Checks|=4; }
            G->SqGoodEven=0; G->SqGoodOdd=0; G->SqBad=0; G->SqZero=0; G->SqPad=0;
            G->TexelBad[0]=0; G->TexelBad[1]=0; G->BoxGoodEven=0; G->BoxGoodOdd=0; G->BoxBad=0; G->OutsideChanged=0;
            for (ULONG i=0;i<3;++i) { G->MaxErr[i]=0; }
            for (ULONG i=0;i<0x120000/4;++i)
            {
                const ULONG offset=i*4,fb=0x462000+offset,y=fb/4320,x=(fb%4320)/4;
                const ULONG value=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i);
                if (x>=412 && x<668 && y>=1072 && y<1328)
                {
                    const ULONG v=(y-1072)/2;
                    if (x>=460 && x<620 && v>=24 && v<104)
                    {
                        const ULONG ti=(x-460)/20,tj=(v-24)/10,texel=tj*8+ti,expected=GmuT1Texel(ti,tj);
                        GmuV1WithinTolerance(value,expected,G->MaxErr); // diagnostic only; acceptance is exact
                        if (value==0 || value==0xFF000000) { ++G->SqZero; }
                        if (value==0xFFFF00FF) { ++G->SqPad; }
                        if (value==expected) { if (y&1) { ++G->SqGoodOdd; } else { ++G->SqGoodEven; } }
                        else
                        {
                            ++G->SqBad; G->TexelBad[texel/32]|=1UL<<(texel%32);
                            if (G->SqFirst==0xFFFFFFFF) { G->SqFirst=offset; G->SqValue=value; G->SqExpected=expected; }
                        }
                    }
                    else if (value==backup[i]) { if (y&1) { ++G->BoxGoodOdd; } else { ++G->BoxGoodEven; } }
                    else { ++G->BoxBad; if (G->BoxFirst==0xFFFFFFFF) { G->BoxFirst=offset; G->BoxValue=value; G->BoxOld=backup[i]; } }
                }
                else if (value!=backup[i])
                {
                    ++G->OutsideChanged;
                    if (G->OutsideFirst==0xFFFFFFFF) { G->OutsideFirst=offset; G->OutsideOld=backup[i]; G->OutsideNew=value; }
                }
            }
            const ULONG sample[][2]={{470,29},{610,29},{610,99},{470,99},{530,59},{550,74},{479,33},{480,34},{414,2},{665,125}};
            for (ULONG i=0;i<10;++i) for (ULONG parity=0;parity<2;++parity)
            { G->Sample[i*2+parity]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+((1072+sample[i][1]*2+parity)*4320+sample[i][0]*4-0x462000)/4); }
            if (G->SqGoodEven==12800 && G->SqGoodOdd==12800) { G->Checks|=8; }
            if (G->BoxGoodEven==19968 && G->BoxGoodOdd==19968 && G->OutsideChanged==0) { G->Checks|=16; }
        }
    }
    KeMemoryBarrier(); G->ScanCount=0; G->ScanDwords=0;
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if (off<0x4000 || (off>=0x5600 && off<0x5B80) || (off>=0x6400 && off<0x6800) || (off>=0x6880 && off<0x6E00) || (off>=0x5100 && off<0x52CC) || (off>=0x5400 && off<0x5560) || (off>=0x4500 && off<0x4620) || (off>=0x4A00 && off<0x4AC0) || (off>=0x4000 && off<0x44E8) || (off>=0x4800 && off<0x4920) ||
            (off>=0x4C00 && off<0x5040) || (off>=0x6000 && off<0x6240) || (off>=0x6300 && off<0x6400) ||
            (off>=0x6800 && off<0x6880) || (off>=0x7000 && off<0x7078) ||
            (off>=0x7400 && off<0x7840) || (off>=0x7C00 && off<0x7C40)) { continue; }
        ++G->ScanDwords;
        const ULONG value=words[off/4];
        if ((value&0xFF000000UL)==0xFF000000UL && value!=0xFFFFFFFF) { ++G->ScanCount; if (G->ScanFirst==0xFFFFFFFF) { G->ScanFirst=off; } }
    }
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (band!=NULL && NT_SUCCESS(G->BackupStatus))
    {
        if (C->Attempted && NT_SUCCESS(st))
        {
            C->Status=st;
            NTSTATUS held=GmuStep(Params,0x1D30);
            if (NT_SUCCESS(held)) { held=EpiSetValue(Diag,L"GpuGmuW1",REG_BINARY,G,sizeof(*G)); }
            if (NT_SUCCESS(held)) { held=ZwFlushKey(Diag); }
            if (NT_SUCCESS(held))
            {
                held=GmuA1Pause(2000);
                if (NT_SUCCESS(held)) { G->HoldMs=2000; }
            }
            G->HoldStatus=held;
            if (NT_SUCCESS(st) && !NT_SUCCESS(held)) { st=held; C->Reason=6; }
        }
        // Restoring display RAM is mandatory even when a breadcrumb or GX prerequisite failed.
        NTSTATUS restored=GmuStep(Params,0x1D31);
        for (ULONG i=0;i<0x120000/4;++i) { WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i,backup[i]); }
        KeMemoryBarrier();
        const NTSTATUS verified=GmuStep(Params,0x1D32);
        G->RestoreMismatch=0;
        for (ULONG i=0;i<0x120000/4;++i) if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i)!=backup[i])
        { ++G->RestoreMismatch; if (G->RestoreFirst==0xFFFFFFFF) { G->RestoreFirst=i*4; } }
        if (NT_SUCCESS(restored) && !NT_SUCCESS(verified)) { restored=verified; }
        if (G->RestoreMismatch==0) { G->Checks|=32; }
        else if (NT_SUCCESS(restored)) { restored=STATUS_IO_DEVICE_ERROR; }
        G->RestoreStatus=restored;
        if (NT_SUCCESS(st) && !NT_SUCCESS(restored)) { st=restored; C->Reason=13; }
    }
    if (band!=NULL) { MmUnmapIoSpace(band,0x120000); }
    if (backup!=NULL) { ExFreePoolWithTag(backup,EPI_GMU_TAG); }
    if (NT_SUCCESS(st) && G->Checks!=0x3F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<2;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
// M-1 keeps all diagnostics in the pooled boot traces; immutable command tables use no kernel stack.
#include "epi_gmu_m1_table.inc"
struct GMU_M1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG WChecks,WReason,CcuBefore,CcuAfter,RbCntlLive;
    ULONG TileData[12],FlushData[2],Data42,TilesDone,WaitUs,ElapsedMs;
    ULONG RingBad,RingFirst,RingValue,RingExpected,OutsideRingBad,OutsideRingFirst,OutsideRingOld,OutsideRingNew;
    ULONG SqGood[2],SqBad,SqFirst,SqValue,SqExpected;
    ULONG ClearGood,ClearBad,ClearFirst,ClearValue;
    ULONG BoxGood,BoxBad,BoxFirst,BoxValue,BoxOld;
    ULONG OutsideChanged,OutsideFirst,OutsideOld,OutsideNew,TileBad[12],Sample[24];
    ULONG CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAfter,ScanCount,ScanFirst,ScanDwords;
    ULONG RestoreMismatch,RestoreFirst,HoldMs,Checks,Prepared;
    NTSTATUS BackupStatus,HoldStatus,RestoreStatus;
};
static_assert(sizeof(GMU_M1_TRACE)==600,"M-1 diagnostic ABI");
static VOID GmuM1TraceInit(GMU_M1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp);
    t->TilesDone=0; t->HoldMs=0; t->Checks=0; t->Prepared=0;
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
    t->BackupStatus=t->HoldStatus=t->RestoreStatus=STATUS_NOT_FOUND;
}
static BOOLEAN GmuM1CanFollow(const GMU_W1_TRACE* W)
{
    return W!=NULL && NT_SUCCESS(W->Cp.Status) && W->Cp.Reason==0 && W->Checks==0x3F &&
        W->PubsDone==650 && W->PacketsDone==1000 && W->Cp.Rptr==3095 && W->Cp.WptrAfter==3095 && W->Cp.ShadowAfter==3095 &&
        W->DataAfter[3]==0xB10003E7 && W->RestoreMismatch==0 && NT_SUCCESS(W->RestoreStatus);
}
static BOOLEAN GmuM1Pointer(GMU_M1_TRACE* G,ULONG Value)
{
    if (Value>=4096) { G->Cp.Reason=18; return FALSE; }
    return TRUE;
}
static BOOLEAN GmuM1Pointers(GMU_M1_TRACE* G)
{
    return GmuM1Pointer(G,G->Cp.Rptr) && GmuM1Pointer(G,G->Cp.ShadowAfter);
}
static ULONG GmuM1OutsideExpected(ULONG Offset,ULONG Old)
{
    for (ULONG j=0;j<RTL_NUMBER_OF(GmuM1Ibs);++j)
    {
        const GMU_M1_IB* ib=&GmuM1Ibs[j];
        if (Offset>=ib->Offset && Offset<ib->Offset+4*ib->Count) { return ib->Words[(Offset-ib->Offset)/4]; }
    }
    if (Offset>=0x7078 && Offset<0x70A8) { return 0xB2000000UL|((Offset-0x7078)/4); }
    if (Offset==0x70A8) { return 0xCAFE001A; }
    if (Offset==0x70AC) { return 0xB2000100; }
    if (Offset==0x70B0) { return 0xB2000101; }
    return Old;
}
static NTSTATUS GmuM1Wait(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_M1_TRACE* G,
                         volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow,volatile ULONG* Data,
                         ULONGLONG Begin)
{
    GMU_G2_TRACE* C=&G->Cp;
    // Check RPTR before every AHB retry too; do not hide a size mismatch inside the 2-ms retry loop.
    for (ULONG retry=0;;++retry)
    {
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
        if (G->MaskAfter) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
        KeMemoryBarrier(); C->ShadowAfter=*Shadow; C->Rptr=READ_REGISTER_ULONG(Cp+6);
        if (!GmuM1Pointers(G)) { return STATUS_IO_DEVICE_ERROR; }
        C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201);
        if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
        if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
        if (retry) { WRITE_REGISTER_ULONG(Cp+7,3272); KeMemoryBarrier(); ++C->WptrWrites; }
        C->FenceStatus=READ_REGISTER_ULONG(GmuReg(S,0x23B13));
        if (!(C->FenceStatus&1)) { break; }
        if (retry==200 || KeQueryInterruptTime()-Begin>=20000) { C->Reason=10; return STATUS_IO_TIMEOUT; }
        KeStallExecutionProcessor(10);
    }
    for (ULONG poll=0;;++poll)
    {
        C->Polls=poll;
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
        if (G->MaskAfter) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
        KeMemoryBarrier(); C->ShadowAfter=*Shadow; C->Rptr=READ_REGISTER_ULONG(Cp+6);
        if (!GmuM1Pointers(G)) { return STATUS_IO_DEVICE_ERROR; }
        C->WptrAfter=READ_REGISTER_ULONG(Cp+7); C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201);
        C->RbbmStatus=READ_REGISTER_ULONG(Gpu+0x210); C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        G->IbRemaining=READ_REGISTER_ULONG(Cp+0x12A);
        if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
        if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
        if (KeQueryInterruptTime()-Begin>=1000000) { C->Reason=16; return STATUS_IO_TIMEOUT; }
        const BOOLEAN idle= (!(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23)) && G->IbRemaining==0);
        if (Data[42]==0xCAFE001A && C->Rptr==3272 && C->WptrAfter==3272 && C->ShadowAfter==3272 && idle)
        { return STATUS_SUCCESS; }
        KeStallExecutionProcessor(100);
    }
}
static NTSTATUS GmuM1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_M1_TRACE* G,HANDLE Diag,const GMU_D1_TRACE* D)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1E00); if (!NT_SUCCESS(st)) { return st; }
    if (G->WChecks!=0x3F || G->WReason!=0) { C->Reason=8; return STATUS_INVALID_DEVICE_STATE; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    PVOID band=NULL; ULONG* backup=NULL;
    volatile ULONG* data=words+0x7000/4;
    PVOID maps[3]={}; const ULONG offsets[]={0,0x2000,0x23000};
    for (ULONG i=0;i<3;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        volatile ULONG* ccu=static_cast<volatile ULONG*>(maps[2])+0x207;
        st=GmuG2Guard(S,Params,Cc,T,C,0x1E01);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                C->ShadowAfter=C->ShadowBefore; C->WptrAfter=C->WptrBefore;
                if (!GmuM1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; }
                if (NT_SUCCESS(st)) { G->RbCntlLive=READ_REGISTER_ULONG(cp+2); }
                BOOLEAN previous=C->ShadowBefore==3095 && C->WptrBefore==3095 && C->Rptr==3095;
                if (NT_SUCCESS(st)) { G->CcuBefore=READ_REGISTER_ULONG(ccu); }
                if (G->RbCntlLive!=0x20B || G->CcuBefore!=0x10000000 || data[29]!=0xB10003E7) { previous=FALSE; }
                for (ULONG i=0;i<4096;++i) { if (words[i]!=GmuW1Expected(i)) { previous=FALSE; } }
                for (ULONG i=0;i<107;++i) { if (words[0x6400/4+i]!=GmuT1Ib[i]) { previous=FALSE; } }
                for (ULONG off=0x6880;off<0x6E00;off+=4)
                    { if (words[off/4]!=GmuT1Word(off)) { previous=FALSE; } }
                if (!(D!=NULL && EpiGmuD1DisplayMatches(D->Disp))) { previous=FALSE; }
                GMU_REGION* ext=GmuFind(S,0x71000000,0x120000);
                if (ext==NULL || !ext->External || !ext->Mapped || ext->Pa.QuadPart!=0xE1462000 ||
                    ext->Size!=0x120000 || ext->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE)) { previous=FALSE; }
                // These are unused W-1 ranges; never overwrite an unexpected earlier resource.
                for (ULONG off=0x5B80;off<0x5EC0;off+=4) { if (words[off/4]!=0) { previous=FALSE; } }
                for (ULONG off=0x7078;off<0x70B4;off+=4) { if (words[off/4]!=0) { previous=FALSE; } }
                for (ULONG off=0x7840;off<0x7B40;off+=4) { if (words[off/4]!=0) { previous=FALSE; } }
                if (NT_SUCCESS(st) && !previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
                if (NT_SUCCESS(st)) { G->Checks|=1; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1E02);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1E08);
            if (NT_SUCCESS(st))
            {
                backup=static_cast<ULONG*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,0x124000,EPI_GMU_TAG));
                if (backup==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                else
                {
                    PHYSICAL_ADDRESS pa; pa.QuadPart=0xE1462000;
                    band=MmMapIoSpaceEx(pa,0x120000,PAGE_READWRITE|PAGE_NOCACHE);
                    if (band==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                    else
                    {
                        for (ULONG i=0;i<0x120000/4;++i) { backup[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i); }
                        for (ULONG i=0;i<0x4000/4;++i) { backup[0x120000/4+i]=words[0x4000/4+i]; }
                        KeMemoryBarrier();
                    }
                }
                G->BackupStatus=st;
                if (!NT_SUCCESS(st)) { C->Reason=GX_REG_MAP_FAILED; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1E03);
            if (NT_SUCCESS(st))
            {
                for (ULONG j=0;j<RTL_NUMBER_OF(GmuM1Ibs);++j)
                {
                    const GMU_M1_IB* ib=&GmuM1Ibs[j];
                    RtlCopyMemory(const_cast<ULONG*>(words+ib->Offset/4),ib->Words,ib->Count*4);
                }
                for (ULONG i=30;i<=44;++i) { data[i]=0xFFFFFFFF; }
                for (ULONG i=0;i<177;++i) { words[3095+i]=GmuM1Packet[i]; }
                KeMemoryBarrier(); G->Prepared=1;
                for (ULONG j=0;j<RTL_NUMBER_OF(GmuM1Ibs);++j)
                {
                    const GMU_M1_IB* ib=&GmuM1Ibs[j];
                    for (ULONG i=0;i<ib->Count;++i)
                        { if (words[ib->Offset/4+i]!=ib->Words[i]) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; } }
                }
                for (ULONG i=30;i<=44;++i) { if (data[i]!=0xFFFFFFFF) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; } }
                for (ULONG i=0;i<177;++i)
                    { if (words[3095+i]!=GmuM1Packet[i]) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; } }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1E04);
            if (NT_SUCCESS(st))
            {
                G->MaskAfter=READ_REGISTER_ULONG(gpu+0x38);
                if (G->MaskAfter) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            }
            if (NT_SUCCESS(st))
            {
                KeMemoryBarrier();
                const ULONGLONG begin=KeQueryInterruptTime();
                C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,3272); KeMemoryBarrier(); ++C->WptrWrites;
                st=GmuM1Wait(S,Cc,T,G,cp,gpu,rptr,data,begin);
                const ULONGLONG end=KeQueryInterruptTime();
                G->WaitUs=static_cast<ULONG>((end-begin+9)/10);
                G->ElapsedMs=static_cast<ULONG>((end-begin)/10000);
            }
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            if (C->Reason==18) { break; }
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1E10+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
            if (i==0)
            {
                KeMemoryBarrier(); C->ShadowAfter=*rptr;
                if (!GmuM1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; break; }
            }
        }
        if (!NT_SUCCESS(st) && C->Reason!=18) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1E20);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        if (!GmuM1Pointer(G,C->ShadowAfter)) { st=STATUS_IO_DEVICE_ERROR; }
        for (ULONG i=0;i<12;++i) { G->TileData[i]=data[30+i]; }
        G->TilesDone=0;
        while (G->TilesDone<12 && G->TileData[G->TilesDone]==(0xB2000000UL|G->TilesDone)) { ++G->TilesDone; }
        G->FlushData[0]=data[43]; G->FlushData[1]=data[44]; G->Data42=data[42];
        const ULONG ccuReason=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1E1C);
        if (NT_SUCCESS(sampled) && ccuReason!=18) { G->CcuAfter=READ_REGISTER_ULONG(ccu); }
        if (!NT_SUCCESS(st)) { C->Reason=ccuReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        if (G->Prepared)
        {
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (G->TilesDone==12 && G->FlushData[0]==0xB2000100 && G->FlushData[1]==0xB2000101 &&
                G->Data42==0xCAFE001A && G->CcuAfter==0x10000000 && G->IbRemaining==0 &&
                C->ShadowAfter==3272 && C->Rptr==3272 && C->WptrAfter==3272 && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }

        }
        if (band!=NULL && NT_SUCCESS(G->BackupStatus))
        {
            G->RingBad=0; G->OutsideRingBad=0;
            for (ULONG i=0;i<4096;++i)
            {
                const ULONG expected=i>=3095 && i<3272 ? GmuM1Packet[i-3095] : GmuW1Expected(i),value=words[i];
                if (value!=expected)
                { ++G->RingBad; if (G->RingFirst==0xFFFFFFFF) { G->RingFirst=i; G->RingValue=value; G->RingExpected=expected; } }
            }
            for (ULONG off=0x4000;off<0x8000;off+=4)
            {
                const ULONG old=GmuM1OutsideExpected(off,backup[0x120000/4+(off-0x4000)/4]),value=words[off/4];
                if (old!=value)
                { ++G->OutsideRingBad; if (G->OutsideRingFirst==0xFFFFFFFF) { G->OutsideRingFirst=off; G->OutsideRingOld=old; G->OutsideRingNew=value; } }
            }
            if (G->RingBad==0 && G->OutsideRingBad==0) { G->Checks|=4; }
            G->SqGood[0]=G->SqGood[1]=G->SqBad=0;
            G->ClearGood=G->ClearBad=G->BoxGood=G->BoxBad=G->OutsideChanged=0;
            for (ULONG k=0;k<12;++k) { G->TileBad[k]=0; }
            for (ULONG i=0;i<0x120000/4;++i)
            {
                const ULONG offset=i*4,fb=0x462000+offset,y=fb/4320,x=(fb%4320)/4,parity=y&1;
                const ULONG value=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i);
                const BOOLEAN row=y>=1072 && y<1328;
                const ULONG v=row ? (y-1072)/2 : 0;
                BOOLEAN bad=FALSE;
                if (row && x>=460 && x<620 && v>=24 && v<104)
                {
                    const ULONG expected=GmuT1Texel((x-460)/20,(v-24)/10);
                    if (value==expected) { ++G->SqGood[parity]; }
                    else
                    {
                        bad=TRUE; ++G->SqBad;
                        if (G->SqFirst==0xFFFFFFFF) { G->SqFirst=offset; G->SqValue=value; G->SqExpected=expected; }
                    }
                }
                else if (row && parity==0 && x>=400 && x<672)
                {
                    if (value==0xFF2E5A8C) { ++G->ClearGood; }
                    else { bad=TRUE; ++G->ClearBad; if (G->ClearFirst==0xFFFFFFFF) { G->ClearFirst=offset; G->ClearValue=value; } }
                }
                else if (row && parity==1 && x>=412 && x<668)
                {
                    if (value==backup[i]) { ++G->BoxGood; }
                    else { bad=TRUE; ++G->BoxBad; if (G->BoxFirst==0xFFFFFFFF) { G->BoxFirst=offset; G->BoxValue=value; G->BoxOld=backup[i]; } }
                }
                else if (value!=backup[i])
                {
                    bad=TRUE; ++G->OutsideChanged;
                    if (G->OutsideFirst==0xFFFFFFFF) { G->OutsideFirst=offset; G->OutsideOld=backup[i]; G->OutsideNew=value; }
                }
                const ULONG tx=(x+8*parity)/128;
                if (bad && row && tx>=3 && tx<=5) { ++G->TileBad[parity*6+(v/64)*3+tx-3]; }
            }
            const ULONG sample[][2]={{470,29},{610,29},{610,99},{470,99},{530,59},{550,74},{479,33},{480,34},{414,2},{665,125},{404,2},{668,125}};
            for (ULONG i=0;i<12;++i) for (ULONG parity=0;parity<2;++parity)
            { G->Sample[i*2+parity]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+((1072+sample[i][1]*2+parity)*4320+sample[i][0]*4-0x462000)/4); }
            if (G->SqGood[0]==12800 && G->SqGood[1]==12800) { G->Checks|=8; }
            if (G->ClearGood==22016 && G->BoxGood==19968 && G->OutsideChanged==0) { G->Checks|=16; }
        }
    }
    KeMemoryBarrier(); G->ScanCount=0; G->ScanDwords=0;
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if ((off>=0x5B80 && off<0x5EC0) || (off>=0x7078 && off<0x70B4) || (off>=0x7840 && off<0x7B40) || off<0x4000 || (off>=0x5600 && off<0x5B80) || (off>=0x6400 && off<0x6800) || (off>=0x6880 && off<0x6E00) || (off>=0x5100 && off<0x52CC) || (off>=0x5400 && off<0x5560) || (off>=0x4500 && off<0x4620) || (off>=0x4A00 && off<0x4AC0) || (off>=0x4000 && off<0x44E8) || (off>=0x4800 && off<0x4920) ||
            (off>=0x4C00 && off<0x5040) || (off>=0x6000 && off<0x6240) || (off>=0x6300 && off<0x6400) ||
            (off>=0x6800 && off<0x6880) || (off>=0x7000 && off<0x7078) ||
            (off>=0x7400 && off<0x7840) || (off>=0x7C00 && off<0x7C40)) { continue; }
        ++G->ScanDwords;
        const ULONG value=words[off/4];
        if ((value&0xFF000000UL)==0xFF000000UL && value!=0xFFFFFFFF) { ++G->ScanCount; if (G->ScanFirst==0xFFFFFFFF) { G->ScanFirst=off; } }
    }
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (band!=NULL && NT_SUCCESS(G->BackupStatus))
    {
        if (C->Attempted && NT_SUCCESS(st))
        {
            C->Status=st;
            NTSTATUS held=GmuStep(Params,0x1E30);
            if (NT_SUCCESS(held)) { held=EpiSetValue(Diag,L"GpuGmuM1",REG_BINARY,G,sizeof(*G)); }
            if (NT_SUCCESS(held)) { held=ZwFlushKey(Diag); }
            if (NT_SUCCESS(held))
            {
                held=GmuA1Pause(5000);
                if (NT_SUCCESS(held)) { G->HoldMs=5000; }
            }
            G->HoldStatus=held;
            if (NT_SUCCESS(st) && !NT_SUCCESS(held)) { st=held; C->Reason=6; }
        }
        // Restoring display RAM is mandatory even when a breadcrumb or GX prerequisite failed.
        NTSTATUS restored=GmuStep(Params,0x1E31);
        for (ULONG i=0;i<0x120000/4;++i) { WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i,backup[i]); }
        KeMemoryBarrier();
        const NTSTATUS verified=GmuStep(Params,0x1E32);
        G->RestoreMismatch=0;
        for (ULONG i=0;i<0x120000/4;++i) if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i)!=backup[i])
        { ++G->RestoreMismatch; if (G->RestoreFirst==0xFFFFFFFF) { G->RestoreFirst=i*4; } }
        if (NT_SUCCESS(restored) && !NT_SUCCESS(verified)) { restored=verified; }
        if (G->RestoreMismatch==0) { G->Checks|=32; }
        else if (NT_SUCCESS(restored)) { restored=STATUS_IO_DEVICE_ERROR; }
        G->RestoreStatus=restored;
        if (NT_SUCCESS(st) && !NT_SUCCESS(restored)) { st=restored; C->Reason=13; }
    }
    if (band!=NULL) { MmUnmapIoSpace(band,0x120000); }
    if (backup!=NULL) { ExFreePoolWithTag(backup,EPI_GMU_TAG); }
    if (NT_SUCCESS(st) && G->Checks!=0x3F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<3;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
// Q-1: immutable commands and A-1 bit-pattern coordinates; no floating point in the driver.
static const ULONG GmuQ1Ib[]={
    0x70460001,0x00000031,0x70268000,0x40BB0801,0x000000BF,0x48AB0402,0x00020300,0x00000001,
    0x40A98001,0x85400100,0x48A98207,0x00000000,0x70016880,0x00000000,0x00000000,0x00000000,
    0x00000000,0x00000000,0x70348003,0x00720000,0x70016880,0x00000000,0x48B30202,0x70016B00,
    0x00000000,0x40B18002,0x70016B00,0x00000000,0x70348003,0x00520000,0x70016AC0,0x00000000,
    0x40A9E002,0x70016AC0,0x00000000,0x70348003,0x00524000,0x70016E00,0x00000000,0x48A9E402,
    0x70016E00,0x00000000,0x40A9A701,0x00000001,0x40A99E02,0x00000001,0x23C40000,0x48A9A301,
    0x00000000,0x4080F002,0x0000019C,0x007F029B,0x48840A02,0x0000019C,0x007F029B,0x4880B002,
    0x0000019C,0x007F029B,0x40A01004,0x70017100,0x00000000,0x00000180,0x00000020,0x40882304,
    0x00000087,0x00000000,0x71008A00,0x00000000,0x70388003,0x00000084,0x00000001,0x0000000C,
    0x70460004,0x0000001D,0x700170B4,0x00000000,0xCAFE001B,0x70268000,0x4080F002,0x000001A4,
    0x007F02A3,0x48840A02,0x000001A4,0x007F02A3,0x4880B002,0x000001A4,0x007F02A3,0x40A01004,
    0x70017280,0x00000000,0x00000180,0x00000020,0x40882304,0x00000087,0x00000000,0x71009AC0,
    0x00000000,0x70388003,0x00000084,0x00000001,0x0000000C,0x70460004,0x0000001D,0x700170B8,
    0x00000000,0xCAFE001C,0x70268000,
};
static_assert(RTL_NUMBER_OF(GmuQ1Ib)==107,"Q-1 IB length");
static const ULONG GmuQ1Packet[]={0x70BF8003,0x70017C40,0,107,0x70460004,4,0x700170BC,0};
struct GMU_Q1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG MChecks,MReason,CcuLive,RbCntlLive,PlaceBad,PlaceFirst;
    ULONG IbBadCount,IbFirstBad,IbBadValue,DescBad;
    ULONG FramesDone,FailFrame,FailData47,FailRptr,FailWptr,MaxRptrSeen,WrapCount,StraddleFrame,StraddleRptr,WptrWrites;
    ULONG FenceMaxUs,FenceTotalUs,ElapsedMs;
    ULONG RingBad,RingFirst,RingValue,RingExpected,OutsideRingBad,OutsideRingFirst,OutsideRingOld,OutsideRingNew;
    ULONG SqGood[2],SqBad,SqFirst,SqValue,SqExpected,TexelBad[2],SqZero,SqPad,SqBg;
    ULONG BgGood[2],BgBad,BgFirst,BgValue,OutsideChanged,OutsideFirst,OutsideOld,OutsideNew,Sample[20];
    ULONG DataAfter[3],CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAfter,ScanCount,ScanFirst,ScanDwords;
    ULONG RestoreMismatch,RestoreFirst,HoldMs,Checks,Prepared;
    NTSTATUS BackupStatus,HoldStatus,RestoreStatus;
};
static_assert(sizeof(GMU_Q1_TRACE)==552,"Q-1 diagnostic ABI");
static VOID GmuQ1TraceInit(GMU_Q1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp);
    t->FramesDone=0; t->MaxRptrSeen=0; t->WrapCount=0; t->WptrWrites=0;
    t->FenceMaxUs=0; t->FenceTotalUs=0; t->HoldMs=0; t->Checks=0; t->Prepared=0;
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
    t->BackupStatus=t->HoldStatus=t->RestoreStatus=STATUS_NOT_FOUND;
}
static BOOLEAN GmuQ1CanFollow(const GMU_M1_TRACE* M)
{
    return M!=NULL && NT_SUCCESS(M->Cp.Status) && M->Cp.Reason==0 && M->Checks==0x3F &&
        M->Cp.Rptr==3272 && M->Cp.WptrAfter==3272 && M->Cp.ShadowAfter==3272 && M->Data42==0xCAFE001A &&
        M->CcuAfter==0x10000000 && M->RestoreMismatch==0 && NT_SUCCESS(M->RestoreStatus);
}
static ULONG GmuQ1VertexWord(ULONG Angle,BOOLEAN PassB,ULONG Index)
{
    static const ULONG order[]={0,1,2,0,2,3};
    const ULONG vertex=Index/8,component=Index%8,corner=order[vertex%6];
    if (component<2) { return GmuA1VertexWord(Angle,PassB,Index); }
    if (component==2 || component==6) { return 0; }
    if (component==3 || component==7) { return 0x3F800000; }
    if (vertex<6) { return component==4 ? 0x41080000 : 0x3F000000; }
    const BOOLEAN eight=component==4 ? (corner==1 || corner==2) : (corner>=2);
    return eight ? 0x41000000 : 0;
}
static ULONG GmuQ1Vertices(volatile ULONG* Words,ULONG Angle,BOOLEAN Initial)
{
    for (ULONG pass=0;pass<2;++pass)
    {
        volatile ULONG* vb=Words+(pass ? 0x7280 : 0x7100)/4;
        for (ULONG i=Initial ? 0 : 48;i<96;++i)
            if (Initial || i%8<2) { vb[i]=GmuQ1VertexWord(Angle,pass!=0,i); }
    }
    KeMemoryBarrier(); ULONG bad=0;
    for (ULONG pass=0;pass<2;++pass) for (ULONG i=0;i<96;++i)
        if (Words[(pass ? 0x7280 : 0x7100)/4+i]!=GmuQ1VertexWord(Angle,pass!=0,i)) { ++bad; }
    return bad;
}
static ULONG GmuQ1Resource(ULONG Offset,ULONG Old)
{
    static const ULONG tc[]={0x0C006880,0x00040009,0x20002000,0,0x70017E00,0};
    if (Offset>=0x6E00 && Offset<0x6E40) { const ULONG i=(Offset-0x6E00)/4; return i<6 ? tc[i] : 0; }
    if (Offset>=0x7E00 && Offset<0x8000)
    {
        const ULONG i=(Offset-0x7E00)/4,x=i%16,y=i/16;
        return x<8 ? GmuT1Texel(x,y) : (x==8 ? 0xFF202020UL : 0xFFFF00FFUL);
    }
    if (Offset>=0x7C40 && Offset<0x7DEC) { return GmuQ1Ib[(Offset-0x7C40)/4]; }
    if (Offset>=0x7100 && Offset<0x7400)
    { return GmuQ1VertexWord(0,Offset>=0x7280,((Offset-0x7100)/4)%96); }
    if (Offset==0x70B4) { return 0xCAFE001B; }
    if (Offset==0x70B8) { return 0xCAFE001C; }
    if (Offset==0x70BC) { return 0xA2000078; }
    return Old;
}
static ULONG GmuQ1Word(ULONG Frame,ULONG Word)
{
    return Word<8 ? GmuQ1Packet[Word] : (0xA2000000UL|Frame);
}
static ULONG GmuQ1Expected(ULONG Position)
{
    const ULONG absolute=Position<265 ? Position+4096 : Position;
    if (absolute>=3272 && absolute<4361) { return GmuQ1Word((absolute-3272)/9,(absolute-3272)%9); }
    return Position>=3095 && Position<3272 ? GmuM1Packet[Position-3095] : GmuW1Expected(Position);
}
static BOOLEAN GmuQ1Pointer(GMU_Q1_TRACE* G,ULONG Value)
{
    if (Value>G->MaxRptrSeen) { G->MaxRptrSeen=Value; }
    if (Value>=4096) { G->Cp.Reason=18; return FALSE; }
    return TRUE;
}
static BOOLEAN GmuQ1Pointers(GMU_Q1_TRACE* G)
{
    const BOOLEAN native=GmuQ1Pointer(G,G->Cp.Rptr),shadow=GmuQ1Pointer(G,G->Cp.ShadowAfter);
    return native && shadow;
}
static NTSTATUS GmuQ1Wait(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_Q1_TRACE* G,
                         volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow,volatile ULONG* Data,
                         ULONG Frame,ULONGLONG Begin)
{
    GMU_G2_TRACE* C=&G->Cp;
    const ULONG target=(3272+9*(Frame+1))%4096;
    // Check RPTR before every AHB retry too; do not hide a size mismatch inside the 2-ms retry loop.
    for (ULONG retry=0;;++retry)
    {
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
        if (G->MaskAfter) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
        KeMemoryBarrier(); C->ShadowAfter=*Shadow; C->Rptr=READ_REGISTER_ULONG(Cp+6);
        if (!GmuQ1Pointers(G)) { return STATUS_IO_DEVICE_ERROR; }
        C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201);
        if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
        if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
        if (retry) { WRITE_REGISTER_ULONG(Cp+7,target); KeMemoryBarrier(); ++C->WptrWrites; }
        C->FenceStatus=READ_REGISTER_ULONG(GmuReg(S,0x23B13));
        if (!(C->FenceStatus&1)) { break; }
        if (retry==200 || KeQueryInterruptTime()-Begin>=20000) { C->Reason=10; return STATUS_IO_TIMEOUT; }
        KeStallExecutionProcessor(10);
    }
    for (ULONG poll=0;;++poll)
    {
        C->Polls=poll;
        if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
        G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
        if (G->MaskAfter) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
        KeMemoryBarrier(); C->ShadowAfter=*Shadow; C->Rptr=READ_REGISTER_ULONG(Cp+6);
        if (!GmuQ1Pointers(G)) { return STATUS_IO_DEVICE_ERROR; }
        C->WptrAfter=READ_REGISTER_ULONG(Cp+7); C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201);
        C->RbbmStatus=READ_REGISTER_ULONG(Gpu+0x210); C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        G->IbRemaining=READ_REGISTER_ULONG(Cp+0x12A);
        if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
        if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
        if (KeQueryInterruptTime()-Begin>=1000000) { C->Reason=16; return STATUS_IO_TIMEOUT; }
        const BOOLEAN idle=Frame!=120 || (!(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23)) && G->IbRemaining==0);
        if (Data[47]==(0xA2000000UL|Frame) && C->Rptr==target && C->WptrAfter==target && C->ShadowAfter==target && idle)
        { return STATUS_SUCCESS; }
        KeStallExecutionProcessor(100);
    }
}
static NTSTATUS GmuQ1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_Q1_TRACE* G,HANDLE Diag,const GMU_D1_TRACE* D)
{
    GMU_G2_TRACE* C=&G->Cp;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x1F00); if (!NT_SUCCESS(st)) { return st; }
    if (G->MChecks!=0x3F || G->MReason!=0) { C->Reason=8; return STATUS_INVALID_DEVICE_STATE; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    PVOID band=NULL; ULONG* backup=NULL;
    volatile ULONG* data=words+0x7000/4;
    PVOID maps[3]={}; const ULONG offsets[]={0,0x2000,0x23000};
    for (ULONG i=0;i<3;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        volatile ULONG* ccu=static_cast<volatile ULONG*>(maps[2])+0x207;
        st=GmuG2Guard(S,Params,Cc,T,C,0x1F01);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                C->ShadowAfter=C->ShadowBefore; C->WptrAfter=C->WptrBefore;
                if (!GmuQ1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; }
                if (NT_SUCCESS(st)) { G->RbCntlLive=READ_REGISTER_ULONG(cp+2); }
                BOOLEAN previous=C->ShadowBefore==3272 && C->WptrBefore==3272 && C->Rptr==3272;
                if (NT_SUCCESS(st)) { G->CcuLive=READ_REGISTER_ULONG(ccu); }
                if (G->RbCntlLive!=0x20B || G->CcuLive!=0x10000000 || data[42]!=0xCAFE001A) { previous=FALSE; }
                for (ULONG i=0;i<4096;++i) { if (words[i]!=(i>=3095 && i<3272 ? GmuM1Packet[i-3095] : GmuW1Expected(i))) { previous=FALSE; } }
                for (ULONG i=0;i<107;++i) { if (words[0x6400/4+i]!=GmuT1Ib[i]) { previous=FALSE; } }
                for (ULONG off=0x6880;off<0x6E00;off+=4)
                    { if (words[off/4]!=GmuT1Word(off)) { previous=FALSE; } }
                if (!(D!=NULL && EpiGmuD1DisplayMatches(D->Disp))) { previous=FALSE; }
                GMU_REGION* ext=GmuFind(S,0x71000000,0x120000);
                if (ext==NULL || !ext->External || !ext->Mapped || ext->Pa.QuadPart!=0xE1462000 ||
                    ext->Size!=0x120000 || ext->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE)) { previous=FALSE; }
                G->PlaceBad=0;
                const ULONG places[][2]={{0x6E00,0x6E40},{0x70B4,0x70C0},{0x7100,0x7400},{0x7C40,0x8000}};
                for (ULONG j=0;j<RTL_NUMBER_OF(places);++j) for (ULONG off=places[j][0];off<places[j][1];off+=4)
                    if (words[off/4]!=0) { ++G->PlaceBad; if (G->PlaceFirst==0xFFFFFFFF) { G->PlaceFirst=off; } }
                if (G->PlaceBad) { previous=FALSE; }
                if (NT_SUCCESS(st) && !previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
                if (NT_SUCCESS(st)) { G->Checks|=1; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1F02);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1F08);
            if (NT_SUCCESS(st))
            {
                backup=static_cast<ULONG*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,0x124000,EPI_GMU_TAG));
                if (backup==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                else
                {
                    PHYSICAL_ADDRESS pa; pa.QuadPart=0xE1462000;
                    band=MmMapIoSpaceEx(pa,0x120000,PAGE_READWRITE|PAGE_NOCACHE);
                    if (band==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                    else
                    {
                        for (ULONG i=0;i<0x120000/4;++i) { backup[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i); }
                        for (ULONG i=0;i<0x4000/4;++i) { backup[0x120000/4+i]=words[0x4000/4+i]; }
                        KeMemoryBarrier();
                    }
                }
                G->BackupStatus=st;
                if (!NT_SUCCESS(st)) { C->Reason=GX_REG_MAP_FAILED; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x1F03);
            if (NT_SUCCESS(st))
            {
                RtlCopyMemory(static_cast<UCHAR*>(ring->Va)+0x7C40,GmuQ1Ib,sizeof(GmuQ1Ib));
                for (ULONG off=0x6E00;off<0x6E40;off+=4) { words[off/4]=GmuQ1Resource(off,0); }
                for (ULONG off=0x7E00;off<0x8000;off+=4) { words[off/4]=GmuQ1Resource(off,0); }
                for (ULONG i=45;i<=47;++i) { data[i]=0xFFFFFFFF; }
                G->DescBad=GmuQ1Vertices(words,0,TRUE); G->Prepared=1; G->IbBadCount=0;
                for (ULONG i=0;i<107;++i)
                {
                    const ULONG value=words[0x7C40/4+i];
                    if (value!=GmuQ1Ib[i])
                    { ++G->IbBadCount; if (G->IbFirstBad==0xFFFFFFFF) { G->IbFirstBad=i; G->IbBadValue=value; } }
                }
                for (ULONG off=0x6E00;off<0x6E40;off+=4) { if (words[off/4]!=GmuQ1Resource(off,0)) { ++G->DescBad; } }
                for (ULONG off=0x7E00;off<0x8000;off+=4) { if (words[off/4]!=GmuQ1Resource(off,0)) { ++G->DescBad; } }
                for (ULONG i=45;i<=47;++i) { if (data[i]!=0xFFFFFFFF) { ++G->DescBad; } }
                if (G->IbBadCount || G->DescBad) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
            }
        }
        if (NT_SUCCESS(st))
        {
            ULONGLONG firstPublish=0;
            for (ULONG frame=0;frame<121;++frame)
            {
                if ((frame%16)==0) { st=GmuG2Guard(S,Params,Cc,T,C,0x1F40+frame/16); }
                else if (!GmuG2Live(S,Cc,T,C)) { st=STATUS_INVALID_DEVICE_STATE; }
                if (NT_SUCCESS(st))
                {
                    G->MaskAfter=READ_REGISTER_ULONG(gpu+0x38);
                    if (G->MaskAfter) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
                }
                if (NT_SUCCESS(st))
                {
                    KeMemoryBarrier(); C->ShadowAfter=*rptr; C->Rptr=READ_REGISTER_ULONG(cp+6);
                    if (!GmuQ1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; }
                    if (NT_SUCCESS(st))
                    {
                        C->IntStatus=READ_REGISTER_ULONG(gpu+0x201);
                        if (C->IntStatus&(1UL<<23)) { C->Reason=11; st=STATUS_IO_DEVICE_ERROR; }
                        else if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; st=STATUS_IO_DEVICE_ERROR; }
                    }
                }
                if (NT_SUCCESS(st) && frame>0)
                {
                    // The preceding unique fence completed before changing x/v; s/t and the background stay fixed.
                    const ULONG bad=GmuQ1Vertices(words,frame%120,FALSE); G->DescBad+=bad;
                    if (bad) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                const ULONG absolute=3272+9*frame,target=(absolute+9)%4096;
                if (NT_SUCCESS(st))
                {
                    KeMemoryBarrier();
                    for (ULONG j=0;j<9;++j) { words[(absolute+j)%4096]=GmuQ1Word(frame,j); }
                    KeMemoryBarrier();
                    for (ULONG j=0;j<9;++j) if (words[(absolute+j)%4096]!=GmuQ1Word(frame,j)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                if (NT_SUCCESS(st))
                {
                    const ULONGLONG begin=KeQueryInterruptTime();
                    if (!C->Attempted) { firstPublish=begin; }
                    C->Attempted=1; WRITE_REGISTER_ULONG(cp+7,target); KeMemoryBarrier(); ++C->WptrWrites;
                    st=GmuQ1Wait(S,Cc,T,G,cp,gpu,rptr,data,frame,begin);
                    const ULONGLONG end=KeQueryInterruptTime();
                    const ULONG us=static_cast<ULONG>((end-begin+9)/10);
                    if (us>G->FenceMaxUs) { G->FenceMaxUs=us; } G->FenceTotalUs+=us;
                    G->ElapsedMs=static_cast<ULONG>((end-firstPublish)/10000);
                    if (NT_SUCCESS(st))
                    {
                        ++G->FramesDone; G->WrapCount+=(absolute+9)/4096-absolute/4096;
                        if (absolute%4096+9>4096) { G->StraddleFrame=frame; G->StraddleRptr=C->Rptr; }
                    }
                }
                if (NT_SUCCESS(st)) { st=GmuA1Pause(16); if (!NT_SUCCESS(st)) { C->Reason=17; } }
                if (!NT_SUCCESS(st)) { G->FailFrame=frame; G->FailData47=data[47]; G->FailRptr=C->Rptr; G->FailWptr=C->WptrAfter; break; }
            }
            G->WptrWrites=C->WptrWrites;
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            if (C->Reason==18) { break; }
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x1F10+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
            if (i==0)
            {
                KeMemoryBarrier(); C->ShadowAfter=*rptr;
                if (!GmuQ1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; break; }
            }
        }
        if (!NT_SUCCESS(st) && C->Reason!=18) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x1F20);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        if (!GmuQ1Pointer(G,C->ShadowAfter)) { st=STATUS_IO_DEVICE_ERROR; }
        for (ULONG i=0;i<3;++i) { G->DataAfter[i]=data[45+i]; }
        if (G->Prepared)
        {
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (G->FramesDone==121 && G->WrapCount==1 && G->StraddleFrame==91 && G->StraddleRptr==4 &&
                G->DataAfter[2]==0xA2000078 && G->IbRemaining==0 && C->ShadowAfter==265 && C->Rptr==265 && C->WptrAfter==265 &&
                !(C->IntStatus&errors) && !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }
        }
        if (band!=NULL && NT_SUCCESS(G->BackupStatus))
        {
            G->RingBad=0; G->OutsideRingBad=0;
            for (ULONG i=0;i<4096;++i)
            {
                const ULONG expected=GmuQ1Expected(i),value=words[i];
                if (value!=expected)
                { ++G->RingBad; if (G->RingFirst==0xFFFFFFFF) { G->RingFirst=i; G->RingValue=value; G->RingExpected=expected; } }
            }
            for (ULONG off=0x4000;off<0x8000;off+=4)
            {
                const ULONG old=GmuQ1Resource(off,backup[0x120000/4+(off-0x4000)/4]),value=words[off/4];
                if (old!=value)
                { ++G->OutsideRingBad; if (G->OutsideRingFirst==0xFFFFFFFF) { G->OutsideRingFirst=off; G->OutsideRingOld=old; G->OutsideRingNew=value; } }
            }
            if (G->RingBad==0 && G->OutsideRingBad==0 && G->DataAfter[0]==0xCAFE001B && G->DataAfter[1]==0xCAFE001C) { G->Checks|=4; }
            G->SqGood[0]=0; G->SqGood[1]=0; G->SqBad=0; G->SqZero=0; G->SqPad=0; G->SqBg=0;
            G->TexelBad[0]=0; G->TexelBad[1]=0; G->BgGood[0]=0; G->BgGood[1]=0; G->BgBad=0; G->OutsideChanged=0;
            for (ULONG i=0;i<0x120000/4;++i)
            {
                const ULONG offset=i*4,fb=0x462000+offset,y=fb/4320,x=(fb%4320)/4;
                const ULONG value=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i);
                if (x>=412 && x<668 && y>=1072 && y<1328)
                {
                    const ULONG v=(y-1072)/2;
                    if (x>=460 && x<620 && v>=24 && v<104)
                    {
                        const ULONG ti=(x-460)/20,tj=(v-24)/10,texel=tj*8+ti,expected=GmuT1Texel(ti,tj);
                        if (value==0 || value==0xFF000000) { ++G->SqZero; }
                        if (value==0xFF202020) { ++G->SqBg; }
                        if (value==0xFFFF00FF) { ++G->SqPad; }
                        if (value==expected) { if (y&1) { ++G->SqGood[1]; } else { ++G->SqGood[0]; } }
                        else
                        {
                            ++G->SqBad; G->TexelBad[texel/32]|=1UL<<(texel%32);
                            if (G->SqFirst==0xFFFFFFFF) { G->SqFirst=offset; G->SqValue=value; G->SqExpected=expected; }
                        }
                    }
                    else if (value==0xFF202020) { if (y&1) { ++G->BgGood[1]; } else { ++G->BgGood[0]; } }
                    else { ++G->BgBad; if (G->BgFirst==0xFFFFFFFF) { G->BgFirst=offset; G->BgValue=value; } }
                }
                else if (value!=backup[i])
                {
                    ++G->OutsideChanged;
                    if (G->OutsideFirst==0xFFFFFFFF) { G->OutsideFirst=offset; G->OutsideOld=backup[i]; G->OutsideNew=value; }
                }
            }
            const ULONG sample[][2]={{470,29},{610,29},{610,99},{470,99},{530,59},{550,74},{479,33},{480,34},{414,2},{665,125}};
            for (ULONG i=0;i<10;++i) for (ULONG parity=0;parity<2;++parity)
            { G->Sample[i*2+parity]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+((1072+sample[i][1]*2+parity)*4320+sample[i][0]*4-0x462000)/4); }
            if (G->SqGood[0]==12800 && G->SqGood[1]==12800) { G->Checks|=8; }
            if (G->BgGood[0]==19968 && G->BgGood[1]==19968 && G->OutsideChanged==0) { G->Checks|=16; }
        }
    }
    KeMemoryBarrier(); G->ScanCount=0; G->ScanDwords=0;
    for (ULONG off=0;off<0x8000;off+=4)
    {
        if ((off>=0x6E00 && off<0x6E40) || (off>=0x70B4 && off<0x70C0) || (off>=0x7100 && off<0x7400) || (off>=0x7C40 && off<0x8000) || (off>=0x5B80 && off<0x5EC0) || (off>=0x7078 && off<0x70B4) || (off>=0x7840 && off<0x7B40) || off<0x4000 || (off>=0x5600 && off<0x5B80) || (off>=0x6400 && off<0x6800) || (off>=0x6880 && off<0x6E00) || (off>=0x5100 && off<0x52CC) || (off>=0x5400 && off<0x5560) || (off>=0x4500 && off<0x4620) || (off>=0x4A00 && off<0x4AC0) || (off>=0x4000 && off<0x44E8) || (off>=0x4800 && off<0x4920) ||
            (off>=0x4C00 && off<0x5040) || (off>=0x6000 && off<0x6240) || (off>=0x6300 && off<0x6400) ||
            (off>=0x6800 && off<0x6880) || (off>=0x7000 && off<0x7078) ||
            (off>=0x7400 && off<0x7840) || (off>=0x7C00 && off<0x7C40)) { continue; }
        ++G->ScanDwords;
        const ULONG value=words[off/4];
        if ((value&0xFF000000UL)==0xFF000000UL && value!=0xFFFFFFFF) { ++G->ScanCount; if (G->ScanFirst==0xFFFFFFFF) { G->ScanFirst=off; } }
    }
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (band!=NULL && NT_SUCCESS(G->BackupStatus))
    {
        if (C->Attempted && NT_SUCCESS(st))
        {
            C->Status=st;
            NTSTATUS held=GmuStep(Params,0x1F30);
            if (NT_SUCCESS(held)) { held=EpiSetValue(Diag,L"GpuGmuQ1",REG_BINARY,G,sizeof(*G)); }
            if (NT_SUCCESS(held)) { held=ZwFlushKey(Diag); }
            if (NT_SUCCESS(held))
            {
                held=GmuA1Pause(2000);
                if (NT_SUCCESS(held)) { G->HoldMs=2000; }
            }
            G->HoldStatus=held;
            if (NT_SUCCESS(st) && !NT_SUCCESS(held)) { st=held; C->Reason=6; }
        }
        // Restoring display RAM is mandatory even when a breadcrumb or GX prerequisite failed.
        NTSTATUS restored=GmuStep(Params,0x1F31);
        for (ULONG i=0;i<0x120000/4;++i) { WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i,backup[i]); }
        KeMemoryBarrier();
        const NTSTATUS verified=GmuStep(Params,0x1F32);
        G->RestoreMismatch=0;
        for (ULONG i=0;i<0x120000/4;++i) if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i)!=backup[i])
        { ++G->RestoreMismatch; if (G->RestoreFirst==0xFFFFFFFF) { G->RestoreFirst=i*4; } }
        if (NT_SUCCESS(restored) && !NT_SUCCESS(verified)) { restored=verified; }
        if (G->RestoreMismatch==0) { G->Checks|=32; }
        else if (NT_SUCCESS(restored)) { restored=STATUS_IO_DEVICE_ERROR; }
        G->RestoreStatus=restored;
        if (NT_SUCCESS(st) && !NT_SUCCESS(restored)) { st=restored; C->Reason=13; }
    }
    if (band!=NULL) { MmUnmapIoSpace(band,0x120000); }
    if (backup!=NULL) { ExFreePoolWithTag(backup,EPI_GMU_TAG); }
    if (NT_SUCCESS(st) && G->Checks!=0x3F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<3;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
// H-1 suballocates only the new, boot-mapped region; allocator metadata is CPU-owned pooled state.
#include "epi_gpuheap.hxx"
struct GMU_H1_WORK { EPI_GPU_HEAP Heap; ULONG Completed,Submitted,B1Base[4]; };
struct GMU_H1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG QChecks,QReason,CcuLive,RbCntlLive;
    ULONG RegionCount,HeapIova,HeapSize,HeapFlags,HeapPaLow,HeapPaHigh,HeapMapped,HeapZeroBad,HeapZeroFirst;
    ULONG LiveBytes,PeakBytes,HighWater,AllocCount,FreeCount,FailCount,ReuseCount,RetirePending,StaticOffset[6];
    ULONG IbOffsetFirst,IbOffsetLast,VbOffsetFirst,VbOffsetLast,DistinctIb;
    ULONG FramesDone,FailFrame,FailFence,FailRptr,FailWptr,MaxRptrSeen,PendingAtPublish,MaxInFlight,FenceBackwards,WptrWrites;
    ULONG WaitMaxUs,WaitTotalUs,ElapsedMs;
    ULONG TsAfter[3],StaticBad,StaticFirst,TailBad,TailFirst,RingBad,RingFirst,RingValue,RingExpected,OldTailBad,OldTailFirst;
    ULONG SqGood[2],SqBad,SqFirst,SqValue,SqExpected,TexelBad[2],SqZero,SqPad,SqBg;
    ULONG BgGood[2],BgBad,BgFirst,BgValue,OutsideChanged,OutsideFirst,OutsideOld,OutsideNew,Sample[20];
    ULONG CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAfter;
    ULONG RestoreMismatch,RestoreFirst,HoldMs,Checks,Prepared;
    NTSTATUS BackupStatus,HoldStatus,RestoreStatus;
    ULONG IbOffset[61],VbOffset[61]; // publication history also makes every final ring address decodable
};
static_assert(sizeof(GMU_H1_TRACE)==1124,"H-1 diagnostic ABI");
static const ULONG GmuH1Size[]={12,128,16,128,64,512},GmuH1Align[]={64,128,64,128,64,256};
static const ULONG GmuH1Offsets[]={0,0x80,0x40,0x100,0x180,0x200};
static VOID GmuH1TraceInit(GMU_H1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp);
    t->FramesDone=t->MaxRptrSeen=t->PendingAtPublish=t->MaxInFlight=t->FenceBackwards=t->WptrWrites=0;
    t->WaitMaxUs=t->WaitTotalUs=t->HoldMs=t->Checks=t->Prepared=0;
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
    t->BackupStatus=t->HoldStatus=t->RestoreStatus=STATUS_NOT_FOUND;
}
static BOOLEAN GmuH1Plan(GMU_TEST* S)
{
    if (S->Count>=EPI_GMU_MAX_REGIONS) { return FALSE; }
    for (ULONG i=0;i<S->Count;++i)
        if (0x70100000ULL<static_cast<ULONGLONG>(S->Regions[i].Iova)+S->Regions[i].Size && S->Regions[i].Iova<0x70140000ULL) { return FALSE; }
    return GmuAdd(S,0x70100000,0x40000,EPI_SMMU_READ|EPI_SMMU_WRITE);
}
static BOOLEAN GmuH1CanFollow(const GMU_Q1_TRACE* Q)
{
    return Q!=NULL && NT_SUCCESS(Q->Cp.Status) && Q->Cp.Reason==0 && Q->Checks==0x3F &&
        Q->Cp.Rptr==265 && Q->Cp.WptrAfter==265 && Q->Cp.ShadowAfter==265 && Q->DataAfter[2]==0xA2000078 &&
        Q->RestoreMismatch==0 && NT_SUCCESS(Q->RestoreStatus);
}
static ULONG GmuH1StaticWord(ULONG Object,ULONG Word)
{
    static const ULONG samp[]={0x00060920,0x00000060,0,0};
    static const ULONG tc[]={0x0C006880,0x00040009,0x20002000,0,0x70100200,0};
    if (Object==0) { return Word<3 ? 0xFFFFFFFFUL : 0; }
    if (Object==1) { return Word==1 ? 0x13000000UL : 0; }
    if (Object==2) { return Word<4 ? samp[Word] : 0; }
    if (Object==3) { return 0; }
    if (Object==4) { return Word<6 ? tc[Word] : 0; }
    return GmuQ1Resource(0x7E00+Word*4,0);
}
static ULONG GmuH1IbWord(ULONG Word,ULONG Vb)
{
    switch (Word)
    {
    case 12: case 20: return 0x70100080;
    case 23: case 26: return 0x70100100;
    case 30: case 33: return 0x70100040;
    case 37: case 40: return 0x70100180;
    case 59: return 0x70100000+Vb;
    case 88: return 0x70100000+Vb+384;
    case 74: return 0x70100000;
    case 103: return 0x70100004;
    default: return GmuQ1Ib[Word];
    }
}
static ULONG GmuH1RingWord(const GMU_H1_TRACE* G,ULONG Frame,ULONG Word)
{
    if (Word==1) { return 0x70100000+G->IbOffset[Frame]; }
    if (Word==6) { return 0x70100008; }
    if (Word==8) { return 0xA3000000UL|Frame; }
    return GmuQ1Packet[Word];
}
static VOID GmuH1Stats(GMU_H1_TRACE* G,const GMU_H1_WORK* W)
{
    G->LiveBytes=W->Heap.LiveBytes; G->PeakBytes=W->Heap.PeakBytes; G->HighWater=W->Heap.HighWater;
    G->AllocCount=W->Heap.AllocCount; G->FreeCount=W->Heap.FreeCount; G->FailCount=W->Heap.FailCount;
    G->ReuseCount=W->Heap.ReuseCount; G->RetirePending=W->Heap.RetirePending;
    G->DistinctIb=0;
    for (ULONG i=0;i<61;++i) if (G->IbOffset[i]!=0xFFFFFFFF)
    {
        BOOLEAN first=TRUE;
        for (ULONG j=0;j<i;++j) if (G->IbOffset[j]==G->IbOffset[i]) { first=FALSE; break; }
        if (first) { ++G->DistinctIb; }
        if (G->IbOffsetFirst==0xFFFFFFFF) { G->IbOffsetFirst=G->IbOffset[i]; }
        G->IbOffsetLast=G->IbOffset[i];
        if (G->VbOffset[i]!=0xFFFFFFFF)
        { if (G->VbOffsetFirst==0xFFFFFFFF) { G->VbOffsetFirst=G->VbOffset[i]; } G->VbOffsetLast=G->VbOffset[i]; }
    }
}
static BOOLEAN GmuH1Pointer(GMU_H1_TRACE* G,ULONG Value)
{
    if (Value>G->MaxRptrSeen) { G->MaxRptrSeen=Value; }
    if (Value>=4096) { G->Cp.Reason=18; return FALSE; }
    return TRUE;
}
static BOOLEAN GmuH1Pointers(GMU_H1_TRACE* G)
{
    const BOOLEAN native=GmuH1Pointer(G,G->Cp.Rptr),shadow=GmuH1Pointer(G,G->Cp.ShadowAfter);
    return native && shadow;
}
static NTSTATUS GmuH1Observe(GMU_H1_TRACE* G,GMU_H1_WORK* W,volatile ULONG* Ts)
{
    KeMemoryBarrier(); const ULONG raw=Ts[2];
    if (raw==0xFFFFFFFF)
    {
        if (W->Completed!=0xFFFFFFFF) { ++G->FenceBackwards; G->Cp.Reason=20; return STATUS_IO_DEVICE_ERROR; }
        return STATUS_SUCCESS; // Never call Reclaim with the uninitialized sentinel.
    }
    if ((raw&0xFF000000UL)!=0xA3000000UL || (raw&0xFFFFFFUL)>=W->Submitted)
    { G->Cp.Reason=13; return STATUS_IO_DEVICE_ERROR; }
    const ULONG completed=raw&0xFFFFFFUL;
    if (W->Completed!=0xFFFFFFFF && completed<W->Completed)
    { ++G->FenceBackwards; G->Cp.Reason=20; return STATUS_IO_DEVICE_ERROR; }
    W->Completed=completed; G->FramesDone=completed+1;
    if (!EpiGpuHeapReclaim(&W->Heap,completed)) { G->Cp.Reason=19; return STATUS_IO_DEVICE_ERROR; }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuH1Live(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_H1_TRACE* G,
                          volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow)
{
    GMU_G2_TRACE* C=&G->Cp;
    if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
    G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
    if (G->MaskAfter) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
    KeMemoryBarrier(); C->ShadowAfter=*Shadow; C->Rptr=READ_REGISTER_ULONG(Cp+6);
    if (!GmuH1Pointers(G)) { return STATUS_IO_DEVICE_ERROR; }
    C->WptrAfter=READ_REGISTER_ULONG(Cp+7); C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201);
    if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
    if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuH1Ahb(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_H1_TRACE* G,
                         volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow,ULONG Target,ULONGLONG Begin)
{
    for (ULONG retry=0;;++retry)
    {
        NTSTATUS st=GmuH1Live(S,Cc,T,G,Cp,Gpu,Shadow); if (!NT_SUCCESS(st)) { return st; }
        if (retry) { WRITE_REGISTER_ULONG(Cp+7,Target); KeMemoryBarrier(); ++G->Cp.WptrWrites; }
        G->Cp.FenceStatus=READ_REGISTER_ULONG(GmuReg(S,0x23B13));
        if (!(G->Cp.FenceStatus&1)) { return STATUS_SUCCESS; }
        if (retry==200 || KeQueryInterruptTime()-Begin>=20000) { G->Cp.Reason=10; return STATUS_IO_TIMEOUT; }
        KeStallExecutionProcessor(10);
    }
}
static NTSTATUS GmuH1Wait(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_H1_TRACE* G,GMU_H1_WORK* W,
                          volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow,volatile ULONG* Ts,
                          ULONG Target,BOOLEAN Final)
{
    const ULONGLONG begin=KeQueryInterruptTime(); NTSTATUS st=STATUS_SUCCESS;
    for (ULONG poll=0;;++poll)
    {
        G->Cp.Polls=poll;
        st=GmuH1Live(S,Cc,T,G,Cp,Gpu,Shadow); if (!NT_SUCCESS(st)) { break; }
        st=GmuH1Observe(G,W,Ts); if (!NT_SUCCESS(st)) { break; }
        if (KeQueryInterruptTime()-begin>=1000000) { G->Cp.Reason=16; st=STATUS_IO_TIMEOUT; break; }
        if (W->Completed!=0xFFFFFFFF && W->Completed>=Target)
        {
            if (!Final) { break; }
            G->Cp.RbbmStatus=READ_REGISTER_ULONG(Gpu+0x210); G->Cp.CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
            G->IbRemaining=READ_REGISTER_ULONG(Cp+0x12A);
            if (W->Completed==60 && G->Cp.Rptr==814 && G->Cp.WptrAfter==814 && G->Cp.ShadowAfter==814 &&
                !(G->Cp.RbbmStatus&~1UL) && !(G->Cp.CxBusy&(1UL<<23)) && G->IbRemaining==0) { break; }
        }
        KeStallExecutionProcessor(100);
    }
    const ULONG us=static_cast<ULONG>((KeQueryInterruptTime()-begin+9)/10);
    if (us>G->WaitMaxUs) { G->WaitMaxUs=us; } G->WaitTotalUs+=us;
    return st;
}
static NTSTATUS GmuH1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_H1_TRACE* G,GMU_H1_WORK* W,HANDLE Diag,const GMU_D1_TRACE* D)
{
    GMU_G2_TRACE* C=&G->Cp;
    RtlZeroMemory(W,sizeof(*W)); W->Completed=0xFFFFFFFF;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x2000); if (!NT_SUCCESS(st)) { return st; }
    if (G->QChecks!=0x3F || G->QReason!=0) { C->Reason=8; return STATUS_INVALID_DEVICE_STATE; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* heap=GmuFind(S,0x70100000,0x40000);
    G->RegionCount=S->Count;
    if (heap!=NULL)
    {
        G->HeapIova=heap->Iova; G->HeapSize=heap->Size; G->HeapFlags=heap->Flags; G->HeapMapped=heap->Mapped;
        G->HeapPaLow=static_cast<ULONG>(heap->Pa.QuadPart); G->HeapPaHigh=static_cast<ULONG>(heap->Pa.QuadPart>>32);
    }
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (heap==NULL || heap->External || heap->Va==NULL || !heap->Mapped || heap->Size!=0x40000 ||
        heap->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE) || S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    PVOID band=NULL; ULONG* backup=NULL;
    volatile ULONG* hw=static_cast<volatile ULONG*>(heap->Va);
    volatile ULONG* ts=hw;
    PVOID maps[3]={}; const ULONG offsets[]={0,0x2000,0x23000};
    for (ULONG i=0;i<3;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        volatile ULONG* ccu=static_cast<volatile ULONG*>(maps[2])+0x207;
        st=GmuG2Guard(S,Params,Cc,T,C,0x2001);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                C->ShadowAfter=C->ShadowBefore; C->WptrAfter=C->WptrBefore;
                if (!GmuH1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; }
                if (NT_SUCCESS(st)) { G->RbCntlLive=READ_REGISTER_ULONG(cp+2); }
                BOOLEAN previous=C->ShadowBefore==265 && C->WptrBefore==265 && C->Rptr==265;
                if (NT_SUCCESS(st)) { G->CcuLive=READ_REGISTER_ULONG(ccu); }
                if (G->RbCntlLive!=0x20B || G->CcuLive!=0x10000000) { previous=FALSE; }
                for (ULONG i=0;i<4096;++i) { if (words[i]!=GmuQ1Expected(i)) { previous=FALSE; } }
                if (!(D!=NULL && EpiGmuD1DisplayMatches(D->Disp))) { previous=FALSE; }
                GMU_REGION* ext=GmuFind(S,0x71000000,0x120000);
                if (ext==NULL || !ext->External || !ext->Mapped || ext->Pa.QuadPart!=0xE1462000 ||
                    ext->Size!=0x120000 || ext->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE)) { previous=FALSE; }
                G->HeapZeroBad=0;
                for (ULONG i=0;i<0x40000/4;++i) if (hw[i]!=0)
                    { ++G->HeapZeroBad; if (G->HeapZeroFirst==0xFFFFFFFF) { G->HeapZeroFirst=i*4; } }
                if (G->HeapZeroBad) { previous=FALSE; }
                if (NT_SUCCESS(st) && !previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
                if (NT_SUCCESS(st)) { G->Checks|=1; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x2002);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x2008);
            if (NT_SUCCESS(st))
            {
                backup=static_cast<ULONG*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,0x124000,EPI_GMU_TAG));
                if (backup==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                else
                {
                    PHYSICAL_ADDRESS pa; pa.QuadPart=0xE1462000;
                    band=MmMapIoSpaceEx(pa,0x120000,PAGE_READWRITE|PAGE_NOCACHE);
                    if (band==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                    else
                    {
                        for (ULONG i=0;i<0x120000/4;++i) { backup[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i); }
                        for (ULONG i=0;i<0x4000/4;++i) { backup[0x120000/4+i]=words[0x4000/4+i]; }
                        KeMemoryBarrier();
                    }
                }
                G->BackupStatus=st;
                if (!NT_SUCCESS(st)) { C->Reason=GX_REG_MAP_FAILED; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x2003);
            if (NT_SUCCESS(st))
            {
                for (ULONG object=0;object<6;++object)
                {
                    G->StaticOffset[object]=EpiGpuHeapAlloc(&W->Heap,GmuH1Size[object],GmuH1Align[object]);
                    if (G->StaticOffset[object]==0xFFFFFFFF) { C->Reason=19; st=STATUS_INSUFFICIENT_RESOURCES; break; }
                    if (G->StaticOffset[object]!=GmuH1Offsets[object]) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; break; }
                }
                if (NT_SUCCESS(st))
                {
                    G->StaticBad=0;
                    for (ULONG object=0;object<6;++object) for (ULONG i=0;i<((GmuH1Size[object]+63)&~63UL)/4;++i)
                        { hw[G->StaticOffset[object]/4+i]=GmuH1StaticWord(object,i); }
                    KeMemoryBarrier(); G->Prepared=1;
                    for (ULONG object=0;object<6;++object) for (ULONG i=0;i<((GmuH1Size[object]+63)&~63UL)/4;++i)
                        if (hw[G->StaticOffset[object]/4+i]!=GmuH1StaticWord(object,i))
                        { ++G->StaticBad; if (G->StaticFirst==0xFFFFFFFF) { G->StaticFirst=G->StaticOffset[object]+i*4; } }
                    if (G->StaticBad) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
            }
        }
        ULONGLONG firstPublish=0;
        if (NT_SUCCESS(st))
        {
            for (ULONG frame=0;frame<61;++frame)
            {
                if ((frame%16)==0) { st=GmuG2Guard(S,Params,Cc,T,C,0x2040+frame/16); }
                if (NT_SUCCESS(st)) { st=GmuH1Live(S,Cc,T,G,cp,gpu,rptr); }
                if (NT_SUCCESS(st)) { st=GmuH1Observe(G,W,ts); }
                if (NT_SUCCESS(st))
                {
                    const ULONG inFlight=W->Completed==0xFFFFFFFF ? frame : frame-W->Completed;
                    if (inFlight>G->MaxInFlight) { G->MaxInFlight=inFlight; }
                    if (inFlight>2) { C->Reason=13; st=STATUS_INVALID_DEVICE_STATE; }
                }
                if (NT_SUCCESS(st))
                {
                    G->IbOffset[frame]=EpiGpuHeapAlloc(&W->Heap,428,64);
                    if (G->IbOffset[frame]!=0xFFFFFFFF) { G->VbOffset[frame]=EpiGpuHeapAlloc(&W->Heap,768,64); }
                    if (G->IbOffset[frame]==0xFFFFFFFF || G->VbOffset[frame]==0xFFFFFFFF)
                    { C->Reason=19; st=STATUS_INSUFFICIENT_RESOURCES; }
                    else if (frame==0 && (G->IbOffset[0]!=0x400 || G->VbOffset[0]!=0x5C0)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                if (NT_SUCCESS(st))
                {
                    volatile ULONG* vb=hw+G->VbOffset[frame]/4;
                    volatile ULONG* ib=hw+G->IbOffset[frame]/4;
                    // Every reused VB is fully initialized, not just its position fields.
                    for (ULONG pass=0;pass<2;++pass) for (ULONG i=0;i<96;++i)
                        { vb[pass*96+i]=GmuQ1VertexWord((2*frame)%120,pass!=0,i); }
                    for (ULONG i=0;i<107;++i) { ib[i]=GmuH1IbWord(i,G->VbOffset[frame]); }
                    KeMemoryBarrier();
                    for (ULONG pass=0;pass<2;++pass) for (ULONG i=0;i<96;++i)
                        if (vb[pass*96+i]!=GmuQ1VertexWord((2*frame)%120,pass!=0,i)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                    for (ULONG i=0;i<107;++i) if (ib[i]!=GmuH1IbWord(i,G->VbOffset[frame])) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                if (NT_SUCCESS(st))
                {
                    for (ULONG i=0;i<9;++i) { words[265+9*frame+i]=GmuH1RingWord(G,frame,i); }
                    KeMemoryBarrier();
                    for (ULONG i=0;i<9;++i) if (words[265+9*frame+i]!=GmuH1RingWord(G,frame,i)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                if (NT_SUCCESS(st))
                {
                    const ULONGLONG begin=KeQueryInterruptTime();
                    if (!C->Attempted) { firstPublish=begin; }
                    W->Submitted=frame+1; C->Attempted=1;
                    WRITE_REGISTER_ULONG(cp+7,265+9*(frame+1)); KeMemoryBarrier(); ++C->WptrWrites;
                    // Even a rejected/timeout fenced write may have reached CP: never free these speculatively.
                    if (!EpiGpuHeapRetire(&W->Heap,G->IbOffset[frame],428,frame) ||
                        !EpiGpuHeapRetire(&W->Heap,G->VbOffset[frame],768,frame)) { C->Reason=19; st=STATUS_IO_DEVICE_ERROR; }
                    if (NT_SUCCESS(st)) { st=GmuH1Ahb(S,Cc,T,G,cp,gpu,rptr,265+9*(frame+1),begin); }
                    if (NT_SUCCESS(st)) { st=GmuH1Observe(G,W,ts); }
                    if (NT_SUCCESS(st) && (W->Completed==0xFFFFFFFF || W->Completed<frame)) { ++G->PendingAtPublish; }
                    if (NT_SUCCESS(st) && frame>=1) { st=GmuH1Wait(S,Cc,T,G,W,cp,gpu,rptr,ts,frame-1,FALSE); }
                }
                if (NT_SUCCESS(st)) { st=GmuA1Pause(16); if (!NT_SUCCESS(st)) { C->Reason=17; } }
                if (!NT_SUCCESS(st)) { G->FailFrame=frame; G->FailFence=ts[2]; G->FailRptr=C->Rptr; G->FailWptr=C->WptrAfter; break; }
            }
            if (NT_SUCCESS(st))
            {
                st=GmuG2Guard(S,Params,Cc,T,C,0x2004);
                if (NT_SUCCESS(st)) { st=GmuH1Wait(S,Cc,T,G,W,cp,gpu,rptr,ts,60,TRUE); }
                if (!NT_SUCCESS(st)) { G->FailFrame=60; G->FailFence=ts[2]; G->FailRptr=C->Rptr; G->FailWptr=C->WptrAfter; }
            }
            G->WptrWrites=C->WptrWrites;
            if (C->Attempted) { G->ElapsedMs=static_cast<ULONG>((KeQueryInterruptTime()-firstPublish)/10000); }
        }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            if (C->Reason==18) { break; }
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x2010+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
            if (i==0)
            {
                KeMemoryBarrier(); C->ShadowAfter=*rptr;
                if (!GmuH1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; break; }
            }
        }
        if (!NT_SUCCESS(st) && C->Reason!=18) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x2020);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        if (!GmuH1Pointer(G,C->ShadowAfter)) { st=STATUS_IO_DEVICE_ERROR; }
        for (ULONG i=0;i<3;++i) { G->TsAfter[i]=ts[i]; }
        GmuH1Stats(G,W);
        if (G->Prepared)
        {
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (G->FramesDone==61 && G->MaxInFlight<=2 && G->FenceBackwards==0 && G->TsAfter[2]==0xA300003C &&
                G->IbRemaining==0 && C->ShadowAfter==814 && C->Rptr==814 && C->WptrAfter==814 && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }
            if (G->FailCount==0 && G->AllocCount==128 && G->FreeCount==122 && G->LiveBytes==960 &&
                G->PeakBytes<=5120 && G->ReuseCount>=1 && G->RetirePending==0) { G->Checks|=4; }
            G->StaticBad=0; G->TailBad=0;
            for (ULONG object=1;object<6;++object) for (ULONG i=0;i<((GmuH1Size[object]+63)&~63UL)/4;++i)
                if (hw[G->StaticOffset[object]/4+i]!=GmuH1StaticWord(object,i))
                { ++G->StaticBad; if (G->StaticFirst==0xFFFFFFFF) { G->StaticFirst=G->StaticOffset[object]+i*4; } }
            for (ULONG off=W->Heap.HighWater;off<0x40000;off+=4) if (hw[off/4]!=0)
                { ++G->TailBad; if (G->TailFirst==0xFFFFFFFF) { G->TailFirst=off; } }
        }
        if (band!=NULL && NT_SUCCESS(G->BackupStatus))
        {
            G->RingBad=0; G->OldTailBad=0;
            for (ULONG i=0;i<4096;++i)
            {
                const ULONG frame=(i>=265 && i<814) ? (i-265)/9 : 61;
                const ULONG expected=frame<W->Submitted ? GmuH1RingWord(G,frame,(i-265)%9) : GmuQ1Expected(i),value=words[i];
                if (value!=expected)
                { ++G->RingBad; if (G->RingFirst==0xFFFFFFFF) { G->RingFirst=i; G->RingValue=value; G->RingExpected=expected; } }
            }
            for (ULONG off=0x4000;off<0x8000;off+=4)
            {
                const ULONG old=backup[0x120000/4+(off-0x4000)/4],value=words[off/4];
                if (old!=value)
                { ++G->OldTailBad; if (G->OldTailFirst==0xFFFFFFFF) { G->OldTailFirst=off; } }
            }
            if (G->Prepared && G->RingBad==0 && G->OldTailBad==0 && G->StaticBad==0 && G->TailBad==0 && G->TsAfter[0]==0xCAFE001B && G->TsAfter[1]==0xCAFE001C) { G->Checks|=8; }
            G->SqGood[0]=0; G->SqGood[1]=0; G->SqBad=0; G->SqZero=0; G->SqPad=0; G->SqBg=0;
            G->TexelBad[0]=0; G->TexelBad[1]=0; G->BgGood[0]=0; G->BgGood[1]=0; G->BgBad=0; G->OutsideChanged=0;
            for (ULONG i=0;i<0x120000/4;++i)
            {
                const ULONG offset=i*4,fb=0x462000+offset,y=fb/4320,x=(fb%4320)/4;
                const ULONG value=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i);
                if (x>=412 && x<668 && y>=1072 && y<1328)
                {
                    const ULONG v=(y-1072)/2;
                    if (x>=460 && x<620 && v>=24 && v<104)
                    {
                        const ULONG ti=(x-460)/20,tj=(v-24)/10,texel=tj*8+ti,expected=GmuT1Texel(ti,tj);
                        if (value==0 || value==0xFF000000) { ++G->SqZero; }
                        if (value==0xFF202020) { ++G->SqBg; }
                        if (value==0xFFFF00FF) { ++G->SqPad; }
                        if (value==expected) { if (y&1) { ++G->SqGood[1]; } else { ++G->SqGood[0]; } }
                        else
                        {
                            ++G->SqBad; G->TexelBad[texel/32]|=1UL<<(texel%32);
                            if (G->SqFirst==0xFFFFFFFF) { G->SqFirst=offset; G->SqValue=value; G->SqExpected=expected; }
                        }
                    }
                    else if (value==0xFF202020) { if (y&1) { ++G->BgGood[1]; } else { ++G->BgGood[0]; } }
                    else { ++G->BgBad; if (G->BgFirst==0xFFFFFFFF) { G->BgFirst=offset; G->BgValue=value; } }
                }
                else if (value!=backup[i])
                {
                    ++G->OutsideChanged;
                    if (G->OutsideFirst==0xFFFFFFFF) { G->OutsideFirst=offset; G->OutsideOld=backup[i]; G->OutsideNew=value; }
                }
            }
            const ULONG sample[][2]={{470,29},{610,29},{610,99},{470,99},{530,59},{550,74},{479,33},{480,34},{414,2},{665,125}};
            for (ULONG i=0;i<10;++i) for (ULONG parity=0;parity<2;++parity)
            { G->Sample[i*2+parity]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+((1072+sample[i][1]*2+parity)*4320+sample[i][0]*4-0x462000)/4); }
            if (G->SqGood[0]==12800 && G->SqGood[1]==12800 && G->BgGood[0]==19968 && G->BgGood[1]==19968 && G->OutsideChanged==0) { G->Checks|=16; }
        }
    }
    GmuH1Stats(G,W);
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (band!=NULL && NT_SUCCESS(G->BackupStatus))
    {
        if (C->Attempted && NT_SUCCESS(st))
        {
            C->Status=st;
            NTSTATUS held=GmuStep(Params,0x2030);
            if (NT_SUCCESS(held)) { held=EpiSetValue(Diag,L"GpuGmuH1",REG_BINARY,G,sizeof(*G)); }
            if (NT_SUCCESS(held)) { held=ZwFlushKey(Diag); }
            if (NT_SUCCESS(held))
            {
                held=GmuA1Pause(2000);
                if (NT_SUCCESS(held)) { G->HoldMs=2000; }
            }
            G->HoldStatus=held;
            if (NT_SUCCESS(st) && !NT_SUCCESS(held)) { st=held; C->Reason=6; }
        }
        // Restoring display RAM is mandatory even when a breadcrumb or GX prerequisite failed.
        NTSTATUS restored=GmuStep(Params,0x2031);
        for (ULONG i=0;i<0x120000/4;++i) { WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i,backup[i]); }
        KeMemoryBarrier();
        const NTSTATUS verified=GmuStep(Params,0x2032);
        G->RestoreMismatch=0;
        for (ULONG i=0;i<0x120000/4;++i) if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i)!=backup[i])
        { ++G->RestoreMismatch; if (G->RestoreFirst==0xFFFFFFFF) { G->RestoreFirst=i*4; } }
        if (NT_SUCCESS(restored) && !NT_SUCCESS(verified)) { restored=verified; }
        if (G->RestoreMismatch==0) { G->Checks|=32; }
        else if (NT_SUCCESS(restored)) { restored=STATUS_IO_DEVICE_ERROR; }
        G->RestoreStatus=restored;
        if (NT_SUCCESS(st) && !NT_SUCCESS(restored)) { st=restored; C->Reason=13; }
    }
    if (band!=NULL) { MmUnmapIoSpace(band,0x120000); }
    if (backup!=NULL) { ExFreePoolWithTag(backup,EPI_GMU_TAG); }
    if (NT_SUCCESS(st) && G->Checks!=0x3F) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<3;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
struct GMU_B1_TRACE {
    GMU_G2_TRACE Cp;
    ULONG HChecks,HReason,CcuBefore,CcuAfter,RbCntlLive;
    ULONG LiveBytes,PeakBytes,HighWater,AllocCount,FreeCount,FailCount,ReuseCount,RetirePending;
    ULONG DistinctIb;
    ULONG FramesDone,FailFrame,FailFence,FailRptr,FailWptr,MaxRptrSeen,MaxInFlight,FenceBackwards,WptrWrites;
    ULONG WaitMaxUs,WaitTotalUs,ElapsedMs;
    ULONG Ts2After[16],StaticBad,StaticFirst,TailBad,TailFirst,RingBad,RingFirst,RingValue,RingExpected,OldTailBad,OldTailFirst;
    ULONG SqGood[2],SqBad,SqFirst,SqValue,SqExpected,TexelBad[2],SqZero,SqPad,SqBg;
    ULONG BgGood[2],BgBad,BgFirst,BgValue,OutsideChanged,OutsideFirst,OutsideOld,OutsideNew,Sample[20];
    ULONG CpInterrupt,CpProtect,CpStatus1,IbBaseLow,IbBaseHigh,IbRemaining;
    NTSTATUS GlobalStatus[2];
    ULONG GlobalBefore[4],GlobalAfter[4],MaskBefore,MaskAfter;
    ULONG RestoreMismatch,RestoreFirst,HoldMs,Checks,Prepared;
    NTSTATUS BackupStatus,HoldStatus,RestoreStatus;
    ULONG Ts2Offset,InFlight2Count,OverlapBad,GateHeldA[3],GateHeldB[3],GateFenceA[3],GateFenceB[3],GateIbBase[3],GateReleased,GateFinal;
    ULONG IbOffset[61],VbOffset[61],InFlight[61]; // publication history also makes every final ring address decodable
};
static_assert(sizeof(GMU_B1_TRACE)==1424,"B-1 diagnostic ABI");
static VOID GmuB1TraceInit(GMU_B1_TRACE* t)
{
    RtlFillMemory(t,sizeof(*t),0xFF); GmuG2TraceInit(&t->Cp);
    t->FramesDone=t->MaxRptrSeen=t->MaxInFlight=t->FenceBackwards=t->WptrWrites=0;
    t->WaitMaxUs=t->WaitTotalUs=t->HoldMs=t->Checks=t->Prepared=0;
    t->InFlight2Count=t->OverlapBad=t->GateReleased=0;
    RtlZeroMemory(t->GateHeldA,sizeof(t->GateHeldA)); RtlZeroMemory(t->GateHeldB,sizeof(t->GateHeldB));
    t->GlobalStatus[0]=t->GlobalStatus[1]=STATUS_NOT_FOUND;
    t->BackupStatus=t->HoldStatus=t->RestoreStatus=STATUS_NOT_FOUND;
}
static BOOLEAN GmuB1CanFollow(const GMU_H1_TRACE* H)
{
    return H!=NULL && NT_SUCCESS(H->Cp.Status) && H->Cp.Reason==0 && H->Checks==0x3F &&
        H->Cp.Rptr==814 && H->Cp.WptrAfter==814 && H->Cp.ShadowAfter==814 &&
        H->RestoreMismatch==0 && NT_SUCCESS(H->RestoreStatus);
}
static ULONG GmuB1Gate(ULONG Frame)
{ return 0xA5000000UL|(Frame>=50 ? 50 : Frame>=30 ? 30 : Frame>=10 ? 10 : 0); }
static ULONG GmuB1IbWord(ULONG Word,ULONG Vb,ULONG Frame)
{
    static const ULONG wait[]={0x70BC8006,0x13,0x701001FC,0,0,0xFFFFFFFF,16};
    if (Word<7) { return Word==4 ? GmuB1Gate(Frame) : wait[Word]; }
    if (Word<56) { return GmuH1IbWord(Word-7,Vb); }
    if (Word<101) { const ULONG i=Word-56; return i==2 ? 0x701001F0 : i==7 ? 0x701001F4 : GmuM1IB10S[i]; }
    if (Word>=671) { return GmuM1IB10E[Word-671]; }
    const BOOLEAN second=Word>=386;
    const ULONG pos=Word-(second ? 386 : 101);
    if (pos<21) { return pos==4 ? 0x70100000+Vb+(second ? 384 : 0) : pos==6 ? 0x180 : (second ? GmuM1IB10PB[pos] : GmuM1IB10PA[pos]); }
    const ULONG tile=(pos-21)/44+(second ? 6 : 0),i=(pos-21)%44;
    static const ULONG* const sel[]={GmuM1SEL0,GmuM1SEL1,GmuM1SEL2,GmuM1SEL3,GmuM1SEL4,GmuM1SEL5,
                                   GmuM1SEL6,GmuM1SEL7,GmuM1SEL8,GmuM1SEL9,GmuM1SEL10,GmuM1SEL11};
    if (i<16) { return sel[tile][i]; }
    if (i<39) { return i==28 ? 12 : GmuM1IB10TB[i-16]; }
    static const ULONG ts[]={0x70460004,0x1A,0,0,0};
    return i==41 ? 0x701001C0+4*tile : i==43 ? 0xB4000000UL|(Frame<<8)|tile : ts[i-39];
}
static ULONG GmuB1Previous(const GMU_H1_TRACE* H,ULONG Word)
{ return Word>=265 && Word<814 ? GmuH1RingWord(H,(Word-265)/9,(Word-265)%9) : GmuQ1Expected(Word); }
static BOOLEAN GmuB1Overlap(ULONG A,ULONG An,ULONG B,ULONG Bn)
{ return A<B+Bn && B<A+An; }
static ULONG GmuB1RingWord(const GMU_B1_TRACE* G,ULONG Frame,ULONG Word)
{
    if (Word==1) { return 0x70100000+G->IbOffset[Frame]; }
    if (Word==3) { return 701; }
    if (Word==6) { return 0x701001F8; }
    if (Word==8) { return 0xA4000000UL|Frame; }
    return GmuQ1Packet[Word];
}
static VOID GmuB1Stats(GMU_B1_TRACE* G,const GMU_H1_WORK* W)
{
    G->LiveBytes=W->Heap.LiveBytes; G->PeakBytes=W->Heap.PeakBytes; G->HighWater=W->Heap.HighWater;
    G->AllocCount=W->Heap.AllocCount-W->B1Base[0]; G->FreeCount=W->Heap.FreeCount-W->B1Base[1]; G->FailCount=W->Heap.FailCount-W->B1Base[2];
    G->ReuseCount=W->Heap.ReuseCount-W->B1Base[3]; G->RetirePending=W->Heap.RetirePending;
    G->DistinctIb=0;
    for (ULONG i=0;i<61;++i) if (G->IbOffset[i]!=0xFFFFFFFF)
    {
        BOOLEAN first=TRUE;
        for (ULONG j=0;j<i;++j) if (G->IbOffset[j]==G->IbOffset[i]) { first=FALSE; break; }
        if (first) { ++G->DistinctIb; }

    }
}
static BOOLEAN GmuB1Pointer(GMU_B1_TRACE* G,ULONG Value)
{
    if (Value>G->MaxRptrSeen) { G->MaxRptrSeen=Value; }
    if (Value>=4096) { G->Cp.Reason=18; return FALSE; }
    return TRUE;
}
static BOOLEAN GmuB1Pointers(GMU_B1_TRACE* G)
{
    const BOOLEAN native=GmuB1Pointer(G,G->Cp.Rptr),shadow=GmuB1Pointer(G,G->Cp.ShadowAfter);
    return native && shadow;
}
static NTSTATUS GmuB1Observe(GMU_B1_TRACE* G,GMU_H1_WORK* W,volatile ULONG* Ts)
{
    KeMemoryBarrier(); const ULONG raw=Ts[14];
    if (raw==0xFFFFFFFF)
    {
        if (W->Completed!=0xFFFFFFFF) { ++G->FenceBackwards; G->Cp.Reason=20; return STATUS_IO_DEVICE_ERROR; }
        return STATUS_SUCCESS; // Never call Reclaim with the uninitialized sentinel.
    }
    if ((raw&0xFF000000UL)!=0xA4000000UL || (raw&0xFFFFFFUL)>=W->Submitted)
    { G->Cp.Reason=13; return STATUS_IO_DEVICE_ERROR; }
    const ULONG completed=raw&0xFFFFFFUL;
    if (W->Completed!=0xFFFFFFFF && completed<W->Completed)
    { ++G->FenceBackwards; G->Cp.Reason=20; return STATUS_IO_DEVICE_ERROR; }
    W->Completed=completed; G->FramesDone=completed+1;
    if (!EpiGpuHeapReclaim(&W->Heap,completed)) { G->Cp.Reason=19; return STATUS_IO_DEVICE_ERROR; }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuB1Live(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_B1_TRACE* G,
                          volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow)
{
    GMU_G2_TRACE* C=&G->Cp;
    if (!GmuG2Live(S,Cc,T,C)) { return STATUS_INVALID_DEVICE_STATE; }
    G->MaskAfter=READ_REGISTER_ULONG(Gpu+0x38);
    if (G->MaskAfter) { C->Reason=15; return STATUS_INVALID_DEVICE_STATE; }
    KeMemoryBarrier(); C->ShadowAfter=*Shadow; C->Rptr=READ_REGISTER_ULONG(Cp+6);
    if (!GmuB1Pointers(G)) { return STATUS_IO_DEVICE_ERROR; }
    C->WptrAfter=READ_REGISTER_ULONG(Cp+7); C->IntStatus=READ_REGISTER_ULONG(Gpu+0x201);
    if (C->IntStatus&(1UL<<23)) { C->Reason=11; return STATUS_IO_DEVICE_ERROR; }
    if (C->IntStatus&((1UL<<1)|(1UL<<9)|(1UL<<24))) { C->Reason=14; return STATUS_IO_DEVICE_ERROR; }
    return STATUS_SUCCESS;
}
static NTSTATUS GmuB1Ahb(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_B1_TRACE* G,
                         volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow,ULONG Target,ULONGLONG Begin)
{
    for (ULONG retry=0;;++retry)
    {
        NTSTATUS st=GmuB1Live(S,Cc,T,G,Cp,Gpu,Shadow); if (!NT_SUCCESS(st)) { return st; }
        if (retry) { WRITE_REGISTER_ULONG(Cp+7,Target); KeMemoryBarrier(); ++G->Cp.WptrWrites; }
        G->Cp.FenceStatus=READ_REGISTER_ULONG(GmuReg(S,0x23B13));
        if (!(G->Cp.FenceStatus&1)) { return STATUS_SUCCESS; }
        if (retry==200 || KeQueryInterruptTime()-Begin>=20000) { G->Cp.Reason=10; return STATUS_IO_TIMEOUT; }
        KeStallExecutionProcessor(10);
    }
}
static NTSTATUS GmuB1Wait(GMU_TEST* S,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_B1_TRACE* G,GMU_H1_WORK* W,
                          volatile ULONG* Cp,volatile ULONG* Gpu,volatile ULONG* Shadow,volatile ULONG* Ts,
                          ULONG Target,BOOLEAN Final)
{
    const ULONGLONG begin=KeQueryInterruptTime(); NTSTATUS st=STATUS_SUCCESS;
    for (ULONG poll=0;;++poll)
    {
        G->Cp.Polls=poll;
        st=GmuB1Live(S,Cc,T,G,Cp,Gpu,Shadow); if (!NT_SUCCESS(st)) { break; }
        st=GmuB1Observe(G,W,Ts); if (!NT_SUCCESS(st)) { break; }
        if (KeQueryInterruptTime()-begin>=1000000) { G->Cp.Reason=16; st=STATUS_IO_TIMEOUT; break; }
        if (W->Completed!=0xFFFFFFFF && W->Completed>=Target)
        {
            if (!Final) { break; }
            G->Cp.RbbmStatus=READ_REGISTER_ULONG(Gpu+0x210); G->Cp.CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
            G->IbRemaining=READ_REGISTER_ULONG(Cp+0x12A);
            if (W->Completed==60 && G->Cp.Rptr==1363 && G->Cp.WptrAfter==1363 && G->Cp.ShadowAfter==1363 &&
                !(G->Cp.RbbmStatus&~1UL) && !(G->Cp.CxBusy&(1UL<<23)) && G->IbRemaining==0) { break; }
        }
        KeStallExecutionProcessor(100);
    }
    const ULONG us=static_cast<ULONG>((KeQueryInterruptTime()-begin+9)/10);
    if (us>G->WaitMaxUs) { G->WaitMaxUs=us; } G->WaitTotalUs+=us;
    return st;
}
static NTSTATUS GmuB1Submit(GMU_TEST* S,HANDLE Params,volatile ULONG* Cc,const GMU_GX_TRACE* T,GMU_B1_TRACE* G,GMU_H1_WORK* W,HANDLE Diag,const GMU_D1_TRACE* D,const GMU_H1_TRACE* H)
{
    GMU_G2_TRACE* C=&G->Cp;
    W->B1Base[0]=W->Heap.AllocCount; W->B1Base[1]=W->Heap.FreeCount;
    W->B1Base[2]=W->Heap.FailCount; W->B1Base[3]=W->Heap.ReuseCount;
    W->Completed=0xFFFFFFFF; W->Submitted=0;
    NTSTATUS st=GmuG2Guard(S,Params,Cc,T,C,0x2100); if (!NT_SUCCESS(st)) { return st; }
    if (!GmuB1CanFollow(H) || G->HChecks!=0x3F || G->HReason!=0) { C->Reason=8; return STATUS_INVALID_DEVICE_STATE; }
    GMU_REGION* ring=GmuFind(S,GmuG1Iova[1],GmuG1Size[1]);
    GMU_REGION* heap=GmuFind(S,0x70100000,0x40000);
    GMU_REGION* shadow=GmuFind(S,GmuG1Iova[2],GmuG1Size[2]);
    if (heap==NULL || heap->External || heap->Va==NULL || !heap->Mapped || heap->Size!=0x40000 ||
        heap->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE) || S->Smmu==NULL || ring==NULL || shadow==NULL || ring->Va==NULL || shadow->Va==NULL ||
        !ring->Mapped || !shadow->Mapped || ring->Flags!=GmuG1Flags[1] || shadow->Flags!=GmuG1Flags[2] ||
        ring->Iova!=0x70010000 || ring->Size<0x8000 || shadow->Iova!=0x70020000)
    { C->Reason=12; return STATUS_INVALID_DEVICE_STATE; }
    volatile ULONG* rptr=static_cast<volatile ULONG*>(shadow->Va);
    volatile ULONG* words=static_cast<volatile ULONG*>(ring->Va);
    PVOID band=NULL; ULONG* backup=NULL;
    volatile ULONG* hw=static_cast<volatile ULONG*>(heap->Va);
    volatile ULONG* ts=hw+0x1C0/4;
    PVOID maps[3]={}; const ULONG offsets[]={0,0x2000,0x23000};
    for (ULONG i=0;i<3;++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GPU_BASE+offsets[i];
        maps[i]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        if (maps[i]==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; C->Reason=GX_REG_MAP_FAILED; break; }
    }
    if (NT_SUCCESS(st))
    {
        volatile ULONG* cp=static_cast<volatile ULONG*>(maps[1]);
        volatile ULONG* gpu=static_cast<volatile ULONG*>(maps[0]);
        volatile ULONG* ccu=static_cast<volatile ULONG*>(maps[2])+0x207;
        st=GmuG2Guard(S,Params,Cc,T,C,0x2101);
        if (NT_SUCCESS(st))
        {
            G->MaskBefore=READ_REGISTER_ULONG(gpu+0x38);
            if (G->MaskBefore!=0) { C->Reason=15; st=STATUS_INVALID_DEVICE_STATE; }
            else
            {
                KeMemoryBarrier(); C->ShadowBefore=*rptr; C->WptrBefore=READ_REGISTER_ULONG(cp+7);
                C->Rptr=READ_REGISTER_ULONG(cp+6);
                C->ShadowAfter=C->ShadowBefore; C->WptrAfter=C->WptrBefore;
                if (!GmuB1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; }
                if (NT_SUCCESS(st)) { G->RbCntlLive=READ_REGISTER_ULONG(cp+2); }
                BOOLEAN previous=C->ShadowBefore==814 && C->WptrBefore==814 && C->Rptr==814;
                if (NT_SUCCESS(st)) { G->CcuBefore=READ_REGISTER_ULONG(ccu); }
                if (G->RbCntlLive!=0x20B || G->CcuBefore!=0x10000000) { previous=FALSE; }
                for (ULONG i=0;i<4096;++i) { if (words[i]!=GmuB1Previous(H,i)) { previous=FALSE; } }
                if (!(D!=NULL && EpiGmuD1DisplayMatches(D->Disp))) { previous=FALSE; }
                GMU_REGION* ext=GmuFind(S,0x71000000,0x120000);
                if (ext==NULL || !ext->External || !ext->Mapped || ext->Pa.QuadPart!=0xE1462000 ||
                    ext->Size!=0x120000 || ext->Flags!=(EPI_SMMU_READ|EPI_SMMU_WRITE)) { previous=FALSE; }
                if (W->Heap.LiveBytes!=960 || W->Heap.RetirePending!=0 || W->Heap.FailCount!=0) { previous=FALSE; }
                for (ULONG object=1;object<6;++object) for (ULONG i=0;i<((GmuH1Size[object]+63)&~63UL)/4;++i)
                    if (hw[GmuH1Offsets[object]/4+i]!=GmuH1StaticWord(object,i)) { previous=FALSE; }
                if (hw[0]!=0xCAFE001B || hw[1]!=0xCAFE001C || hw[2]!=0xA300003C) { previous=FALSE; }
                if (NT_SUCCESS(st) && !previous) { C->Reason=12; st=STATUS_INVALID_DEVICE_STATE; }
                
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x2102);
            if (NT_SUCCESS(st)) { G->GlobalStatus[0]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalBefore); }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x2108);
            if (NT_SUCCESS(st))
            {
                backup=static_cast<ULONG*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,0x124000,EPI_GMU_TAG));
                if (backup==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                else
                {
                    PHYSICAL_ADDRESS pa; pa.QuadPart=0xE1462000;
                    band=MmMapIoSpaceEx(pa,0x120000,PAGE_READWRITE|PAGE_NOCACHE);
                    if (band==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
                    else
                    {
                        for (ULONG i=0;i<0x120000/4;++i) { backup[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i); }
                        for (ULONG i=0;i<0x4000/4;++i) { backup[0x120000/4+i]=words[0x4000/4+i]; }
                        KeMemoryBarrier();
                    }
                }
                G->BackupStatus=st;
                if (!NT_SUCCESS(st)) { C->Reason=GX_REG_MAP_FAILED; }
            }
        }
        if (NT_SUCCESS(st))
        {
            st=GmuG2Guard(S,Params,Cc,T,C,0x2103);
            if (NT_SUCCESS(st))
            {
                G->Ts2Offset=EpiGpuHeapAlloc(&W->Heap,64,64);
                if (G->Ts2Offset==0xFFFFFFFF) { C->Reason=19; st=STATUS_INSUFFICIENT_RESOURCES; }
                else if (G->Ts2Offset!=0x1C0) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                else
                {
                    for (ULONG i=0;i<16;++i) { ts[i]=i==15 ? 0xA5000000 : 0xFFFFFFFF; }
                    KeMemoryBarrier();
                    for (ULONG i=0;i<16;++i) if (ts[i]!=(i==15 ? 0xA5000000UL : 0xFFFFFFFFUL)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                    if (NT_SUCCESS(st)) { G->Checks|=1; G->Prepared=1; }
                }
            }
        }
        ULONGLONG firstPublish=0;
        if (NT_SUCCESS(st))
        {
            for (ULONG frame=0;frame<61;++frame)
            {
                if ((frame%16)==0) { st=GmuG2Guard(S,Params,Cc,T,C,0x2140+frame/16); }
                if (NT_SUCCESS(st)) { st=GmuB1Live(S,Cc,T,G,cp,gpu,rptr); }
                if (NT_SUCCESS(st)) { st=GmuB1Observe(G,W,ts); }
                if (NT_SUCCESS(st))
                {
                    G->IbOffset[frame]=EpiGpuHeapAlloc(&W->Heap,2804,64);
                    if (G->IbOffset[frame]!=0xFFFFFFFF) { G->VbOffset[frame]=EpiGpuHeapAlloc(&W->Heap,768,64); }
                    if (G->IbOffset[frame]==0xFFFFFFFF || G->VbOffset[frame]==0xFFFFFFFF)
                    { C->Reason=19; st=STATUS_INSUFFICIENT_RESOURCES; }
                    else if (frame==0 && (G->IbOffset[0]!=0x400 || G->VbOffset[0]!=0xF00)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                if (NT_SUCCESS(st))
                {
                    const ULONG done=W->Completed==0xFFFFFFFF ? 0 : W->Completed+1;
                    for (ULONG old=done;old<frame;++old)
                    {
                        if (GmuB1Overlap(G->IbOffset[frame],2816,G->IbOffset[old],2816) ||
                            GmuB1Overlap(G->IbOffset[frame],2816,G->VbOffset[old],768) ||
                            GmuB1Overlap(G->VbOffset[frame],768,G->IbOffset[old],2816) ||
                            GmuB1Overlap(G->VbOffset[frame],768,G->VbOffset[old],768)) { ++G->OverlapBad; }
                    }
                    if (GmuB1Overlap(G->IbOffset[frame],2816,G->VbOffset[frame],768)) { ++G->OverlapBad; }
                    if (G->OverlapBad) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                if (NT_SUCCESS(st))
                {
                    volatile ULONG* vb=hw+G->VbOffset[frame]/4;
                    volatile ULONG* ib=hw+G->IbOffset[frame]/4;
                    // Every reused VB is fully initialized, not just its position fields.
                    for (ULONG pass=0;pass<2;++pass) for (ULONG i=0;i<96;++i)
                        { vb[pass*96+i]=GmuQ1VertexWord((2*frame)%120,pass!=0,i); }
                    for (ULONG i=0;i<701;++i) { ib[i]=GmuB1IbWord(i,G->VbOffset[frame],frame); }
                    KeMemoryBarrier();
                    for (ULONG pass=0;pass<2;++pass) for (ULONG i=0;i<96;++i)
                        if (vb[pass*96+i]!=GmuQ1VertexWord((2*frame)%120,pass!=0,i)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                    for (ULONG i=0;i<701;++i) if (ib[i]!=GmuB1IbWord(i,G->VbOffset[frame],frame)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                if (NT_SUCCESS(st))
                {
                    for (ULONG i=0;i<9;++i) { words[814+9*frame+i]=GmuB1RingWord(G,frame,i); }
                    KeMemoryBarrier();
                    for (ULONG i=0;i<9;++i) if (words[814+9*frame+i]!=GmuB1RingWord(G,frame,i)) { C->Reason=13; st=STATUS_IO_DEVICE_ERROR; }
                }
                if (NT_SUCCESS(st))
                {
                    const ULONGLONG begin=KeQueryInterruptTime();
                    if (!C->Attempted) { firstPublish=begin; }
                    W->Submitted=frame+1; C->Attempted=1;
                    WRITE_REGISTER_ULONG(cp+7,814+9*(frame+1)); KeMemoryBarrier(); ++C->WptrWrites;
                    // Even a rejected/timeout fenced write may have reached CP: never free these speculatively.
                    if (!EpiGpuHeapRetire(&W->Heap,G->IbOffset[frame],2804,frame) ||
                        !EpiGpuHeapRetire(&W->Heap,G->VbOffset[frame],768,frame)) { C->Reason=19; st=STATUS_IO_DEVICE_ERROR; }
                    if (NT_SUCCESS(st)) { st=GmuB1Ahb(S,Cc,T,G,cp,gpu,rptr,814+9*(frame+1),begin); }
                    if (NT_SUCCESS(st)) { st=GmuB1Observe(G,W,ts); }
                    if (NT_SUCCESS(st))
                    {
                        G->InFlight[frame]=W->Completed==0xFFFFFFFF ? frame+1 : frame-W->Completed;
                        if (G->InFlight[frame]>G->MaxInFlight) { G->MaxInFlight=G->InFlight[frame]; }
                        if (G->InFlight[frame]==2) { ++G->InFlight2Count; }
                        if (frame==10 || frame==30 || frame==50)
                        {
                            const ULONG index=(frame-10)/20;
                            st=GmuG2Guard(S,Params,Cc,T,C,0x2150+index);
                            if (NT_SUCCESS(st)) { st=GmuB1Wait(S,Cc,T,G,W,cp,gpu,rptr,ts,frame-1,FALSE); }
                            for (ULONG poll=0;NT_SUCCESS(st) && poll<20;++poll)
                            {
                                KeStallExecutionProcessor(100);
                                st=GmuB1Live(S,Cc,T,G,cp,gpu,rptr);
                                if (NT_SUCCESS(st)) { st=GmuB1Observe(G,W,ts); }
                            }
                            G->GateFenceA[index]=ts[14]; G->GateHeldA[index]=ts[14]==(0xA4000000UL|(frame-1));
                            if (NT_SUCCESS(st)) { G->GateIbBase[index]=READ_REGISTER_ULONG(cp+0x128); }
                        }
                        else
                        {
                            if (frame==11 || frame==31 || frame==51)
                            {
                                const ULONG index=(frame-11)/20;
                                G->GateFenceB[index]=ts[14]; G->GateHeldB[index]=W->Completed==frame-2;
                                ts[15]=GmuB1Gate(frame); KeMemoryBarrier(); ++G->GateReleased;
                            }
                            if (frame>=1) { st=GmuB1Wait(S,Cc,T,G,W,cp,gpu,rptr,ts,frame-1,FALSE); }
                        }
                    }
                }
                if (!NT_SUCCESS(st)) { G->FailFrame=frame; G->FailFence=ts[14]; G->FailRptr=C->Rptr; G->FailWptr=C->WptrAfter; break; }
            }
            if (NT_SUCCESS(st))
            {
                st=GmuG2Guard(S,Params,Cc,T,C,0x2104);
                if (NT_SUCCESS(st)) { st=GmuB1Wait(S,Cc,T,G,W,cp,gpu,rptr,ts,60,TRUE); }
                if (!NT_SUCCESS(st)) { G->FailFrame=60; G->FailFence=ts[14]; G->FailRptr=C->Rptr; G->FailWptr=C->WptrAfter; }
            }
            G->WptrWrites=C->WptrWrites;
            if (C->Attempted) { G->ElapsedMs=static_cast<ULONG>((KeQueryInterruptTime()-firstPublish)/10000); }
        }
        if (!NT_SUCCESS(st) && G->Prepared && W->Submitted)
        {
            const ULONG last=W->Submitted-1;
            if (ts[15]!=GmuB1Gate(last)) { ts[15]=GmuB1Gate(last); KeMemoryBarrier(); ++G->GateReleased; }
            // Give accepted jobs a bounded opportunity to drain before restoring display RAM.
            const ULONG reason=C->Reason;
            (void)GmuB1Wait(S,Cc,T,G,W,cp,gpu,rptr,ts,last,FALSE);
            C->Reason=reason;
        }
        const ULONG beforeCcu=C->Reason;
        if (C->Reason!=18 && GmuG2Live(S,Cc,T,C)) { G->CcuAfter=READ_REGISTER_ULONG(ccu); }
        if (!NT_SUCCESS(st)) { C->Reason=beforeCcu; }
        volatile ULONG* diagRegs[]={cp+6,cp+7,cp+0x21,cp+0x23,cp+0x24,cp+0x25,
                                   gpu+0x210,gpu+0x201,cp+0x128,cp+0x129,cp+0x12A,gpu+0x38};
        ULONG* diagValues[]={&C->Rptr,&C->WptrAfter,&C->CpHwFault,&G->CpInterrupt,&G->CpProtect,&G->CpStatus1,
                             &C->RbbmStatus,&C->IntStatus,&G->IbBaseLow,&G->IbBaseHigh,&G->IbRemaining,&G->MaskAfter};
        const ULONG firstReason=C->Reason;
        NTSTATUS sampled=STATUS_SUCCESS;
        for (ULONG i=0;i<RTL_NUMBER_OF(diagRegs);++i)
        {
            if (C->Reason==18) { break; }
            sampled=GmuG2Guard(S,Params,Cc,T,C,0x2110+i); if (!NT_SUCCESS(sampled)) { break; }
            *diagValues[i]=READ_REGISTER_ULONG(diagRegs[i]);
            if (i==0)
            {
                KeMemoryBarrier(); C->ShadowAfter=*rptr;
                if (!GmuB1Pointers(G)) { st=STATUS_IO_DEVICE_ERROR; break; }
            }
        }
        if (!NT_SUCCESS(st) && C->Reason!=18) { C->Reason=firstReason; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        const ULONG prior=C->Reason;
        sampled=GmuG2Guard(S,Params,Cc,T,C,0x2120);
        if (NT_SUCCESS(sampled)) { G->GlobalStatus[1]=EpiSmmuQueryGlobalFault(S->Smmu,G->GlobalAfter); }
        if (!NT_SUCCESS(st)) { C->Reason=prior; }
        else if (!NT_SUCCESS(sampled)) { st=sampled; }
        C->CxBusy=READ_REGISTER_ULONG(GmuReg(S,0x23B0C));
        KeMemoryBarrier(); C->ShadowAfter=*rptr;
        if (!GmuB1Pointer(G,C->ShadowAfter)) { st=STATUS_IO_DEVICE_ERROR; }
        for (ULONG i=0;i<16;++i) { G->Ts2After[i]=ts[i]; }
        G->GateFinal=ts[15];
        GmuB1Stats(G,W);
        if (G->Prepared)
        {
            BOOLEAN tileOk=G->Ts2After[12]==0xB2000100 && G->Ts2After[13]==0xB2000101;
            for (ULONG i=0;i<12;++i) if (G->Ts2After[i]!=(0xB4003C00UL|i)) { tileOk=FALSE; }
            if (G->GateHeldA[0] && G->GateHeldA[1] && G->GateHeldA[2] && G->GateHeldB[0] && G->GateHeldB[1] && G->GateHeldB[2] &&
                G->MaxInFlight==2 && G->OverlapBad==0 && G->GateFinal==0xA5000032) { G->Checks|=8; }
            const ULONG errors=(1UL<<1)|(1UL<<9)|(1UL<<23)|(1UL<<24);
            if (G->FramesDone==61 && G->FenceBackwards==0 && G->Ts2After[14]==0xA400003C && tileOk && G->CcuAfter==0x10000000 &&
                G->IbRemaining==0 && C->ShadowAfter==1363 && C->Rptr==1363 && C->WptrAfter==1363 && !(C->IntStatus&errors) &&
                !(C->RbbmStatus&~1UL) && !(C->CxBusy&(1UL<<23))) { G->Checks|=2; }
            if (G->FailCount==0 && G->AllocCount==123 && G->FreeCount==122 && G->LiveBytes==1024 &&
                G->PeakBytes<=11776 && G->ReuseCount>=1 && G->RetirePending==0) { G->Checks|=4; }
            G->StaticBad=0; G->TailBad=0;
            for (ULONG object=1;object<6;++object) for (ULONG i=0;i<((GmuH1Size[object]+63)&~63UL)/4;++i)
                if (hw[GmuH1Offsets[object]/4+i]!=GmuH1StaticWord(object,i))
                { ++G->StaticBad; if (G->StaticFirst==0xFFFFFFFF) { G->StaticFirst=GmuH1Offsets[object]+i*4; } }
            for (ULONG off=W->Heap.HighWater;off<0x40000;off+=4) if (hw[off/4]!=0)
                { ++G->TailBad; if (G->TailFirst==0xFFFFFFFF) { G->TailFirst=off; } }
        }
        if (band!=NULL && NT_SUCCESS(G->BackupStatus))
        {
            G->RingBad=0; G->OldTailBad=0;
            for (ULONG i=0;i<4096;++i)
            {
                const ULONG frame=(i>=814 && i<1363) ? (i-814)/9 : 61;
                const ULONG expected=frame<W->Submitted ? GmuB1RingWord(G,frame,(i-814)%9) : GmuB1Previous(H,i),value=words[i];
                if (value!=expected)
                { ++G->RingBad; if (G->RingFirst==0xFFFFFFFF) { G->RingFirst=i; G->RingValue=value; G->RingExpected=expected; } }
            }
            for (ULONG off=0x4000;off<0x8000;off+=4)
            {
                const ULONG old=backup[0x120000/4+(off-0x4000)/4],value=words[off/4];
                if (old!=value)
                { ++G->OldTailBad; if (G->OldTailFirst==0xFFFFFFFF) { G->OldTailFirst=off; } }
            }
            if (G->Prepared && G->RingBad==0 && G->OldTailBad==0 && G->StaticBad==0 && G->TailBad==0 && hw[0]==0xCAFE001B && hw[1]==0xCAFE001C && hw[2]==0xA300003C) { G->Checks|=16; }
            G->SqGood[0]=0; G->SqGood[1]=0; G->SqBad=0; G->SqZero=0; G->SqPad=0; G->SqBg=0;
            G->TexelBad[0]=0; G->TexelBad[1]=0; G->BgGood[0]=0; G->BgGood[1]=0; G->BgBad=0; G->OutsideChanged=0;
            for (ULONG i=0;i<0x120000/4;++i)
            {
                const ULONG offset=i*4,fb=0x462000+offset,y=fb/4320,x=(fb%4320)/4;
                const ULONG value=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i);
                if (x>=412 && x<668 && y>=1072 && y<1328)
                {
                    const ULONG v=(y-1072)/2;
                    if (x>=460 && x<620 && v>=24 && v<104)
                    {
                        const ULONG ti=(x-460)/20,tj=(v-24)/10,texel=tj*8+ti,expected=GmuT1Texel(ti,tj);
                        if (value==0 || value==0xFF000000) { ++G->SqZero; }
                        if (value==0xFF202020) { ++G->SqBg; }
                        if (value==0xFFFF00FF) { ++G->SqPad; }
                        if (value==expected) { if (y&1) { ++G->SqGood[1]; } else { ++G->SqGood[0]; } }
                        else
                        {
                            ++G->SqBad; G->TexelBad[texel/32]|=1UL<<(texel%32);
                            if (G->SqFirst==0xFFFFFFFF) { G->SqFirst=offset; G->SqValue=value; G->SqExpected=expected; }
                        }
                    }
                    else if (value==0xFF202020) { if (y&1) { ++G->BgGood[1]; } else { ++G->BgGood[0]; } }
                    else { ++G->BgBad; if (G->BgFirst==0xFFFFFFFF) { G->BgFirst=offset; G->BgValue=value; } }
                }
                else if (value!=backup[i])
                {
                    ++G->OutsideChanged;
                    if (G->OutsideFirst==0xFFFFFFFF) { G->OutsideFirst=offset; G->OutsideOld=backup[i]; G->OutsideNew=value; }
                }
            }
            const ULONG sample[][2]={{470,29},{610,29},{610,99},{470,99},{530,59},{550,74},{479,33},{480,34},{414,2},{665,125}};
            for (ULONG i=0;i<10;++i) for (ULONG parity=0;parity<2;++parity)
            { G->Sample[i*2+parity]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+((1072+sample[i][1]*2+parity)*4320+sample[i][0]*4-0x462000)/4); }
            if (G->SqGood[0]==12800 && G->SqGood[1]==12800 && G->BgGood[0]==19968 && G->BgGood[1]==19968 && G->OutsideChanged==0) { G->Checks|=32; }
        }
    }
    GmuB1Stats(G,W);
    ULONG fsr=0,syn=0; ULONGLONG faultAddr=0;
    C->FaultStatus=EpiSmmuQueryFault(S->Smmu,&fsr,&faultAddr,&syn);
    if (NT_SUCCESS(C->FaultStatus))
    { C->Fsr=fsr; C->FarLow=static_cast<ULONG>(faultAddr); C->FarHigh=static_cast<ULONG>(faultAddr>>32); C->Fsynr=syn; }
    if (band!=NULL && NT_SUCCESS(G->BackupStatus))
    {
        if (C->Attempted && NT_SUCCESS(st))
        {
            C->Status=st;
            NTSTATUS held=GmuStep(Params,0x2130);
            if (NT_SUCCESS(held)) { held=EpiSetValue(Diag,L"GpuGmuB1",REG_BINARY,G,sizeof(*G)); }
            if (NT_SUCCESS(held)) { held=ZwFlushKey(Diag); }
            if (NT_SUCCESS(held))
            {
                held=GmuA1Pause(2000);
                if (NT_SUCCESS(held)) { G->HoldMs=2000; }
            }
            G->HoldStatus=held;
            if (NT_SUCCESS(st) && !NT_SUCCESS(held)) { st=held; C->Reason=6; }
        }
        // Restoring display RAM is mandatory even when a breadcrumb or GX prerequisite failed.
        NTSTATUS restored=GmuStep(Params,0x2131);
        for (ULONG i=0;i<0x120000/4;++i) { WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i,backup[i]); }
        KeMemoryBarrier();
        const NTSTATUS verified=GmuStep(Params,0x2132);
        G->RestoreMismatch=0;
        for (ULONG i=0;i<0x120000/4;++i) if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(band)+i)!=backup[i])
        { ++G->RestoreMismatch; if (G->RestoreFirst==0xFFFFFFFF) { G->RestoreFirst=i*4; } }
        if (NT_SUCCESS(restored) && !NT_SUCCESS(verified)) { restored=verified; }
        if (G->RestoreMismatch==0) { G->Checks|=64; }
        else if (NT_SUCCESS(restored)) { restored=STATUS_IO_DEVICE_ERROR; }
        G->RestoreStatus=restored;
        if (NT_SUCCESS(st) && !NT_SUCCESS(restored)) { st=restored; C->Reason=13; }
    }
    if (band!=NULL) { MmUnmapIoSpace(band,0x120000); }
    if (backup!=NULL) { ExFreePoolWithTag(backup,EPI_GMU_TAG); }
    if (NT_SUCCESS(st) && G->Checks!=0x7F) { C->Reason=G->Checks==0x77 ? 21 : 13; st=STATUS_IO_DEVICE_ERROR; }
    if (NT_SUCCESS(st)) { C->Reason=0; }
    for (ULONG i=0;i<3;++i) { if (maps[i]!=NULL) { MmUnmapIoSpace(maps[i],PAGE_SIZE); } }
    return st;
}
static NTSTATUS GmuGxOob(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc, GMU_GX_TRACE* T,
                         GMU_GX_REG_TRACE* GxReg = NULL, GMU_GX_HALT_TRACE* Halt = NULL,
                         GMU_GX_INIT_TRACE* Init = NULL, BOOLEAN AddrMode = FALSE, GMU_GX_HWCG_TRACE* Hwcg = NULL,
                         GMU_GX_CFG_TRACE* Cfg = NULL, GMU_GX_STAGE_E_TRACE* StageE = NULL,
                         GMU_GX_STAGE_F_TRACE* StageF = NULL, GMU_G1_TRACE* G1 = NULL, GMU_G2_TRACE* G2 = NULL,
                         GMU_G3_TRACE* G3 = NULL, GMU_G3B_TRACE* G3b = NULL, GMU_G3C_TRACE* G3c = NULL,
                         GMU_G4_TRACE* G4 = NULL, GMU_G5_TRACE* G5 = NULL, GMU_G5B_TRACE* G5b = NULL,
                         GMU_Z1_TRACE* Z1 = NULL,HANDLE Diag = NULL,const UCHAR* Zap = NULL,ULONG ZapLen = 0,
                         GMU_S1_TRACE* S1 = NULL, GMU_R1_TRACE* R1 = NULL, GMU_D1_TRACE* D1 = NULL, GMU_V1_TRACE* V1 = NULL, GMU_A1_TRACE* A1 = NULL, GMU_T1_TRACE* T1 = NULL, GMU_W1_TRACE* W1 = NULL, GMU_M1_TRACE* M1 = NULL, GMU_Q1_TRACE* Q1 = NULL, GMU_H1_TRACE* H1 = NULL, GMU_H1_WORK* H1Work = NULL, GMU_B1_TRACE* B1 = NULL)
{
    NTSTATUS st=GmuGxStep(Params,0x7C0,T);
    if (!NT_SUCCESS(st)) { T->OobStatus=st; return st; }
    WRITE_REGISTER_ULONG(GmuReg(S,R_GMU2HOST_INTR_CLR),0x80000000UL);
    GmuGxSnap(S,Cc,T,0);
    st=GmuGxStep(Params,0x7C1,T);
    if (!NT_SUCCESS(st)) { T->OobStatus=st; return st; }
    T->Attempted=1;
    WRITE_REGISTER_ULONG(GmuReg(S,R_HOST2GMU_INTR_SET),0x40000000UL);
    GmuGxStep(Params,0x7C2,T);
    T->OobStatus=GmuGxWait(GmuReg(S,R_GMU2HOST_INTR_INFO),0x80000000UL,0x80000000UL,
                            100,100,&T->OobPolls,&T->OobInfo);
    // Clear/release are mandatory after publication, even if persistence or ACK waiting failed.
    GmuGxStep(Params,0x7C3,T);
    WRITE_REGISTER_ULONG(GmuReg(S,R_GMU2HOST_INTR_CLR),0x80000000UL);
    GmuGxSnap(S,Cc,T,1);
    if (GxReg!=NULL)
    {
        GmuGxReadReg(S,Params,Cc,T,GxReg,Halt);
        if (Halt!=NULL && Halt->Status==STATUS_NOT_FOUND && Halt->Reason==GX_REG_NOT_REACHED)
        {
            Halt->Reason=GxReg->Reason==GX_REG_READ ? 8 : GxReg->Reason;
            Halt->Gdscr=GxReg->Gdscr; Halt->GxHm=GxReg->GxHm;
            Halt->Status=STATUS_INVALID_DEVICE_STATE; // six reads did not complete; no writes allowed
        }
    }
    if (Init!=NULL)
    {
        if (Halt!=NULL && NT_SUCCESS(Halt->Status) && Halt->Count==2)
        {
            Init->Status=GmuGxSecvid(S,Params,Cc,T,Init);
            if (NT_SUCCESS(Init->Status) && AddrMode)
            { Init->Status=GmuGxAddrMode(S,Params,Cc,T,Init); }
        }
        else { Init->Reason=8; Init->Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (Hwcg!=NULL)
    {
        if (Init!=NULL && NT_SUCCESS(Init->Status) && Init->Count==16)
        { Hwcg->Status=GmuGxHwcg(S,Params,Cc,T,Hwcg); }
        else { Hwcg->Reason=8; Hwcg->Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (Cfg!=NULL)
    {
        if (Hwcg!=NULL && NT_SUCCESS(Hwcg->Status)) { Cfg->Status=GmuGxCfg(S,Params,Cc,T,Cfg); }
        else { Cfg->Reason=8; Cfg->Status=STATUS_INVALID_DEVICE_STATE; }
        if (Cfg->CxWritten&4) { T->PowerCounterOriginal=Cfg->CxBefore[2]; T->PowerCounterSaved=1; }
    }
    if (StageE!=NULL)
    {
        if (Cfg!=NULL && NT_SUCCESS(Cfg->Status)) { StageE->Status=GmuGxStageE(S,Params,Cc,T,StageE); }
        else { StageE->Reason=8; StageE->Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (StageF!=NULL)
    {
        if (StageE!=NULL && NT_SUCCESS(StageE->Status)) { StageF->Status=GmuGxStageF(S,Params,Cc,T,StageF); }
        else { StageF->Reason=8; StageF->Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (G1!=NULL)
    {
        if (StageF!=NULL && NT_SUCCESS(StageF->Status) && NT_SUCCESS(G1->MemoryStatus))
        { G1->RegisterStatus=GmuG1Registers(S,Params,Cc,T,G1); }
        else { G1->Reason=8; G1->RegisterStatus=STATUS_INVALID_DEVICE_STATE; }
    }
    if (G2!=NULL)
    {
        if (G1!=NULL && NT_SUCCESS(G1->RegisterStatus) && NT_SUCCESS(G1->MemoryStatus) && G1->Count==7)
        {
            BOOLEAN bases=TRUE;
            for (ULONG i=0;i<7;++i) { if (G1->After[i]!=G1->Value[i]) { bases=FALSE; } }
            if (bases) { G2->Status=GmuG2Start(S,Params,Cc,T,G2); }
            else { G2->Reason=12; G2->Status=STATUS_INVALID_DEVICE_STATE; }
        }
        else { G2->Reason=8; G2->Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (G3!=NULL)
    {
        if (G2!=NULL && NT_SUCCESS(G2->Status)) { G3->Cp.Status=GmuG3Submit(S,Params,Cc,T,G3); }
        else { G3->Cp.Reason=8; G3->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (G3b!=NULL)
    {
        if (G3!=NULL && NT_SUCCESS(G3->Cp.Status)) { G3b->Cp.Status=GmuG3bSubmit(S,Params,Cc,T,G3b); }
        else { G3b->Cp.Reason=8; G3b->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (G3c!=NULL)
    {
        if (G3b!=NULL && NT_SUCCESS(G3b->Cp.Status)) { G3c->Cp.Status=GmuG3cSubmit(S,Params,Cc,T,G3c); }
        else { G3c->Cp.Reason=8; G3c->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (G4!=NULL)
    {
        if (G3c!=NULL && NT_SUCCESS(G3c->Cp.Status)) { G4->Cp.Status=GmuG4Submit(S,Params,Cc,T,G4); }
        else { G4->Cp.Reason=8; G4->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (G5!=NULL)
    {
        if (G4!=NULL && NT_SUCCESS(G4->Cp.Status)) { G5->Cp.Status=GmuG5Submit(S,Params,Cc,T,G5); }
        else { G5->Cp.Reason=8; G5->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (G5b!=NULL)
    {
        if (GmuG5bCanFollow(G5)) { G5b->Draw.Cp.Status=GmuG5bSubmit(S,Params,Cc,T,G5b); }
        else { G5b->Draw.Cp.Reason=8; G5b->Draw.Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (Z1!=NULL)
    {
        if (GmuZ1CanFollow(G5b)) { Z1->Gpu.Draw.Cp.Status=GmuZ1Submit(S,Params,Cc,T,Z1,Diag,Zap,ZapLen); }
        else { Z1->Gpu.Draw.Cp.Reason=8; Z1->Gpu.Draw.Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (S1!=NULL)
    {
        if (Z1!=NULL) { S1->ZChecks=Z1->Gpu.Draw.Checks; S1->ZReason=Z1->Gpu.Draw.Cp.Reason; }
        if (GmuS1CanFollow(Z1)) { S1->Checks=1; S1->Cp.Status=GmuS1Submit(S,Params,Cc,T,S1); }
        else { S1->Cp.Reason=8; S1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (R1!=NULL)
    {
        if (S1!=NULL) { R1->SChecks=S1->Checks; R1->SReason=S1->Cp.Reason; }
        if (GmuR1CanFollow(S1)) { R1->Checks=1; R1->Cp.Status=GmuR1Submit(S,Params,Cc,T,R1); }
        else { R1->Cp.Reason=8; R1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (D1!=NULL)
    {
        if (R1!=NULL) { D1->RChecks=R1->Checks; D1->RReason=R1->Cp.Reason; }
        if (GmuD1CanFollow(R1)) { D1->Checks=1; D1->Cp.Status=GmuD1Submit(S,Params,Cc,T,D1,Diag); }
        else { D1->Cp.Reason=8; D1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (V1!=NULL)
    {
        if (D1!=NULL) { V1->DChecks=D1->Checks; V1->DReason=D1->Cp.Reason; }
        if (GmuV1CanFollow(D1)) { V1->Checks=1; V1->Cp.Status=GmuV1Submit(S,Params,Cc,T,V1,Diag,D1); }
        else { V1->Cp.Reason=8; V1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (A1!=NULL)
    {
        if (V1!=NULL) { A1->VChecks=V1->Checks; A1->VReason=V1->Cp.Reason; }
        if (GmuA1CanFollow(V1)) { A1->Checks=1; A1->Cp.Status=GmuA1Submit(S,Params,Cc,T,A1,Diag,D1); }
        else { A1->Cp.Reason=8; A1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (T1!=NULL)
    {
        if (A1!=NULL) { T1->AChecks=A1->Checks; T1->AReason=A1->Cp.Reason; }
        if (GmuT1CanFollow(A1)) { T1->Checks=1; T1->Cp.Status=GmuT1Submit(S,Params,Cc,T,T1,Diag,D1); }
        else { T1->Cp.Reason=8; T1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (W1!=NULL)
    {
        if (T1!=NULL) { W1->TChecks=T1->Checks; W1->TReason=T1->Cp.Reason; }
        if (G1!=NULL) { W1->RbCntlG1=G1->After[4]; }
        if (GmuW1CanFollow(T1)) { W1->Cp.Status=GmuW1Submit(S,Params,Cc,T,W1,Diag,D1); }
        else { W1->Cp.Reason=8; W1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (M1!=NULL)
    {
        if (W1!=NULL) { M1->WChecks=W1->Checks; M1->WReason=W1->Cp.Reason; }
        if (GmuM1CanFollow(W1)) { M1->Cp.Status=GmuM1Submit(S,Params,Cc,T,M1,Diag,D1); }
        else { M1->Cp.Reason=8; M1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (Q1!=NULL)
    {
        if (M1!=NULL) { Q1->MChecks=M1->Checks; Q1->MReason=M1->Cp.Reason; }
        if (GmuQ1CanFollow(M1)) { Q1->Cp.Status=GmuQ1Submit(S,Params,Cc,T,Q1,Diag,D1); }
        else { Q1->Cp.Reason=8; Q1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (H1!=NULL && H1Work!=NULL)
    {
        if (Q1!=NULL) { H1->QChecks=Q1->Checks; H1->QReason=Q1->Cp.Reason; }
        if (GmuH1CanFollow(Q1)) { H1->Cp.Status=GmuH1Submit(S,Params,Cc,T,H1,H1Work,Diag,D1); }
        else { H1->Cp.Reason=8; H1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    if (B1!=NULL && H1Work!=NULL)
    {
        if (H1!=NULL) { B1->HChecks=H1->Checks; B1->HReason=H1->Cp.Reason; }
        if (GmuB1CanFollow(H1)) { B1->Cp.Status=GmuB1Submit(S,Params,Cc,T,B1,H1Work,Diag,D1,H1); }
        else { B1->Cp.Reason=8; B1->Cp.Status=STATUS_INVALID_DEVICE_STATE; }
    }
    GmuGxStep(Params,0x7C4,T);
    WRITE_REGISTER_ULONG(GmuReg(S,R_HOST2GMU_INTR_SET),0x80000000UL);
    return NT_SUCCESS(T->OobStatus) ? T->BreadcrumbStatus : T->OobStatus;
}
static NTSTATUS GmuGxSlumber(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc,
                             GMU_GX_TRACE* T, GMU_HFI_TRACE* Hfi)
{
    GmuGxStep(Params,0x7D0,T);
    T->IdleStatus=GmuGxWait(GmuReg(S,R_GMU_RPMH_POWER_STATE),0xFFFFFFFFUL,0,
                              100,100,&T->IdlePolls,&T->IdleLast);
    volatile ULONG* halt=static_cast<volatile ULONG*>(S->Maps[6])+0x45;
    volatile ULONG* ack=static_cast<volatile ULONG*>(S->Maps[6])+0x46;
    T->HaltStatus=STATUS_SUCCESS;
    NTSTATUS st=GmuGxStep(Params,0x7D1,T);
    if (NT_SUCCESS(st))
    {
        WRITE_REGISTER_ULONG(halt,1);
        GmuGxStep(Params,0x7D2,T);
        st=GmuGxWait(ack,1,1,10,1000,&T->HaltPolls[0],&T->HaltAck[0]);
    }
    if (!NT_SUCCESS(st)) { T->HaltStatus=st; }
    st=GmuGxStep(Params,0x7D3,T);
    if (NT_SUCCESS(st))
    {
        WRITE_REGISTER_ULONG(halt,2);
        st=GmuGxWait(ack,2,2,10,1000,&T->HaltPolls[1],&T->HaltAck[1]);
    }
    if (!NT_SUCCESS(st) && NT_SUCCESS(T->HaltStatus)) { T->HaltStatus=st; }
    GmuGxStep(Params,0x7D4,T); WRITE_REGISTER_ULONG(halt,0);
    st=GmuGxStep(Params,0x7D5,T);
    if (NT_SUCCESS(st))
    {
        if (!T->PowerCounterSaved) { T->PowerCounterOriginal=READ_REGISTER_ULONG(GmuReg(S,0x1F840)); }
        T->PowerCounterSaved=1; WRITE_REGISTER_ULONG(GmuReg(S,0x1F840),0);
    }
    ULONG slumber[3]={};
    T->SlumberStatus=GmuHfiExchange(S,Params,slumber,RTL_NUMBER_OF(slumber),33,6,0x7E0,Hfi,&Hfi->Messages[5]);
    Hfi->Messages[5].Status=T->SlumberStatus;
    // Continue every shutdown phase regardless of halt / slumber / busy failures.
    GmuGxStep(Params,0x7F0,T);
    WRITE_REGISTER_ULONG(GmuReg(S,0x23B10),0);
    WRITE_REGISTER_ULONG(GmuReg(S,R_CX_FALNEXT_INTF),1);
    GmuGxStep(Params,0x7F1,T);
    T->BusyStatus=GmuGxWait(GmuReg(S,0x23B0C),1UL<<23,0,100,100,&T->BusyPolls,&T->BusyLast);
    GmuGxStep(Params,0x7F2,T);
    KeMemoryBarrier();
    for (ULONG q=0;q<2;++q)
    {
        volatile ULONG* h=GmuHfiHeader(S,q);
        T->QueueEmpty[q]=h[10]==h[11] ? 1 : 0;
    }
    GmuHfiQueues(S,Hfi,7); GmuGxSnap(S,Cc,T,2);
    return T->SlumberStatus;
}
static BOOLEAN GmuGxRpmhStop(GMU_TEST* S, HANDLE Params, GMU_GX_TRACE* T)
{
    BOOLEAN issued=FALSE;
    NTSTATUS st=GmuGxStep(Params,0x7F4,T);
    if (NT_SUCCESS(st))
    {
        WRITE_REGISTER_ULONG(GmuReg(S,0x23B07),1);
        issued=TRUE;
        GmuGxStep(Params,0x7F5,T);
        st=GmuGxWait(static_cast<volatile ULONG*>(S->Maps[5])+0x004,1UL<<16,1UL<<16,
                      100,100,&T->RpmhStopPolls,&T->RpmhStopLast);
    }
    T->RpmhStopStatus=st;
    GmuGxStep(Params,0x7F6,T); WRITE_REGISTER_ULONG(GmuReg(S,0x23B07),0);
    return issued;
}
static BOOLEAN GmuCopy(GMU_TEST* S, HANDLE Params, const UCHAR* Fw, ULONG Length)
{
    ULONG mismatch=0;
    for (ULONG off=0; off<Length;)
    {
        EPI_GMU_FBLK b; RtlCopyMemory(&b,Fw+off,sizeof(b));
        const UCHAR* data=Fw+off+sizeof(b);
        if (b.Size != 0)
        {
            volatile ULONG* dst=NULL;
            if (GmuRange(b.Addr,b.Size,EPI_GMU_ITCM_BASE,EPI_GMU_TCM_SIZE))
            { dst=static_cast<volatile ULONG*>(S->Maps[3])+(b.Addr-EPI_GMU_ITCM_BASE)/4; }
            else if (GmuRange(b.Addr,b.Size,EPI_GMU_DTCM_BASE,EPI_GMU_TCM_SIZE))
            { dst=static_cast<volatile ULONG*>(S->Maps[4])+(b.Addr-EPI_GMU_DTCM_BASE)/4; }
            if (dst != NULL)
            {
                for (ULONG i=0; i<b.Size/4; ++i)
                {
                    ULONG v; RtlCopyMemory(&v,data+i*4,4);
                    // Persist the exact target dword before each TCM write.
                    if (!NT_SUCCESS(GmuStep(Params,0x80000000UL|((b.Addr+i*4)&0x1FFFFFFFUL)))) { return FALSE; }
                    WRITE_REGISTER_ULONG(dst+i,v);
                    if (READ_REGISTER_ULONG(dst+i)!=v) { ++mismatch; }
                }
            }
            else
            {
                GMU_REGION* r=GmuFind(S,b.Addr,b.Size);
                RtlCopyMemory(static_cast<UCHAR*>(r->Va)+b.Addr-r->Iova,data,b.Size);
            }
        }
        off+=sizeof(b)+b.Size;
    }
    KeMemoryBarrier(); return mismatch==0;
}

struct GMU_BOOT_SETTING { ULONG Reg, Original, Value, Readback, CmpMask; };
struct GMU_BOOT_RSCC_SETTING { ULONG Dword, Original, Value, Readback, Restored; };
static const GMU_BOOT_SETTING GmuBootSetup[] = {
        {0x23B06,0,0xFFFFFFFF,0xFFFFFFFF,0x3F}, // only six implemented IRQ mask bits
        {0x1F993,0,0xFFFFFFFF,0xFFFFFFFF,0}, // GMU2HOST_INTR_MASK: readback only until implemented bits are measured
        {R_CX_FALNEXT_INTF,0,1,0xFFFFFFFF,0xFFFFFFFF},
        {R_CX_FAL_INTF,0,1,0xFFFFFFFF,0xFFFFFFFF}, {R_GENERAL_7,0,1,0xFFFFFFFF,0xFFFFFFFF},
        {R_CM3_FW_INIT_RESULT,0,0,0xFFFFFFFF,0xFFFFFFFF}, {R_CM3_BOOT_CONFIG,0,2,0xFFFFFFFF,0xFFFFFFFF},
        {R_HFI_QTBL_ADDR,0,EPI_GMU_HFI_IOVA,0xFFFFFFFF,0xFFFFFFFF}, {R_HFI_QTBL_INFO,0,1,0xFFFFFFFF,0xFFFFFFFF},
        {R_AHB_FENCE_RANGE_0,0,V_AHB_FENCE_RANGE_0,0xFFFFFFFF,0xFFFFFFFF}, {R_CM3_CFG,0,V_CM3_CFG,0xFFFFFFFF,0xFFFFFFFF},
        {R_HFI_SFR_ADDR,0,V_GMU_CHIPID,0xFFFFFFFF,0xFFFFFFFF}, {R_PWR_COL_CP_MSG,0,EPI_GMU_LOG_IOVA|3,0xFFFFFFFF,0xFFFFFFFF},
        {R_SYS_BUS_CONFIG,0,1,0xFFFFFFFF,0xFFFFFFFF}, {R_ICACHE_CONFIG,0,1,0xFFFFFFFF,0xFFFFFFFF},
        {R_DCACHE_CONFIG,0,1,0xFFFFFFFF,0xFFFFFFFF},
        {R_PWR_COL_IFRAME_CTRL,0,V_IFRAME_CTRL,0xFFFFFFFF,0xFFFFFFFF}, {R_PWR_COL_CP_RESP,0,0,0xFFFFFFFF,0xFFFFFFFF},
        {R_GMU_RPMH_CTRL,0,0xF11,0xFFFFFFFF,0xFFFFFFFF}
    };
static const GMU_BOOT_RSCC_SETTING GmuBootRscc[] = {
        {0x004,0,0x01000000,0xFFFFFFFF,0xFFFFFFFF},
        {0x00E,0,0,0xFFFFFFFF,0xFFFFFFFF}, {0x010,0,0,0xFFFFFFFF,0xFFFFFFFF},
        {0x012,0,0x80000000,0xFFFFFFFF,0xFFFFFFFF}, {0x100,0,0,0xFFFFFFFF,0xFFFFFFFF},
        {0x180,0,0xEAAAE5A0,0xFFFFFFFF,0xFFFFFFFF}, {0x181,0,0xE1A1EBAB,0xFFFFFFFF,0xFFFFFFFF},
        {0x182,0,0xA2E0A581,0xFFFFFFFF,0xFFFFFFFF}, {0x183,0,0xECAC82E2,0xFFFFFFFF,0xFFFFFFFF},
        {0x184,0,0x0020EDAD,0xFFFFFFFF,0xFFFFFFFF}
    };
// CPU-only diagnostics: no pointer from GMU_TEST or hardware may reference this block.
struct GMU_BOOT_TRACES {
    GMU_GX_REG_TRACE gxReg;
    GMU_GX_HALT_TRACE gxHalt;
    GMU_GX_INIT_TRACE gxInit;
    GMU_GX_HWCG_TRACE gxHwcg;
    GMU_GX_CFG_TRACE gxCfg;
    GMU_GX_STAGE_E_TRACE gxStageE;
    GMU_GX_STAGE_F_TRACE gxStageF;
    GMU_G1_TRACE g1;
    GMU_G2_TRACE g2;
    GMU_G3_TRACE g3;
    GMU_G3B_TRACE g3b;
    GMU_G3C_TRACE g3c;
    GMU_G4_TRACE g4;
    GMU_G5_TRACE g5;
    GMU_G5B_TRACE g5b;
    GMU_Z1_TRACE z1;
    GMU_S1_TRACE s1;
    GMU_R1_TRACE r1;
    GMU_D1_TRACE d1;
    GMU_V1_TRACE v1;
    GMU_A1_TRACE a1;
    GMU_T1_TRACE t1;
    GMU_W1_TRACE w1;
    GMU_M1_TRACE m1;
    GMU_Q1_TRACE q1;
    GMU_H1_TRACE h1;
    GMU_B1_TRACE b1;
    GMU_H1_WORK h1Work;
    GMU_GX_TRACE gx;
    GMU_CLOCK_TRACE clocks;
    GMU_HFI_TRACE hfi;
    GMU_GBIF_TRACE gbif;
    GMU_BOOT_SETTING setup[RTL_NUMBER_OF(GmuBootSetup)];
    GMU_BOOT_RSCC_SETTING rscc[RTL_NUMBER_OF(GmuBootRscc)];
};
static VOID GmuBootTracesInit(GMU_BOOT_TRACES* p,BOOLEAN VoteGx,const ULONG* Disp,BOOLEAN W1)
{
    RtlFillMemory(&p->gxReg,sizeof(p->gxReg),0xFF);
    p->gxReg.Attempted=0; p->gxReg.Reason=GX_REG_NOT_REACHED; p->gxReg.Count=0;
    RtlZeroMemory(&p->gxHalt,sizeof(p->gxHalt)); p->gxHalt.Reason=GX_REG_NOT_REACHED;
    p->gxHalt.Gdscr=0xFFFFFFFF; p->gxHalt.GxHm=0xFFFFFFFF;
    p->gxHalt.Status=STATUS_NOT_FOUND; p->gxHalt.CxIdleStatus=STATUS_NOT_FOUND; p->gxHalt.CxPolls=0xFFFFFFFF;
    for (ULONG i=0;i<4;++i) { p->gxHalt.Before[i]=0xFFFFFFFF; p->gxHalt.After[i]=0xFFFFFFFF; }
    p->gxHalt.FinalCx[0]=0xFFFFFFFF; p->gxHalt.FinalCx[1]=0xFFFFFFFF;
    GmuGxInitTraceInit(&p->gxInit);
    GmuGxHwcgTraceInit(&p->gxHwcg);
    GmuGxCfgTraceInit(&p->gxCfg);
    GmuGxStageETraceInit(&p->gxStageE);
    GmuGxStageFTraceInit(&p->gxStageF);
    GmuG1TraceInit(&p->g1,W1);
    GmuG2TraceInit(&p->g2);
    GmuG3TraceInit(&p->g3);
    GmuG3bTraceInit(&p->g3b);
    GmuG3cTraceInit(&p->g3c);
    GmuG4TraceInit(&p->g4);
    GmuG5TraceInit(&p->g5);
    GmuG5bTraceInit(&p->g5b);
    GmuZ1TraceInit(&p->z1);
    GmuS1TraceInit(&p->s1);
    GmuR1TraceInit(&p->r1);
    GmuD1TraceInit(&p->d1, Disp);
    GmuV1TraceInit(&p->v1);
    GmuA1TraceInit(&p->a1);
    GmuT1TraceInit(&p->t1);
    GmuW1TraceInit(&p->w1);
    GmuM1TraceInit(&p->m1);
    GmuQ1TraceInit(&p->q1);
    GmuH1TraceInit(&p->h1);
    GmuB1TraceInit(&p->b1);
    RtlZeroMemory(&p->h1Work,sizeof(p->h1Work)); p->h1Work.Completed=0xFFFFFFFF;
    RtlZeroMemory(&p->gx,sizeof(p->gx));
    p->gx.OobStatus=STATUS_NOT_FOUND; p->gx.HaltStatus=STATUS_NOT_FOUND; p->gx.SlumberStatus=STATUS_NOT_FOUND;
    p->gx.BusyStatus=STATUS_NOT_FOUND; p->gx.RpmhStopStatus=STATUS_NOT_FOUND; p->gx.IdleStatus=STATUS_NOT_FOUND;
    p->gx.PowerCounterRestoreStatus=STATUS_NOT_FOUND; p->gx.BreadcrumbStatus=STATUS_SUCCESS;
    p->gx.OobPolls=0xFFFFFFFF; p->gx.OobInfo=0xFFFFFFFF; p->gx.BusyPolls=0xFFFFFFFF;
    p->gx.PowerCounterOriginal=0xFFFFFFFF; p->gx.RpmhStopPolls=0xFFFFFFFF; p->gx.IdlePolls=0xFFFFFFFF;
    p->gx.IdleLast=0xFFFFFFFF; p->gx.BusyLast=0xFFFFFFFF; p->gx.RpmhStopLast=0xFFFFFFFF;
    for (ULONG i=0;i<2;++i)
    { p->gx.HaltPolls[i]=0xFFFFFFFF; p->gx.HaltAck[i]=0xFFFFFFFF; p->gx.QueueEmpty[i]=0xFFFFFFFF; }
    for (ULONG i=0;i<RTL_NUMBER_OF(p->gx.Snap);++i)
    {
        p->gx.Snap[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(p->gx.Snap[i]);++j) { p->gx.Snap[i][j]=0xFFFFFFFF; }
    }
    RtlZeroMemory(&p->clocks,sizeof(p->clocks)); p->clocks.Status=STATUS_NOT_FOUND; p->clocks.RestoreStatus=STATUS_NOT_FOUND;
    for (ULONG i=0;i<6;++i) { p->clocks.Gcc[i]=0xFFFFFFFF; }
    for (ULONG i=0;i<RTL_NUMBER_OF(p->clocks.Rcgs);++i)
    {
        GMU_CLOCK_RCG* r=&p->clocks.Rcgs[i];
        r->CmdOffset=i==0 ? 0x1120 : 0x117C; r->CfgOffset=r->CmdOffset+4;
        r->TargetCfg=i==0 ? 0x602 : 0x603;
        r->OriginalCmd=0xFFFFFFFF; r->OriginalCfg=0xFFFFFFFF;
        r->SetCmd=0xFFFFFFFF; r->SetCfg=0xFFFFFFFF; r->SetPolls=0xFFFFFFFF;
        r->RestoreCmd=0xFFFFFFFF; r->RestoreCfg=0xFFFFFFFF;
        r->RestorePolls=0xFFFFFFFF; r->PendingPolls=0xFFFFFFFF;
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(p->clocks.Branches);++i)
    { for (ULONG j=0;j<RTL_NUMBER_OF(p->clocks.Branches[i]);++j) { p->clocks.Branches[i][j]=0xFFFFFFFF; } }
    RtlZeroMemory(&p->hfi,sizeof(p->hfi)); p->hfi.Status=STATUS_NOT_FOUND; p->hfi.CtrlStatus=0xFFFFFFFF;
    p->hfi.ExtraEnabled=VoteGx; p->hfi.ExtraStatus=STATUS_NOT_FOUND;
    RtlZeroMemory(&p->gbif,sizeof(p->gbif)); p->gbif.Status=STATUS_NOT_FOUND; p->gbif.RestoreStatus=STATUS_NOT_FOUND;
    p->gbif.BadIndex=0xFFFFFFFF; p->gbif.RestoreBadIndex=0xFFFFFFFF;
    for (ULONG i=0;i<RTL_NUMBER_OF(p->gbif.Entries);++i)
    {
        p->gbif.Entries[i].Reg=0x3C03+i; p->gbif.Entries[i].Value=0x00071620;
        p->gbif.Entries[i].Original=0xFFFFFFFF; p->gbif.Entries[i].Readback=0xFFFFFFFF; p->gbif.Entries[i].Restored=0xFFFFFFFF;
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(p->hfi.Messages);++i) { p->hfi.Messages[i].Status=STATUS_NOT_FOUND; }
    for (ULONG i=0;i<RTL_NUMBER_OF(p->hfi.Queues);++i)
    {
        p->hfi.Queues[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(p->hfi.Queues[i]);++j) { p->hfi.Queues[i][j]=0xFFFFFFFF; }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(p->hfi.Power);++i)
    {
        p->hfi.Power[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(p->hfi.Power[i]);++j) { p->hfi.Power[i][j]=0xFFFFFFFF; }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(p->hfi.Rscc);++i)
    {
        p->hfi.Rscc[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(p->hfi.Rscc[i]);++j) { p->hfi.Rscc[i][j]=0xFFFFFFFF; }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(p->hfi.Extra);++i)
    {
        p->hfi.Extra[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(p->hfi.Extra[i]);++j) { p->hfi.Extra[i][j]=0xFFFFFFFF; }
    }
    RtlCopyMemory(p->setup,GmuBootSetup,sizeof(p->setup));
    RtlCopyMemory(p->rscc,GmuBootRscc,sizeof(p->rscc));
}
ULONG EpiGmuBootTest(HANDLE Params, HANDLE Diag, volatile ULONG* Cc, const UCHAR* Fw, ULONG FwLen,
                     PWCHAR Text, SIZE_T TextCch, BOOLEAN* KeepCx, BOOLEAN StartHfi, BOOLEAN VoteGx, BOOLEAN FastClock,
                     BOOLEAN GxOn, BOOLEAN GxReg, BOOLEAN ClearHalt, BOOLEAN Secvid, BOOLEAN AddrMode, BOOLEAN Hwcg,
                     BOOLEAN GxCfg, BOOLEAN StageE, BOOLEAN StageF, BOOLEAN G1, const UCHAR* Sqe, ULONG SqeLen,
                     BOOLEAN G2, BOOLEAN G3, BOOLEAN G3b, BOOLEAN G3c, BOOLEAN G4, BOOLEAN G5, BOOLEAN G5b,
                     BOOLEAN Z1,const UCHAR* Zap,ULONG ZapLen,BOOLEAN S1,BOOLEAN R1,BOOLEAN D1,const ULONG* Disp,BOOLEAN V1,BOOLEAN A1,BOOLEAN T1,BOOLEAN W1,BOOLEAN M1,BOOLEAN Q1,BOOLEAN H1,BOOLEAN B1)
{
    PAGED_CODE();
    if (KeepCx == NULL) { return EPI_GMU_E_PRECOND; }
    *KeepCx=FALSE;
    if (g_KeptGmu != NULL) { *KeepCx=TRUE; return EPI_GMU_E_TEARDOWN; }
    if (B1 && !H1) { return EPI_GMU_E_PRECOND; }
    if (H1 && !Q1) { return EPI_GMU_E_PRECOND; }
    if (Q1 && !M1) { return EPI_GMU_E_PRECOND; }
    if (M1 && !W1) { return EPI_GMU_E_PRECOND; }
    if (W1 && !T1) { return EPI_GMU_E_PRECOND; }
    if (T1 && !A1) { return EPI_GMU_E_PRECOND; }
    if (A1 && !V1) { return EPI_GMU_E_PRECOND; }
    if (V1 && !D1) { return EPI_GMU_E_PRECOND; }
    if (D1 && (!R1 || !EpiGmuD1DisplayMatches(Disp))) { return EPI_GMU_E_PRECOND; }
    if (VoteGx && !StartHfi) { return EPI_GMU_E_PRECOND; }
    if (FastClock && (!VoteGx || !StartHfi)) { return EPI_GMU_E_PRECOND; }
    if (GxOn && (!FastClock || !VoteGx || !StartHfi)) { return EPI_GMU_E_PRECOND; }
    if (GxReg && !GxOn) { return EPI_GMU_E_PRECOND; }
    if (ClearHalt && !GxReg) { return EPI_GMU_E_PRECOND; }
    if (Secvid && !ClearHalt) { return EPI_GMU_E_PRECOND; }
    if (AddrMode && !Secvid) { return EPI_GMU_E_PRECOND; }
    if (Hwcg && !AddrMode) { return EPI_GMU_E_PRECOND; }
    if (GxCfg && !Hwcg) { return EPI_GMU_E_PRECOND; }
    if (StageE && !GxCfg) { return EPI_GMU_E_PRECOND; }
    if (StageF && !StageE) { return EPI_GMU_E_PRECOND; }
    if (G1 && !StageF) { return EPI_GMU_E_PRECOND; }
    if (G2 && !G1) { return EPI_GMU_E_PRECOND; }
    if (G3 && !G2) { return EPI_GMU_E_PRECOND; }
    if (G3b && !G3) { return EPI_GMU_E_PRECOND; }
    if (G3c && !G3b) { return EPI_GMU_E_PRECOND; }
    if (G4 && !G3c) { return EPI_GMU_E_PRECOND; }
    if (G5 && !G4) { return EPI_GMU_E_PRECOND; }
    if (G5b && !G5) { return EPI_GMU_E_PRECOND; }
    if (Z1 && (!G5b || !EpiZapValidate(Zap,ZapLen))) { return EPI_GMU_E_PRECOND; }
    if (S1 && !Z1) { return EPI_GMU_E_PRECOND; }
    if (R1 && !S1) { return EPI_GMU_E_PRECOND; }
    if (Cc == NULL || Text == NULL || TextCch==0) { return EPI_GMU_E_PRECOND; }
    GMU_BOOT_TRACES* p=static_cast<GMU_BOOT_TRACES*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(GMU_BOOT_TRACES),EPI_GMU_TAG));
    if (p==NULL) { return EPI_GMU_E_NOMEM; }
    GmuBootTracesInit(p,VoteGx,Disp,W1);
    EpiSetDword(Diag,L"GpuGmuKeepCx",0);
    EpiSetDword(Diag,L"GpuGmuFaultStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetDword(Diag,L"GpuGmuDisableStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetDword(Diag,L"GpuGmuBadIndex",0xFFFFFFFFUL);
    EpiSetValue(Diag,L"GpuGmuSetup",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuSmmuBadWrite",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuSmmuDisableWrites",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuSmmuCbar",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuRsccEntry",REG_BINARY,NULL,0);
    EpiSetDword(Diag,L"GpuGmuRsccBadIndex",0xFFFFFFFFUL);
    EpiSetDword(Diag,L"GpuGmuRsccRestoreBadIndex",0xFFFFFFFFUL);
    EpiSetDword(Diag,L"GpuGmuRsccRestoreStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetDword(Diag,L"GpuGmuHfiStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetDword(Diag,L"GpuGmuHfiCtrlStatus",0xFFFFFFFF);
    EpiSetDword(Diag,L"GpuGmuHfiBwPublished",0);
    EpiSetDword(Diag,L"GpuGmuHfiIdleStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetValue(Diag,L"GpuGmuHfiMessages",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuHfiQueues",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuHfiPower",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuHfiRscc",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuHfiExtra",REG_BINARY,NULL,0);
    EpiSetDword(Diag,L"GpuGmuHfiExtraStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetValue(Diag,L"GpuGmuGbifSetup",REG_BINARY,NULL,0);
    EpiSetDword(Diag,L"GpuGmuGbifStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetDword(Diag,L"GpuGmuGbifRestoreStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetDword(Diag,L"GpuGmuGbifBadIndex",0xFFFFFFFF);
    EpiSetDword(Diag,L"GpuGmuGbifRestoreBadIndex",0xFFFFFFFF);
    EpiSetDword(Diag,L"GpuGmuClockStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetDword(Diag,L"GpuGmuClockRestoreStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetValue(Diag,L"GpuGmuClockGcc",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuClockRcgs",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuClockBranches",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuGxReg",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuGxHalt",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuGxInit",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuGxHwcg",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuGxCfg",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuGxStageE",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuGxStageF",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuG1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuG2",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuG3",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuG3b",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuG3c",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuG4",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuG5",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuG5b",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuZ1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuZ1Pas",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuS1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuR1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuD1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuV1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuA1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuT1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuW1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuM1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuQ1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuH1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuB1",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuSmmuGpuStreams",REG_BINARY,NULL,0);
    GMU_TEST* s=static_cast<GMU_TEST*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(GMU_TEST),EPI_GMU_TAG));
    if (s==NULL) { ExFreePoolWithTag(p,EPI_GMU_TAG); return EPI_GMU_E_NOMEM; }
    for (ULONG i=0;i<RTL_NUMBER_OF(s->SmmuCbar);++i)
    {
        s->SmmuCbar[i][0]=i; s->SmmuCbar[i][1]=static_cast<ULONG>(STATUS_NOT_FOUND);
        s->SmmuCbar[i][2]=0xFFFFFFFF;
    }
    ULONG result=EPI_GMU_OK, configured=0, rsccConfigured=0;
    NTSTATUS st=STATUS_SUCCESS;
    BOOLEAN smmuAttempted=FALSE, released=FALSE, rsccSaved=FALSE, rpmhStopIssued=FALSE;
    const ULONGLONG bases[]={EPI_GMU_PAGE_C_PHYS,EPI_GMU_PAGE_A_PHYS,EPI_GMU_PAGE_B_PHYS,
                             EPI_GMU_ITCM_PHYS_,EPI_GMU_DTCM_PHYS_,EPI_GMU_RSCC_PHYS};
    if (!GmuPlan(s,Fw,FwLen)) { result=EPI_GMU_E_FW; goto cleanup; }
    if (G1)
    {
        st=GmuStep(Params,0xC00);
        if (NT_SUCCESS(st)) { st=GmuG1Plan(s,Sqe,SqeLen); }
        if (!NT_SUCCESS(st)) { p->g1.MemoryStatus=st; result=EPI_GMU_E_GPU_MEMORY; goto cleanup; }
    }
    if (H1 && !GmuH1Plan(s)) { result=EPI_GMU_E_GPU_MEMORY; st=STATUS_INSUFFICIENT_RESOURCES; goto cleanup; }
    if (D1 && !GmuD1Plan(s)) { result=EPI_GMU_E_GPU_MEMORY; st=STATUS_INSUFFICIENT_RESOURCES; goto cleanup; }
    // Confirm the CX window before accessing any GMU register.
    {
        const ULONG branchOffsets[]={0x1078,0x109C,0x1098,0x1204,0x5000};
        if (!(READ_REGISTER_ULONG(Cc+0x106C/4)&0x80000000UL) ||
            (READ_REGISTER_ULONG(Cc+0x100C/4)&0x80000001UL)!=1) { result=EPI_GMU_E_PRECOND; goto cleanup; }
        for (ULONG i=0;i<RTL_NUMBER_OF(branchOffsets);++i)
        { if (READ_REGISTER_ULONG(Cc+branchOffsets[i]/4)&0x80000000UL) { result=EPI_GMU_E_PRECOND; goto cleanup; } }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(bases);++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=bases[i];
        s->Maps[i]=MmMapIoSpaceEx(pa,GmuMapLength(i),PAGE_READWRITE|PAGE_NOCACHE);
        if (s->Maps[i]==NULL) { result=EPI_GMU_E_NOMEM; goto cleanup; }
    }
    if (READ_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET))!=1)
    { result=EPI_GMU_E_TEARDOWN; goto keep; } // never collapse a pre-existing running GMU
    if ((READ_REGISTER_ULONG(GmuReg(s,0x1F8D0))&0xC0)!=0xC0)
    { result=EPI_GMU_E_PRECOND; goto cleanup; }
    if (VoteGx)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=EPI_GMU_GBIF_PHYS;
        s->Maps[6]=MmMapIoSpaceEx(pa,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
        pa.QuadPart=EPI_GMU_GCC_VOTE_PHYS;
        s->Maps[7]=MmMapIoSpaceEx(pa,PAGE_SIZE,(FastClock ? PAGE_READWRITE : PAGE_READONLY)|PAGE_NOCACHE);
        if (s->Maps[6]==NULL || s->Maps[7]==NULL) { result=EPI_GMU_E_NOMEM; goto cleanup; }
    }
    for (ULONG i=0;i<s->Count;++i)
    {
        GMU_REGION* r=&s->Regions[i];
        if (r->External) { continue; }
        PHYSICAL_ADDRESS low,high,boundary; low.QuadPart=0; high.QuadPart=(1ULL<<36)-1; boundary.QuadPart=0;
        r->Va=MmAllocateContiguousMemorySpecifyCache(r->Size,low,high,boundary,MmNonCached);
        if (r->Va==NULL)
        { st=STATUS_INSUFFICIENT_RESOURCES; if (G1) { p->g1.MemoryStatus=st; } result=EPI_GMU_E_NOMEM; goto cleanup; }
        r->Pa=MmGetPhysicalAddress(r->Va); RtlZeroMemory(r->Va,r->Size);
    }
    if (G1)
    {
        st=GmuG1Copy(s,Params,Sqe,&p->g1);
        if (!NT_SUCCESS(st)) { p->g1.MemoryStatus=st; result=EPI_GMU_E_GPU_MEMORY; goto cleanup; }
    }
    st=EpiSmmuCreate(Params,&s->Smmu);
    if (!NT_SUCCESS(st)) { if (G1) { p->g1.MemoryStatus=st; } result=EPI_GMU_E_SMMU_MAP; goto cleanup; }
    if (G1)
    {
        st=EpiSmmuConfigureGpuStreams(s->Smmu);
        if (!NT_SUCCESS(st)) { p->g1.MemoryStatus=st; result=EPI_GMU_E_SMMU_MAP; goto cleanup; }
    }
    for (ULONG i=0;i<s->Count;++i)
    {
        GMU_REGION* r=&s->Regions[i];
        st=EpiSmmuMap(s->Smmu,r->Iova,r->Pa,r->Size,r->Flags);
        if (!NT_SUCCESS(st)) { if (G1) { p->g1.MemoryStatus=st; } result=EPI_GMU_E_SMMU_MAP; goto cleanup; }
        r->Mapped=TRUE;
    }
    if (G1) { p->g1.MemoryStatus=STATUS_SUCCESS; GmuG1Diag(s,Diag,&p->g1); }
    GmuQueues(s);
    // Snapshot all controls before writing any. IRQ masks remain masked until CX off.
    for (ULONG i=0;i<RTL_NUMBER_OF(p->setup);++i) { p->setup[i].Original=READ_REGISTER_ULONG(GmuReg(s,p->setup[i].Reg)); }
    st=GmuStep(Params,0x680);
    if (!NT_SUCCESS(st)) { result=EPI_GMU_E_RSCC; goto cleanup; }
    {
        // Cold-start probes only: do not assume CX collapse erased RSCC sleep state, and do not wake it here.
        const ULONG entry[]={READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+0x004),
                             READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+0x101),
                             READ_REGISTER_ULONG(GmuReg(s,0x23B07)),READ_REGISTER_ULONG(GmuReg(s,0x23B08))};
        EpiSetValue(Diag,L"GpuGmuRsccEntry",REG_BINARY,entry,sizeof(entry));
        if ((entry[0]&(1UL<<16)) || entry[1]!=0 || entry[2]!=0 || entry[3]!=0)
        { st=STATUS_INVALID_DEVICE_STATE; result=EPI_GMU_E_PRECOND; goto cleanup; }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(p->rscc);++i)
    { p->rscc[i].Original=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+p->rscc[i].Dword); }
    rsccSaved=TRUE;
    for (ULONG i=0;i<RTL_NUMBER_OF(p->rscc);++i)
    {
        st=GmuStep(Params,0x690+i);
        if (NT_SUCCESS(st))
        {
            rsccConfigured=i+1;
            WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+p->rscc[i].Dword,p->rscc[i].Value);
            p->rscc[i].Readback=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+p->rscc[i].Dword);
            if (p->rscc[i].Readback!=p->rscc[i].Value) { st=STATUS_IO_DEVICE_ERROR; }
        }
        if (!NT_SUCCESS(st))
        { EpiSetDword(Diag,L"GpuGmuRsccBadIndex",i); result=EPI_GMU_E_RSCC; goto cleanup; }
    }
    EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,p->rscc,sizeof(p->rscc));
    for (ULONG i=0;i<RTL_NUMBER_OF(p->setup);++i)
    {
        configured=i+1;
        st=GmuWrite(s,Params,p->setup[i].Reg,p->setup[i].Value,0x600+i,p->setup[i].CmpMask,&p->setup[i].Readback);
        if (!NT_SUCCESS(st))
        {
            EpiSetDword(Diag,L"GpuGmuBadIndex",i);
            EpiSetValue(Diag,L"GpuGmuSetup",REG_BINARY,p->setup,sizeof(p->setup));
            result=EPI_GMU_E_TCM; goto cleanup;
        }
        if (VoteGx && p->setup[i].Reg==R_PWR_COL_CP_MSG)
        {
            // Log address is configured; apply CX-side GBIF before the remaining power controls.
            st=GmuGbifSetup(s,Params,Cc,&p->gbif); p->gbif.Status=st;
            GmuGbifDiag(Diag,&p->gbif);
            if (!NT_SUCCESS(st))
            {
                EpiSetValue(Diag,L"GpuGmuSetup",REG_BINARY,p->setup,sizeof(p->setup));
                result=EPI_GMU_E_GBIF; goto cleanup;
            }
        }
    }
    EpiSetValue(Diag,L"GpuGmuSetup",REG_BINARY,p->setup,sizeof(p->setup));
    if (!GmuCopy(s,Params,Fw,FwLen))
    {
        EpiSetDword(Diag,L"GpuGmuBadIndex",0xFF);
        st=STATUS_IO_DEVICE_ERROR; result=EPI_GMU_E_TCM; goto cleanup;
    }
    GmuText(Text,TextCch,L"gmu_fw_ver",READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[4])+EPI_GMU_VER_DTCM_OFF/4));
    if (FastClock)
    {
        st=GmuClockSetup(s,Params,Cc,&p->clocks); p->clocks.Status=st; GmuClockDiag(Diag,&p->clocks);
        if (!NT_SUCCESS(st)) { result=EPI_GMU_E_CLOCK; goto cleanup; }
    }
    smmuAttempted=TRUE;
    st=EpiSmmuEnable(s->Smmu,Cc,static_cast<volatile ULONG*>(s->Maps[1]));
    if (!NT_SUCCESS(st))
    {
        // GpuSmmuBadWrite: four little-endian ULONGs {Step, Offset (bytes), Value, Readback}.
        // Keep the Enable status in st even when the failure was not a write mismatch.
        ULONG badWrite[4]={};
        if (NT_SUCCESS(EpiSmmuQueryLastError(s->Smmu,&badWrite[0],&badWrite[1],&badWrite[2],&badWrite[3])))
        { EpiSetValue(Diag,L"GpuSmmuBadWrite",REG_BINARY,badWrite,sizeof(badWrite)); }
        GmuSmmuDisableDiag(s->Smmu,Diag); // Enable may have attempted an internal rollback
        result=EPI_GMU_E_SMMU_ENABLE; goto cleanup;
    }
    // No release unless SMMU and all buffers are ready; clear stale init result first.
    st=GmuStep(Params,0x620);
    if (!NT_SUCCESS(st)) { result=EPI_GMU_E_PRECOND; goto cleanup; }
    WRITE_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET),1);
    st=GmuWrite(s,Params,R_PWR_COL_CP_RESP,0,0x621);
    if (!NT_SUCCESS(st)) { result=EPI_GMU_E_PRECOND; goto cleanup; }
    st=GmuStep(Params,0x622);
    if (!NT_SUCCESS(st)) { result=EPI_GMU_E_PRECOND; goto cleanup; }
    released=TRUE; KeMemoryBarrier(); WRITE_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET),0);
    result=EPI_GMU_E_TIMEOUT;
    for (ULONG i=0;i<EPI_GMU_POLL_LOOPS;++i)
    {
        if ((READ_REGISTER_ULONG(GmuReg(s,R_CM3_FW_INIT_RESULT))&V_FW_INIT_MASK)==V_FW_INIT_DONE)
        { result=EPI_GMU_OK; break; }
        KeStallExecutionProcessor(100);
    }
    if (result==EPI_GMU_E_TIMEOUT) { st=STATUS_IO_TIMEOUT; }
    if (result==EPI_GMU_OK)
    {
        st=GmuHfiTables(s,Params,Cc,&p->hfi,StartHfi,VoteGx); p->hfi.Status=st;
        if (!NT_SUCCESS(st)) { result=EPI_GMU_E_HFI; }
        else if (GxOn)
        {
            const NTSTATUS oob=GmuGxOob(s,Params,Cc,&p->gx,GxReg ? &p->gxReg : NULL,
                                        ClearHalt ? &p->gxHalt : NULL,Secvid ? &p->gxInit : NULL,AddrMode,Hwcg ? &p->gxHwcg : NULL,
                                        GxCfg ? &p->gxCfg : NULL,StageE ? &p->gxStageE : NULL,StageF ? &p->gxStageF : NULL,
                                        G1 ? &p->g1 : NULL,G2 ? &p->g2 : NULL,G3 ? &p->g3 : NULL,G3b ? &p->g3b : NULL,
                                        G3c ? &p->g3c : NULL,G4 ? &p->g4 : NULL,G5 ? &p->g5 : NULL,G5b ? &p->g5b : NULL,
                                        Z1 ? &p->z1 : NULL,Diag,Zap,ZapLen,S1 ? &p->s1 : NULL,R1 ? &p->r1 : NULL,D1 ? &p->d1 : NULL,V1 ? &p->v1 : NULL,A1 ? &p->a1 : NULL,T1 ? &p->t1 : NULL,W1 ? &p->w1 : NULL,M1 ? &p->m1 : NULL,Q1 ? &p->q1 : NULL,H1 ? &p->h1 : NULL,H1 ? &p->h1Work : NULL,B1 ? &p->b1 : NULL);
            NTSTATUS slumber=STATUS_NOT_FOUND;
            if (p->gx.Attempted)
            {
                slumber=GmuGxSlumber(s,Params,Cc,&p->gx,&p->hfi);
                if (!NT_SUCCESS(slumber)) { p->hfi.Status=slumber; }
            }
            if (!NT_SUCCESS(oob) || (p->gx.Attempted && !NT_SUCCESS(slumber)) || !NT_SUCCESS(p->gx.BreadcrumbStatus) ||
                (ClearHalt && !NT_SUCCESS(p->gxHalt.Status)) || (Secvid && !NT_SUCCESS(p->gxInit.Status)) ||
                (Hwcg && !NT_SUCCESS(p->gxHwcg.Status)) || (GxCfg && !NT_SUCCESS(p->gxCfg.Status)) ||
                (StageE && !NT_SUCCESS(p->gxStageE.Status)) || (StageF && !NT_SUCCESS(p->gxStageF.Status)) ||
                (G1 && !NT_SUCCESS(p->g1.RegisterStatus)))
            {
                result=EPI_GMU_E_GX;
                st=!NT_SUCCESS(oob) ? oob : (!NT_SUCCESS(slumber) ? slumber :
                   (!NT_SUCCESS(p->gx.BreadcrumbStatus) ? p->gx.BreadcrumbStatus :
                    (ClearHalt && !NT_SUCCESS(p->gxHalt.Status) ? p->gxHalt.Status :
                     (Secvid && !NT_SUCCESS(p->gxInit.Status) ? p->gxInit.Status :
                      (Hwcg && !NT_SUCCESS(p->gxHwcg.Status) ? p->gxHwcg.Status :
                       (GxCfg && !NT_SUCCESS(p->gxCfg.Status) ? p->gxCfg.Status :
                        (StageE && !NT_SUCCESS(p->gxStageE.Status) ? p->gxStageE.Status :
                         (StageF && !NT_SUCCESS(p->gxStageF.Status) ? p->gxStageF.Status : p->g1.RegisterStatus))))))));
            }
            if (result==EPI_GMU_OK && G2 && !NT_SUCCESS(p->g2.Status))
            { result=EPI_GMU_E_CP; st=p->g2.Status; }
            if (result==EPI_GMU_OK && G3 && !NT_SUCCESS(p->g3.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->g3.Cp.Status; }
            if (result==EPI_GMU_OK && G3b && !NT_SUCCESS(p->g3b.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->g3b.Cp.Status; }
            if (result==EPI_GMU_OK && G3c && !NT_SUCCESS(p->g3c.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->g3c.Cp.Status; }
            if (result==EPI_GMU_OK && G4 && !NT_SUCCESS(p->g4.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->g4.Cp.Status; }
            if (result==EPI_GMU_OK && G5 && !NT_SUCCESS(p->g5.Cp.Status) && !(G5b && GmuG5bCanFollow(&p->g5)))
            { result=EPI_GMU_E_CP; st=p->g5.Cp.Status; }
            if (result==EPI_GMU_OK && G5b && !NT_SUCCESS(p->g5b.Draw.Cp.Status) && !(Z1 && GmuZ1CanFollow(&p->g5b)))
            { result=EPI_GMU_E_CP; st=p->g5b.Draw.Cp.Status; }
            if (result==EPI_GMU_OK && Z1 && !NT_SUCCESS(p->z1.Gpu.Draw.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->z1.Gpu.Draw.Cp.Status; }
            if (result==EPI_GMU_OK && S1 && !NT_SUCCESS(p->s1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->s1.Cp.Status; }
            if (result==EPI_GMU_OK && R1 && !NT_SUCCESS(p->r1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->r1.Cp.Status; }
            if (result==EPI_GMU_OK && D1 && !NT_SUCCESS(p->d1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->d1.Cp.Status; }
            if (result==EPI_GMU_OK && V1 && !NT_SUCCESS(p->v1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->v1.Cp.Status; }
            if (result==EPI_GMU_OK && A1 && !NT_SUCCESS(p->a1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->a1.Cp.Status; }
            if (result==EPI_GMU_OK && T1 && !NT_SUCCESS(p->t1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->t1.Cp.Status; }
            if (result==EPI_GMU_OK && W1 && !NT_SUCCESS(p->w1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->w1.Cp.Status; }
            if (result==EPI_GMU_OK && M1 && !NT_SUCCESS(p->m1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->m1.Cp.Status; }
            if (result==EPI_GMU_OK && Q1 && !NT_SUCCESS(p->q1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->q1.Cp.Status; }
            if (result==EPI_GMU_OK && H1 && !NT_SUCCESS(p->h1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->h1.Cp.Status; }
            if (result==EPI_GMU_OK && B1 && !NT_SUCCESS(p->b1.Cp.Status))
            { result=EPI_GMU_E_CP; st=p->b1.Cp.Status; }
        }
        GmuHfiPower(s,Params,Cc,&p->hfi,1);
        GmuText(Text,TextCch,L"gmu_hfi_status",static_cast<ULONG>(p->hfi.Status));
    }
    {
        const ULONG regs[]={R_CM3_FW_INIT_RESULT,R_CM3_FW_BUSY,R_GMU2HOST_INTR_INFO,R_AO_HOST_INTR,R_GENERAL_0,R_GENERAL_1};
        ULONG values[RTL_NUMBER_OF(regs)]={};
        for (ULONG i=0;i<RTL_NUMBER_OF(regs);++i) { values[i]=READ_REGISTER_ULONG(GmuReg(s,regs[i])); }
        EpiSetValue(Diag,L"GpuGmuRegs",REG_BINARY,values,sizeof(values));
        GmuText(Text,TextCch,L"gmu_fw_init",values[0]);
        ULONG fsr=0,syn=0; ULONGLONG faultAddress=0;
        NTSTATUS fault=EpiSmmuQueryFault(s->Smmu,&fsr,&faultAddress,&syn);
        EpiSetDword(Diag,L"GpuGmuFaultStatus",static_cast<ULONG>(fault));
        const ULONGLONG faults[]={fsr,faultAddress,syn};
        EpiSetValue(Diag,L"GpuGmuFault",REG_BINARY,faults,sizeof(faults));
        GMU_REGION* log=GmuFind(s,EPI_GMU_LOG_IOVA,EPI_GMU_LOG_SIZE);
        EpiSetValue(Diag,L"GpuGmuLog",REG_BINARY,log->Va,EPI_GMU_LOG_DUMP);
    }
cleanup:
    if (G3c) { EpiSetValue(Diag,L"GpuGmuG3c",REG_BINARY,&p->g3c,sizeof(p->g3c)); }
    if (G4) { EpiSetValue(Diag,L"GpuGmuG4",REG_BINARY,&p->g4,sizeof(p->g4)); }
    if (G5) { EpiSetValue(Diag,L"GpuGmuG5",REG_BINARY,&p->g5,sizeof(p->g5)); }
    if (G5b) { EpiSetValue(Diag,L"GpuGmuG5b",REG_BINARY,&p->g5b,sizeof(p->g5b)); }
    if (Z1) { EpiSetValue(Diag,L"GpuGmuZ1",REG_BINARY,&p->z1,sizeof(p->z1)); }
    if (S1) { EpiSetValue(Diag,L"GpuGmuS1",REG_BINARY,&p->s1,sizeof(p->s1)); }
    if (R1) { EpiSetValue(Diag,L"GpuGmuR1",REG_BINARY,&p->r1,sizeof(p->r1)); }
    if (D1) { EpiSetValue(Diag,L"GpuGmuD1",REG_BINARY,&p->d1,sizeof(p->d1)); }
    if (V1) { EpiSetValue(Diag,L"GpuGmuV1",REG_BINARY,&p->v1,sizeof(p->v1)); }
    if (A1) { EpiSetValue(Diag,L"GpuGmuA1",REG_BINARY,&p->a1,sizeof(p->a1)); }
    if (T1) { EpiSetValue(Diag,L"GpuGmuT1",REG_BINARY,&p->t1,sizeof(p->t1)); }
    if (W1) { EpiSetValue(Diag,L"GpuGmuW1",REG_BINARY,&p->w1,sizeof(p->w1)); }
    if (M1) { EpiSetValue(Diag,L"GpuGmuM1",REG_BINARY,&p->m1,sizeof(p->m1)); }
    if (Q1) { EpiSetValue(Diag,L"GpuGmuQ1",REG_BINARY,&p->q1,sizeof(p->q1)); }
    if (H1) { EpiSetValue(Diag,L"GpuGmuH1",REG_BINARY,&p->h1,sizeof(p->h1)); }
    if (B1) { EpiSetValue(Diag,L"GpuGmuB1",REG_BINARY,&p->b1,sizeof(p->b1)); }
    if (G3b) { EpiSetValue(Diag,L"GpuGmuG3b",REG_BINARY,&p->g3b,sizeof(p->g3b)); }
    if (G3) { EpiSetValue(Diag,L"GpuGmuG3",REG_BINARY,&p->g3,sizeof(p->g3)); }
    if (G2) { EpiSetValue(Diag,L"GpuGmuG2",REG_BINARY,&p->g2,sizeof(p->g2)); }
    if (G1) { GmuG1Diag(s,Diag,&p->g1); }
    if (StageF) { EpiSetValue(Diag,L"GpuGmuGxStageF",REG_BINARY,&p->gxStageF,sizeof(p->gxStageF)); }
    if (StageE) { EpiSetValue(Diag,L"GpuGmuGxStageE",REG_BINARY,&p->gxStageE,sizeof(p->gxStageE)); }
    if (GxCfg) { EpiSetValue(Diag,L"GpuGmuGxCfg",REG_BINARY,&p->gxCfg,sizeof(p->gxCfg)); }
    if (Hwcg) { EpiSetValue(Diag,L"GpuGmuGxHwcg",REG_BINARY,&p->gxHwcg,sizeof(p->gxHwcg)); }
    EpiSetDword(Diag,L"GpuGmuNtStatus",static_cast<ULONG>(st));
    if (Secvid) { EpiSetValue(Diag,L"GpuGmuGxInit",REG_BINARY,&p->gxInit,sizeof(p->gxInit)); }
    if (GxReg) { EpiSetValue(Diag,L"GpuGmuGxReg",REG_BINARY,&p->gxReg,sizeof(p->gxReg)); }
    if (ClearHalt) { EpiSetValue(Diag,L"GpuGmuGxHalt",REG_BINARY,&p->gxHalt,sizeof(p->gxHalt)); }
    if (GxOn)
    {
        EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,&p->gx,sizeof(p->gx));
        EpiSetValue(Diag,L"GpuSmmuCbar",REG_BINARY,s->SmmuCbar,sizeof(s->SmmuCbar));
    }
    if (p->clocks.Attempted) { GmuClockDiag(Diag,&p->clocks); }
    if (p->gbif.Attempted) { GmuGbifDiag(Diag,&p->gbif); }
    if (p->hfi.Attempted) { GmuHfiDiag(Diag,&p->hfi); }
    if (rsccSaved) { EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,p->rscc,sizeof(p->rscc)); }
    if (released)
    {
        // Reset write/readback is mandatory even if breadcrumb persistence fails here.
        GmuStep(Params,0x630);
        WRITE_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET),1);
        const BOOLEAN reset=READ_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET))==1;
        if (p->hfi.Attempted)
        { GmuHfiQueues(s,&p->hfi,8); GmuHfiPower(s,Params,Cc,&p->hfi,2); }
        if (!reset) { result=EPI_GMU_E_TEARDOWN; goto keep; }
        KeStallExecutionProcessor(100);
        if (GxOn && p->gx.Attempted)
        { rpmhStopIssued=GmuGxRpmhStop(s,Params,&p->gx); GmuGxSnap(s,Cc,&p->gx,3); }
        if (p->hfi.BwPublished)
        {
            // Accept confirmed RSCC sleep only after this run actually issued rpmh_stop.
            GmuStep(Params,0x730);
            BOOLEAN idle=FALSE;
            for (ULONG i=0;i<=100;++i)
            {
                if (GmuHfiIdle(s,Cc,rpmhStopIssued)) { idle=TRUE; break; }
                if (i!=100) { KeStallExecutionProcessor(100); }
            }
            GmuHfiPower(s,Params,Cc,&p->hfi,2);
            EpiSetDword(Diag,L"GpuGmuHfiIdleStatus",static_cast<ULONG>(idle ? STATUS_SUCCESS : STATUS_IO_TIMEOUT));
            if (GxOn && p->gx.Attempted)
            { GmuGxStep(Params,0x7F3,&p->gx); GmuGxSnap(s,Cc,&p->gx,4); }
            if (!idle) { result=EPI_GMU_E_TEARDOWN; goto keep; }
        }
    }
    if (GxOn)
    {
        EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,&p->gx,sizeof(p->gx));
        EpiSetValue(Diag,L"GpuSmmuCbar",REG_BINARY,s->SmmuCbar,sizeof(s->SmmuCbar));
    }
    if (ClearHalt && p->gx.Attempted)
    {
        // CX-side halt may survive GX collapse. Never proceed to CX teardown while it remains asserted.
        p->gxHalt.CxIdleStatus=GmuGxCxHaltIdle(s,Params,&p->gxHalt);
        EpiSetValue(Diag,L"GpuGmuGxHalt",REG_BINARY,&p->gxHalt,sizeof(p->gxHalt));
        if (!NT_SUCCESS(p->gxHalt.CxIdleStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (smmuAttempted)
    {
        st=EpiSmmuDisable(s->Smmu);
        GmuSmmuDisableDiag(s->Smmu,Diag);
        if (G1) { GmuG1Diag(s,Diag,&p->g1); }
        EpiSetDword(Diag,L"GpuGmuDisableStatus",static_cast<ULONG>(st));
        if (!NT_SUCCESS(st))
        {
            // Preserve the first SMMU mismatch on Disable too; do not replace its failure status in st.
            ULONG badWrite[4]={};
            if (NT_SUCCESS(EpiSmmuQueryLastError(s->Smmu,&badWrite[0],&badWrite[1],&badWrite[2],&badWrite[3])))
            { EpiSetValue(Diag,L"GpuSmmuBadWrite",REG_BINARY,badWrite,sizeof(badWrite)); }
            result=EPI_GMU_E_TEARDOWN; goto keep;
        }
    }
    if (GxCfg && p->gxCfg.CxWritten!=0)
    {
        p->gxCfg.CxRestoreStatus=GmuGxCfgRestore(s,Params,&p->gxCfg,&p->gx);
        EpiSetValue(Diag,L"GpuGmuGxCfg",REG_BINARY,&p->gxCfg,sizeof(p->gxCfg));
        EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,&p->gx,sizeof(p->gx));
        if (!NT_SUCCESS(p->gxCfg.CxRestoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (Hwcg && p->gxHwcg.Count!=0)
    {
        p->gxHwcg.CgcRestoreStatus=GmuGxHwcgRestore(s,Params,&p->gxHwcg);
        EpiSetValue(Diag,L"GpuGmuGxHwcg",REG_BINARY,&p->gxHwcg,sizeof(p->gxHwcg));
        if (!NT_SUCCESS(p->gxHwcg.CgcRestoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (p->clocks.Saved && (p->clocks.Changed[0] || p->clocks.Changed[1] || p->clocks.Gcc[6]))
    {
        p->clocks.RestoreStatus=GmuClockRestore(s,Params,Cc,&p->clocks); GmuClockDiag(Diag,&p->clocks);
        if (!NT_SUCCESS(p->clocks.RestoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    // Processor reset and SMMU detached; restore address/config registers before freeing buffers.
    for (ULONG i=configured; i-- > 2;)
    {
        st=GmuWrite(s,Params,p->setup[i].Reg,p->setup[i].Original,0x640+i,p->setup[i].CmpMask);
        if (!NT_SUCCESS(st)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (p->gx.PowerCounterSaved && p->gx.PowerCounterOriginal!=0 && !(GxCfg && (p->gxCfg.CxWritten&4)))
    {
        p->gx.PowerCounterRestoreStatus=GmuWrite(s,Params,0x1F840,p->gx.PowerCounterOriginal,0x7F8);
        EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,&p->gx,sizeof(p->gx));
        if (!NT_SUCCESS(p->gx.PowerCounterRestoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (p->gbif.Written!=0)
    {
        p->gbif.RestoreStatus=GmuGbifRestore(s,Params,&p->gbif); GmuGbifDiag(Diag,&p->gbif);
        if (!NT_SUCCESS(p->gbif.RestoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    // CM3 is reset and SMMU detached; RPMH_CTRL has been restored before RSCC changes.
    if (rsccConfigured!=0)
    {
        NTSTATUS restoreStatus=STATUS_SUCCESS;
        for (ULONG i=rsccConfigured; i-- > 0;)
        {
            // Restore even if breadcrumb persistence fails, and attempt every written target.
            st=GmuStep(Params,0x6B0+i);
            WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+p->rscc[i].Dword,p->rscc[i].Original);
            p->rscc[i].Restored=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+p->rscc[i].Dword);
            // STATUS0 contains live sleep/status bits; compare only the bit24 control we changed.
            // Keep the unmasked readback above in GpuGmuRsccSetup for diagnosis.
            const ULONG cmpMask=p->rscc[i].Dword==0x004 ? 0x01000000UL : 0xFFFFFFFFUL;
            if ((p->rscc[i].Restored&cmpMask)!=(p->rscc[i].Original&cmpMask)) { st=STATUS_IO_DEVICE_ERROR; }
            if (!NT_SUCCESS(st) && NT_SUCCESS(restoreStatus))
            { restoreStatus=st; EpiSetDword(Diag,L"GpuGmuRsccRestoreBadIndex",i); }
        }
        EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,p->rscc,sizeof(p->rscc));
        EpiSetDword(Diag,L"GpuGmuRsccRestoreStatus",static_cast<ULONG>(restoreStatus));
        if (!NT_SUCCESS(restoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (p->hfi.Attempted) { GmuHfiPower(s,Params,Cc,&p->hfi,3); GmuHfiDiag(Diag,&p->hfi); }
    if (s->Smmu!=NULL)
    {
        for (ULONG i=0;i<s->Count;++i)
        {
            GMU_REGION* r=&s->Regions[i];
            if (r->Mapped)
            {
                st=EpiSmmuUnmap(s->Smmu,r->Iova,r->Size);
                if (!NT_SUCCESS(st)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
            }
        }
        st=EpiSmmuDestroy(s->Smmu);
        if (!NT_SUCCESS(st)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
        s->Smmu=NULL;
    }
    for (ULONG i=0;i<s->Count;++i)
    { if (!s->Regions[i].External && s->Regions[i].Va!=NULL) { MmFreeContiguousMemorySpecifyCache(s->Regions[i].Va,s->Regions[i].Size,MmNonCached); } }
    for (ULONG i=0;i<RTL_NUMBER_OF(s->Maps);++i)
    { if (s->Maps[i]!=NULL) { MmUnmapIoSpace(s->Maps[i],GmuMapLength(i)); } }
    ExFreePoolWithTag(s,EPI_GMU_TAG);
    EpiSetDword(Diag,L"GpuGmuStatus",result); GmuText(Text,TextCch,L"gmu_status",result);
    ExFreePoolWithTag(p,EPI_GMU_TAG);
    return result;
keep:
    if (G3c) { EpiSetValue(Diag,L"GpuGmuG3c",REG_BINARY,&p->g3c,sizeof(p->g3c)); }
    if (G4) { EpiSetValue(Diag,L"GpuGmuG4",REG_BINARY,&p->g4,sizeof(p->g4)); }
    if (G5) { EpiSetValue(Diag,L"GpuGmuG5",REG_BINARY,&p->g5,sizeof(p->g5)); }
    if (G5b) { EpiSetValue(Diag,L"GpuGmuG5b",REG_BINARY,&p->g5b,sizeof(p->g5b)); }
    if (Z1) { EpiSetValue(Diag,L"GpuGmuZ1",REG_BINARY,&p->z1,sizeof(p->z1)); }
    if (S1) { EpiSetValue(Diag,L"GpuGmuS1",REG_BINARY,&p->s1,sizeof(p->s1)); }
    if (R1) { EpiSetValue(Diag,L"GpuGmuR1",REG_BINARY,&p->r1,sizeof(p->r1)); }
    if (D1) { EpiSetValue(Diag,L"GpuGmuD1",REG_BINARY,&p->d1,sizeof(p->d1)); }
    if (V1) { EpiSetValue(Diag,L"GpuGmuV1",REG_BINARY,&p->v1,sizeof(p->v1)); }
    if (A1) { EpiSetValue(Diag,L"GpuGmuA1",REG_BINARY,&p->a1,sizeof(p->a1)); }
    if (T1) { EpiSetValue(Diag,L"GpuGmuT1",REG_BINARY,&p->t1,sizeof(p->t1)); }
    if (W1) { EpiSetValue(Diag,L"GpuGmuW1",REG_BINARY,&p->w1,sizeof(p->w1)); }
    if (M1) { EpiSetValue(Diag,L"GpuGmuM1",REG_BINARY,&p->m1,sizeof(p->m1)); }
    if (Q1) { EpiSetValue(Diag,L"GpuGmuQ1",REG_BINARY,&p->q1,sizeof(p->q1)); }
    if (H1) { EpiSetValue(Diag,L"GpuGmuH1",REG_BINARY,&p->h1,sizeof(p->h1)); }
    if (B1) { EpiSetValue(Diag,L"GpuGmuB1",REG_BINARY,&p->b1,sizeof(p->b1)); }
    if (G3b) { EpiSetValue(Diag,L"GpuGmuG3b",REG_BINARY,&p->g3b,sizeof(p->g3b)); }
    if (G3) { EpiSetValue(Diag,L"GpuGmuG3",REG_BINARY,&p->g3,sizeof(p->g3)); }
    if (G2) { EpiSetValue(Diag,L"GpuGmuG2",REG_BINARY,&p->g2,sizeof(p->g2)); }
    if (G1) { GmuG1Diag(s,Diag,&p->g1); }
    if (StageF) { EpiSetValue(Diag,L"GpuGmuGxStageF",REG_BINARY,&p->gxStageF,sizeof(p->gxStageF)); }
    if (StageE) { EpiSetValue(Diag,L"GpuGmuGxStageE",REG_BINARY,&p->gxStageE,sizeof(p->gxStageE)); }
    if (GxCfg) { EpiSetValue(Diag,L"GpuGmuGxCfg",REG_BINARY,&p->gxCfg,sizeof(p->gxCfg)); }
    if (Hwcg) { EpiSetValue(Diag,L"GpuGmuGxHwcg",REG_BINARY,&p->gxHwcg,sizeof(p->gxHwcg)); }
    if (Secvid) { EpiSetValue(Diag,L"GpuGmuGxInit",REG_BINARY,&p->gxInit,sizeof(p->gxInit)); }
    if (ClearHalt) { EpiSetValue(Diag,L"GpuGmuGxHalt",REG_BINARY,&p->gxHalt,sizeof(p->gxHalt)); }
    if (GxReg) { EpiSetValue(Diag,L"GpuGmuGxReg",REG_BINARY,&p->gxReg,sizeof(p->gxReg)); }
    if (GxOn) { EpiSetValue(Diag,L"GpuSmmuCbar",REG_BINARY,s->SmmuCbar,sizeof(s->SmmuCbar)); }
    if (GxOn) { EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,&p->gx,sizeof(p->gx)); }
    if (p->clocks.Attempted) { GmuClockDiag(Diag,&p->clocks); }
    if (p->gbif.Attempted) { GmuGbifDiag(Diag,&p->gbif); }
    if (p->hfi.Attempted) { GmuHfiDiag(Diag,&p->hfi); }
    g_KeptGmu=s; *KeepCx=TRUE;
    EpiSetDword(Diag,L"GpuGmuStatus",result); EpiSetDword(Diag,L"GpuGmuKeepCx",1);
    GmuText(Text,TextCch,L"gmu_keep_cx",1);
    ExFreePoolWithTag(p,EPI_GMU_TAG);
    return result;
}
