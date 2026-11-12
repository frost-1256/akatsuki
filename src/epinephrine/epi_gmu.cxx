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
//      Record ACKs/queue indices,
//      diagnostics, the SMMU fault state and the GMU log; put CM3 back in reset, bit12 rpmh_stop, check RSCC/GX idle.
//   7. EpiSmmuDisable, restore GMU controls, optional GBIF and RSCC, then Unmap / Destroy / free.
//      A failed Disable keeps everything and asks the caller to keep
//      CX on (*KeepCx).
// Not done here: rpmh_start, force_off, CP execution, GPU PDC (reading it reset the phone on 2026-11-07).
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
};
struct GMU_TEST {
    GMU_REGION Regions[EPI_GMU_MAX_REGIONS];
    ULONG Count;
    EPI_SMMU* Smmu;
    PVOID Maps[8]; // control C/A/B, ITCM, DTCM, RSCC, optional GBIF/GCC vote; retained on failed teardown
    ULONG SmmuCbar[5][3]; // GX snapshot stages: {stage, query NTSTATUS, CBAR0}
};
static GMU_TEST* g_KeptGmu = NULL; // debugger-visible ownership of quarantined allocations

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
    r->Iova = Addr; r->Size = rounded; r->Flags = Flags;
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
static volatile ULONG* GmuReg(GMU_TEST* S, ULONG Dword)
{
    ULONG page = 0, first = 0x1F400;
    if (Dword >= 0x23800) { page=2; first=0x23800; }
    else if (Dword >= 0x1F800) { page=1; first=0x1F800; }
    NT_ASSERT(Dword >= first && Dword-first < 0x400);
    return static_cast<volatile ULONG*>(S->Maps[page])+Dword-first;
}
static NTSTATUS GmuStep(HANDLE Params, ULONG Step)
{
    NTSTATUS st=EpiSetDword(Params,L"EpiGpuProbeStep",Step);
    return NT_SUCCESS(st) ? ZwFlushKey(Params) : st;
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
static GMU_GX_INIT_TRACE GmuGxInitTrace()
{
    GMU_GX_INIT_TRACE t={}; t.Reason=GX_REG_NOT_REACHED; t.Status=STATUS_NOT_FOUND;
    t.Gdscr=0xFFFFFFFF; t.GxHm=0xFFFFFFFF;
    for (ULONG i=0;i<16;++i) { t.Before[i]=0xFFFFFFFF; t.After[i]=0xFFFFFFFF; }
    return t;
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
static GMU_GX_HWCG_TRACE GmuGxHwcgTrace()
{
    GMU_GX_HWCG_TRACE t;
    RtlFillMemory(&t,sizeof(t),0xFF);
    t.Attempted=0; t.Reason=GX_REG_NOT_REACHED; t.Count=0; t.Skipped=0;
    t.Status=STATUS_NOT_FOUND; t.CgcRestoreStatus=STATUS_NOT_FOUND;
    return t;
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
static NTSTATUS GmuGxOob(GMU_TEST* S, HANDLE Params, volatile ULONG* Cc, GMU_GX_TRACE* T,
                         GMU_GX_REG_TRACE* GxReg = NULL, GMU_GX_HALT_TRACE* Halt = NULL,
                         GMU_GX_INIT_TRACE* Init = NULL, BOOLEAN AddrMode = FALSE, GMU_GX_HWCG_TRACE* Hwcg = NULL)
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
        T->PowerCounterOriginal=READ_REGISTER_ULONG(GmuReg(S,0x1F840));
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

ULONG EpiGmuBootTest(HANDLE Params, HANDLE Diag, volatile ULONG* Cc, const UCHAR* Fw, ULONG FwLen,
                     PWCHAR Text, SIZE_T TextCch, BOOLEAN* KeepCx, BOOLEAN StartHfi, BOOLEAN VoteGx, BOOLEAN FastClock,
                     BOOLEAN GxOn, BOOLEAN GxReg, BOOLEAN ClearHalt, BOOLEAN Secvid, BOOLEAN AddrMode, BOOLEAN Hwcg)
{
    PAGED_CODE();
    if (KeepCx == NULL) { return EPI_GMU_E_PRECOND; }
    *KeepCx=FALSE;
    if (g_KeptGmu != NULL) { *KeepCx=TRUE; return EPI_GMU_E_TEARDOWN; }
    if (VoteGx && !StartHfi) { return EPI_GMU_E_PRECOND; }
    if (FastClock && (!VoteGx || !StartHfi)) { return EPI_GMU_E_PRECOND; }
    if (GxOn && (!FastClock || !VoteGx || !StartHfi)) { return EPI_GMU_E_PRECOND; }
    if (GxReg && !GxOn) { return EPI_GMU_E_PRECOND; }
    if (ClearHalt && !GxReg) { return EPI_GMU_E_PRECOND; }
    if (Secvid && !ClearHalt) { return EPI_GMU_E_PRECOND; }
    if (AddrMode && !Secvid) { return EPI_GMU_E_PRECOND; }
    if (Hwcg && !AddrMode) { return EPI_GMU_E_PRECOND; }
    if (Cc == NULL || Text == NULL || TextCch==0) { return EPI_GMU_E_PRECOND; }
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
    GMU_TEST* s=static_cast<GMU_TEST*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(GMU_TEST),EPI_GMU_TAG));
    if (s==NULL) { return EPI_GMU_E_NOMEM; }
    for (ULONG i=0;i<RTL_NUMBER_OF(s->SmmuCbar);++i)
    {
        s->SmmuCbar[i][0]=i; s->SmmuCbar[i][1]=static_cast<ULONG>(STATUS_NOT_FOUND);
        s->SmmuCbar[i][2]=0xFFFFFFFF;
    }
    ULONG result=EPI_GMU_OK, configured=0, rsccConfigured=0;
    NTSTATUS st=STATUS_SUCCESS;
    BOOLEAN smmuAttempted=FALSE, released=FALSE, rsccSaved=FALSE, rpmhStopIssued=FALSE;
    GMU_GX_REG_TRACE gxReg={0,GX_REG_NOT_REACHED,0xFFFFFFFF,0xFFFFFFFF,0,
                           {0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF}};
    GMU_GX_HALT_TRACE gxHalt={}; gxHalt.Reason=GX_REG_NOT_REACHED;
    gxHalt.Gdscr=0xFFFFFFFF; gxHalt.GxHm=0xFFFFFFFF;
    gxHalt.Status=STATUS_NOT_FOUND; gxHalt.CxIdleStatus=STATUS_NOT_FOUND; gxHalt.CxPolls=0xFFFFFFFF;
    for (ULONG i=0;i<4;++i) { gxHalt.Before[i]=0xFFFFFFFF; gxHalt.After[i]=0xFFFFFFFF; }
    gxHalt.FinalCx[0]=0xFFFFFFFF; gxHalt.FinalCx[1]=0xFFFFFFFF;
    GMU_GX_INIT_TRACE gxInit=GmuGxInitTrace();
    GMU_GX_HWCG_TRACE gxHwcg=GmuGxHwcgTrace();
    GMU_GX_TRACE gx={};
    gx.OobStatus=STATUS_NOT_FOUND; gx.HaltStatus=STATUS_NOT_FOUND; gx.SlumberStatus=STATUS_NOT_FOUND;
    gx.BusyStatus=STATUS_NOT_FOUND; gx.RpmhStopStatus=STATUS_NOT_FOUND; gx.IdleStatus=STATUS_NOT_FOUND;
    gx.PowerCounterRestoreStatus=STATUS_NOT_FOUND; gx.BreadcrumbStatus=STATUS_SUCCESS;
    gx.OobPolls=0xFFFFFFFF; gx.OobInfo=0xFFFFFFFF; gx.BusyPolls=0xFFFFFFFF;
    gx.PowerCounterOriginal=0xFFFFFFFF; gx.RpmhStopPolls=0xFFFFFFFF; gx.IdlePolls=0xFFFFFFFF;
    gx.IdleLast=0xFFFFFFFF; gx.BusyLast=0xFFFFFFFF; gx.RpmhStopLast=0xFFFFFFFF;
    for (ULONG i=0;i<2;++i)
    { gx.HaltPolls[i]=0xFFFFFFFF; gx.HaltAck[i]=0xFFFFFFFF; gx.QueueEmpty[i]=0xFFFFFFFF; }
    for (ULONG i=0;i<RTL_NUMBER_OF(gx.Snap);++i)
    {
        gx.Snap[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(gx.Snap[i]);++j) { gx.Snap[i][j]=0xFFFFFFFF; }
    }
    GMU_CLOCK_TRACE clocks={}; clocks.Status=STATUS_NOT_FOUND; clocks.RestoreStatus=STATUS_NOT_FOUND;
    for (ULONG i=0;i<6;++i) { clocks.Gcc[i]=0xFFFFFFFF; }
    for (ULONG i=0;i<RTL_NUMBER_OF(clocks.Rcgs);++i)
    {
        GMU_CLOCK_RCG* r=&clocks.Rcgs[i];
        r->CmdOffset=i==0 ? 0x1120 : 0x117C; r->CfgOffset=r->CmdOffset+4;
        r->TargetCfg=i==0 ? 0x602 : 0x603;
        r->OriginalCmd=0xFFFFFFFF; r->OriginalCfg=0xFFFFFFFF;
        r->SetCmd=0xFFFFFFFF; r->SetCfg=0xFFFFFFFF; r->SetPolls=0xFFFFFFFF;
        r->RestoreCmd=0xFFFFFFFF; r->RestoreCfg=0xFFFFFFFF;
        r->RestorePolls=0xFFFFFFFF; r->PendingPolls=0xFFFFFFFF;
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(clocks.Branches);++i)
    { for (ULONG j=0;j<RTL_NUMBER_OF(clocks.Branches[i]);++j) { clocks.Branches[i][j]=0xFFFFFFFF; } }
    GMU_HFI_TRACE hfi={}; hfi.Status=STATUS_NOT_FOUND; hfi.CtrlStatus=0xFFFFFFFF;
    hfi.ExtraEnabled=VoteGx; hfi.ExtraStatus=STATUS_NOT_FOUND;
    GMU_GBIF_TRACE gbif={}; gbif.Status=STATUS_NOT_FOUND; gbif.RestoreStatus=STATUS_NOT_FOUND;
    gbif.BadIndex=0xFFFFFFFF; gbif.RestoreBadIndex=0xFFFFFFFF;
    for (ULONG i=0;i<RTL_NUMBER_OF(gbif.Entries);++i)
    {
        gbif.Entries[i].Reg=0x3C03+i; gbif.Entries[i].Value=0x00071620;
        gbif.Entries[i].Original=0xFFFFFFFF; gbif.Entries[i].Readback=0xFFFFFFFF; gbif.Entries[i].Restored=0xFFFFFFFF;
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(hfi.Messages);++i) { hfi.Messages[i].Status=STATUS_NOT_FOUND; }
    for (ULONG i=0;i<RTL_NUMBER_OF(hfi.Queues);++i)
    {
        hfi.Queues[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(hfi.Queues[i]);++j) { hfi.Queues[i][j]=0xFFFFFFFF; }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(hfi.Power);++i)
    {
        hfi.Power[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(hfi.Power[i]);++j) { hfi.Power[i][j]=0xFFFFFFFF; }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(hfi.Rscc);++i)
    {
        hfi.Rscc[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(hfi.Rscc[i]);++j) { hfi.Rscc[i][j]=0xFFFFFFFF; }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(hfi.Extra);++i)
    {
        hfi.Extra[i][0]=i;
        for (ULONG j=1;j<RTL_NUMBER_OF(hfi.Extra[i]);++j) { hfi.Extra[i][j]=0xFFFFFFFF; }
    }
    // No GPU PDC or RSCC handshake. VoteGx opts into GBIF QSB and the GX vote after START.
    // Field order is the GpuGmuSetup REG_BINARY diagnostic ABI.
    struct SETTING { ULONG Reg, Original, Value, Readback, CmpMask; } setup[] = {
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
    // Same ten targets/values as the independent 0x100 test; no HIDDEN_TCS ADDR writes.
    struct RSCC_SETTING { ULONG Dword, Original, Value, Readback, Restored; } rscc[] = {
        {0x004,0,0x01000000,0xFFFFFFFF,0xFFFFFFFF},
        {0x00E,0,0,0xFFFFFFFF,0xFFFFFFFF}, {0x010,0,0,0xFFFFFFFF,0xFFFFFFFF},
        {0x012,0,0x80000000,0xFFFFFFFF,0xFFFFFFFF}, {0x100,0,0,0xFFFFFFFF,0xFFFFFFFF},
        {0x180,0,0xEAAAE5A0,0xFFFFFFFF,0xFFFFFFFF}, {0x181,0,0xE1A1EBAB,0xFFFFFFFF,0xFFFFFFFF},
        {0x182,0,0xA2E0A581,0xFFFFFFFF,0xFFFFFFFF}, {0x183,0,0xECAC82E2,0xFFFFFFFF,0xFFFFFFFF},
        {0x184,0,0x0020EDAD,0xFFFFFFFF,0xFFFFFFFF}
    };
    const ULONGLONG bases[]={EPI_GMU_PAGE_C_PHYS,EPI_GMU_PAGE_A_PHYS,EPI_GMU_PAGE_B_PHYS,
                             EPI_GMU_ITCM_PHYS_,EPI_GMU_DTCM_PHYS_,EPI_GMU_RSCC_PHYS};
    if (!GmuPlan(s,Fw,FwLen)) { result=EPI_GMU_E_FW; goto cleanup; }
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
        PHYSICAL_ADDRESS low,high,boundary; low.QuadPart=0; high.QuadPart=(1ULL<<36)-1; boundary.QuadPart=0;
        r->Va=MmAllocateContiguousMemorySpecifyCache(r->Size,low,high,boundary,MmNonCached);
        if (r->Va==NULL) { result=EPI_GMU_E_NOMEM; goto cleanup; }
        r->Pa=MmGetPhysicalAddress(r->Va); RtlZeroMemory(r->Va,r->Size);
    }
    st=EpiSmmuCreate(Params,&s->Smmu);
    if (!NT_SUCCESS(st)) { result=EPI_GMU_E_SMMU_MAP; goto cleanup; }
    for (ULONG i=0;i<s->Count;++i)
    {
        GMU_REGION* r=&s->Regions[i];
        st=EpiSmmuMap(s->Smmu,r->Iova,r->Pa,r->Size,r->Flags);
        if (!NT_SUCCESS(st)) { result=EPI_GMU_E_SMMU_MAP; goto cleanup; }
        r->Mapped=TRUE;
    }
    GmuQueues(s);
    // Snapshot all controls before writing any. IRQ masks remain masked until CX off.
    for (ULONG i=0;i<RTL_NUMBER_OF(setup);++i) { setup[i].Original=READ_REGISTER_ULONG(GmuReg(s,setup[i].Reg)); }
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
    for (ULONG i=0;i<RTL_NUMBER_OF(rscc);++i)
    { rscc[i].Original=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+rscc[i].Dword); }
    rsccSaved=TRUE;
    for (ULONG i=0;i<RTL_NUMBER_OF(rscc);++i)
    {
        st=GmuStep(Params,0x690+i);
        if (NT_SUCCESS(st))
        {
            rsccConfigured=i+1;
            WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+rscc[i].Dword,rscc[i].Value);
            rscc[i].Readback=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+rscc[i].Dword);
            if (rscc[i].Readback!=rscc[i].Value) { st=STATUS_IO_DEVICE_ERROR; }
        }
        if (!NT_SUCCESS(st))
        { EpiSetDword(Diag,L"GpuGmuRsccBadIndex",i); result=EPI_GMU_E_RSCC; goto cleanup; }
    }
    EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,rscc,sizeof(rscc));
    for (ULONG i=0;i<RTL_NUMBER_OF(setup);++i)
    {
        configured=i+1;
        st=GmuWrite(s,Params,setup[i].Reg,setup[i].Value,0x600+i,setup[i].CmpMask,&setup[i].Readback);
        if (!NT_SUCCESS(st))
        {
            EpiSetDword(Diag,L"GpuGmuBadIndex",i);
            EpiSetValue(Diag,L"GpuGmuSetup",REG_BINARY,setup,sizeof(setup));
            result=EPI_GMU_E_TCM; goto cleanup;
        }
        if (VoteGx && setup[i].Reg==R_PWR_COL_CP_MSG)
        {
            // Log address is configured; apply CX-side GBIF before the remaining power controls.
            st=GmuGbifSetup(s,Params,Cc,&gbif); gbif.Status=st;
            GmuGbifDiag(Diag,&gbif);
            if (!NT_SUCCESS(st))
            {
                EpiSetValue(Diag,L"GpuGmuSetup",REG_BINARY,setup,sizeof(setup));
                result=EPI_GMU_E_GBIF; goto cleanup;
            }
        }
    }
    EpiSetValue(Diag,L"GpuGmuSetup",REG_BINARY,setup,sizeof(setup));
    if (!GmuCopy(s,Params,Fw,FwLen))
    {
        EpiSetDword(Diag,L"GpuGmuBadIndex",0xFF);
        st=STATUS_IO_DEVICE_ERROR; result=EPI_GMU_E_TCM; goto cleanup;
    }
    GmuText(Text,TextCch,L"gmu_fw_ver",READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[4])+EPI_GMU_VER_DTCM_OFF/4));
    if (FastClock)
    {
        st=GmuClockSetup(s,Params,Cc,&clocks); clocks.Status=st; GmuClockDiag(Diag,&clocks);
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
        st=GmuHfiTables(s,Params,Cc,&hfi,StartHfi,VoteGx); hfi.Status=st;
        if (!NT_SUCCESS(st)) { result=EPI_GMU_E_HFI; }
        else if (GxOn)
        {
            const NTSTATUS oob=GmuGxOob(s,Params,Cc,&gx,GxReg ? &gxReg : NULL,
                                        ClearHalt ? &gxHalt : NULL,Secvid ? &gxInit : NULL,AddrMode,Hwcg ? &gxHwcg : NULL);
            NTSTATUS slumber=STATUS_NOT_FOUND;
            if (gx.Attempted)
            {
                slumber=GmuGxSlumber(s,Params,Cc,&gx,&hfi);
                if (!NT_SUCCESS(slumber)) { hfi.Status=slumber; }
            }
            if (!NT_SUCCESS(oob) || (gx.Attempted && !NT_SUCCESS(slumber)) || !NT_SUCCESS(gx.BreadcrumbStatus) ||
                (ClearHalt && !NT_SUCCESS(gxHalt.Status)) || (Secvid && !NT_SUCCESS(gxInit.Status)) ||
                (Hwcg && !NT_SUCCESS(gxHwcg.Status)))
            {
                result=EPI_GMU_E_GX;
                st=!NT_SUCCESS(oob) ? oob : (!NT_SUCCESS(slumber) ? slumber :
                   (!NT_SUCCESS(gx.BreadcrumbStatus) ? gx.BreadcrumbStatus :
                    (ClearHalt && !NT_SUCCESS(gxHalt.Status) ? gxHalt.Status :
                     (Secvid && !NT_SUCCESS(gxInit.Status) ? gxInit.Status : gxHwcg.Status))));
            }
        }
        GmuHfiPower(s,Params,Cc,&hfi,1);
        GmuText(Text,TextCch,L"gmu_hfi_status",static_cast<ULONG>(hfi.Status));
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
    if (Hwcg) { EpiSetValue(Diag,L"GpuGmuGxHwcg",REG_BINARY,&gxHwcg,sizeof(gxHwcg)); }
    EpiSetDword(Diag,L"GpuGmuNtStatus",static_cast<ULONG>(st));
    if (Secvid) { EpiSetValue(Diag,L"GpuGmuGxInit",REG_BINARY,&gxInit,sizeof(gxInit)); }
    if (GxReg) { EpiSetValue(Diag,L"GpuGmuGxReg",REG_BINARY,&gxReg,sizeof(gxReg)); }
    if (ClearHalt) { EpiSetValue(Diag,L"GpuGmuGxHalt",REG_BINARY,&gxHalt,sizeof(gxHalt)); }
    if (GxOn)
    {
        EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,&gx,sizeof(gx));
        EpiSetValue(Diag,L"GpuSmmuCbar",REG_BINARY,s->SmmuCbar,sizeof(s->SmmuCbar));
    }
    if (clocks.Attempted) { GmuClockDiag(Diag,&clocks); }
    if (gbif.Attempted) { GmuGbifDiag(Diag,&gbif); }
    if (hfi.Attempted) { GmuHfiDiag(Diag,&hfi); }
    if (rsccSaved) { EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,rscc,sizeof(rscc)); }
    if (released)
    {
        // Reset write/readback is mandatory even if breadcrumb persistence fails here.
        GmuStep(Params,0x630);
        WRITE_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET),1);
        const BOOLEAN reset=READ_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET))==1;
        if (hfi.Attempted)
        { GmuHfiQueues(s,&hfi,8); GmuHfiPower(s,Params,Cc,&hfi,2); }
        if (!reset) { result=EPI_GMU_E_TEARDOWN; goto keep; }
        KeStallExecutionProcessor(100);
        if (GxOn && gx.Attempted)
        { rpmhStopIssued=GmuGxRpmhStop(s,Params,&gx); GmuGxSnap(s,Cc,&gx,3); }
        if (hfi.BwPublished)
        {
            // Accept confirmed RSCC sleep only after this run actually issued rpmh_stop.
            GmuStep(Params,0x730);
            BOOLEAN idle=FALSE;
            for (ULONG i=0;i<=100;++i)
            {
                if (GmuHfiIdle(s,Cc,rpmhStopIssued)) { idle=TRUE; break; }
                if (i!=100) { KeStallExecutionProcessor(100); }
            }
            GmuHfiPower(s,Params,Cc,&hfi,2);
            EpiSetDword(Diag,L"GpuGmuHfiIdleStatus",static_cast<ULONG>(idle ? STATUS_SUCCESS : STATUS_IO_TIMEOUT));
            if (GxOn && gx.Attempted)
            { GmuGxStep(Params,0x7F3,&gx); GmuGxSnap(s,Cc,&gx,4); }
            if (!idle) { result=EPI_GMU_E_TEARDOWN; goto keep; }
        }
    }
    if (GxOn)
    {
        EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,&gx,sizeof(gx));
        EpiSetValue(Diag,L"GpuSmmuCbar",REG_BINARY,s->SmmuCbar,sizeof(s->SmmuCbar));
    }
    if (ClearHalt && gx.Attempted)
    {
        // CX-side halt may survive GX collapse. Never proceed to CX teardown while it remains asserted.
        gxHalt.CxIdleStatus=GmuGxCxHaltIdle(s,Params,&gxHalt);
        EpiSetValue(Diag,L"GpuGmuGxHalt",REG_BINARY,&gxHalt,sizeof(gxHalt));
        if (!NT_SUCCESS(gxHalt.CxIdleStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (smmuAttempted)
    {
        st=EpiSmmuDisable(s->Smmu);
        GmuSmmuDisableDiag(s->Smmu,Diag);
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
    if (Hwcg && gxHwcg.Count!=0)
    {
        gxHwcg.CgcRestoreStatus=GmuGxHwcgRestore(s,Params,&gxHwcg);
        EpiSetValue(Diag,L"GpuGmuGxHwcg",REG_BINARY,&gxHwcg,sizeof(gxHwcg));
        if (!NT_SUCCESS(gxHwcg.CgcRestoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (clocks.Saved && (clocks.Changed[0] || clocks.Changed[1] || clocks.Gcc[6]))
    {
        clocks.RestoreStatus=GmuClockRestore(s,Params,Cc,&clocks); GmuClockDiag(Diag,&clocks);
        if (!NT_SUCCESS(clocks.RestoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    // Processor reset and SMMU detached; restore address/config registers before freeing buffers.
    for (ULONG i=configured; i-- > 2;)
    {
        st=GmuWrite(s,Params,setup[i].Reg,setup[i].Original,0x640+i,setup[i].CmpMask);
        if (!NT_SUCCESS(st)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (gx.PowerCounterSaved && gx.PowerCounterOriginal!=0)
    {
        gx.PowerCounterRestoreStatus=GmuWrite(s,Params,0x1F840,gx.PowerCounterOriginal,0x7F8);
        EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,&gx,sizeof(gx));
        if (!NT_SUCCESS(gx.PowerCounterRestoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (gbif.Written!=0)
    {
        gbif.RestoreStatus=GmuGbifRestore(s,Params,&gbif); GmuGbifDiag(Diag,&gbif);
        if (!NT_SUCCESS(gbif.RestoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    // CM3 is reset and SMMU detached; RPMH_CTRL has been restored before RSCC changes.
    if (rsccConfigured!=0)
    {
        NTSTATUS restoreStatus=STATUS_SUCCESS;
        for (ULONG i=rsccConfigured; i-- > 0;)
        {
            // Restore even if breadcrumb persistence fails, and attempt every written target.
            st=GmuStep(Params,0x6B0+i);
            WRITE_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+rscc[i].Dword,rscc[i].Original);
            rscc[i].Restored=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+rscc[i].Dword);
            // STATUS0 contains live sleep/status bits; compare only the bit24 control we changed.
            // Keep the unmasked readback above in GpuGmuRsccSetup for diagnosis.
            const ULONG cmpMask=rscc[i].Dword==0x004 ? 0x01000000UL : 0xFFFFFFFFUL;
            if ((rscc[i].Restored&cmpMask)!=(rscc[i].Original&cmpMask)) { st=STATUS_IO_DEVICE_ERROR; }
            if (!NT_SUCCESS(st) && NT_SUCCESS(restoreStatus))
            { restoreStatus=st; EpiSetDword(Diag,L"GpuGmuRsccRestoreBadIndex",i); }
        }
        EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,rscc,sizeof(rscc));
        EpiSetDword(Diag,L"GpuGmuRsccRestoreStatus",static_cast<ULONG>(restoreStatus));
        if (!NT_SUCCESS(restoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    if (hfi.Attempted) { GmuHfiPower(s,Params,Cc,&hfi,3); GmuHfiDiag(Diag,&hfi); }
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
    { if (s->Regions[i].Va!=NULL) { MmFreeContiguousMemorySpecifyCache(s->Regions[i].Va,s->Regions[i].Size,MmNonCached); } }
    for (ULONG i=0;i<RTL_NUMBER_OF(s->Maps);++i)
    { if (s->Maps[i]!=NULL) { MmUnmapIoSpace(s->Maps[i],GmuMapLength(i)); } }
    ExFreePoolWithTag(s,EPI_GMU_TAG);
    EpiSetDword(Diag,L"GpuGmuStatus",result); GmuText(Text,TextCch,L"gmu_status",result);
    return result;
keep:
    if (Hwcg) { EpiSetValue(Diag,L"GpuGmuGxHwcg",REG_BINARY,&gxHwcg,sizeof(gxHwcg)); }
    if (Secvid) { EpiSetValue(Diag,L"GpuGmuGxInit",REG_BINARY,&gxInit,sizeof(gxInit)); }
    if (ClearHalt) { EpiSetValue(Diag,L"GpuGmuGxHalt",REG_BINARY,&gxHalt,sizeof(gxHalt)); }
    if (GxReg) { EpiSetValue(Diag,L"GpuGmuGxReg",REG_BINARY,&gxReg,sizeof(gxReg)); }
    if (GxOn) { EpiSetValue(Diag,L"GpuSmmuCbar",REG_BINARY,s->SmmuCbar,sizeof(s->SmmuCbar)); }
    if (GxOn) { EpiSetValue(Diag,L"GpuGmuGx",REG_BINARY,&gx,sizeof(gx)); }
    if (clocks.Attempted) { GmuClockDiag(Diag,&clocks); }
    if (gbif.Attempted) { GmuGbifDiag(Diag,&gbif); }
    if (hfi.Attempted) { GmuHfiDiag(Diag,&hfi); }
    g_KeptGmu=s; *KeepCx=TRUE;
    EpiSetDword(Diag,L"GpuGmuStatus",result); EpiSetDword(Diag,L"GpuGmuKeepCx",1);
    GmuText(Text,TextCch,L"gmu_keep_cx",1);
    return result;
}
