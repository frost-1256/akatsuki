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
//   5. EpiSmmuEnable, then release the CM3 (SYSRESET 1, CP_RESP 0, SYSRESET 0) and poll FW_INIT_RESULT. This
//      firmware is newer than 0x20010004, so the done value is (v & 0x1FF) == 0x100. Up to 100 ms.
//   6. Record diagnostics, the SMMU fault state and the start of the GMU log; put the CM3 back in reset.
//   7. EpiSmmuDisable, restore GMU controls and RSCC, then Unmap / Destroy / free. A failed Disable keeps everything and asks the caller to keep
//      CX on (*KeepCx).
// Not done here (later steps): rpmh_start/stop, GPU PDC (reading it reset the phone on 2026-11-07),
// GBIF QSB settings, HFI messages. Preserve all RSCC HIDDEN_TCS ADDR and PDC slave/sequence/match values.

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
#define R_CX_FAL_INTF           0x1F8F0
#define R_CX_FALNEXT_INTF       0x1F8F1
#define R_PWR_COL_CP_MSG        0x1F900
#define R_PWR_COL_CP_RESP       0x1F901
#define R_HFI_SFR_ADDR          0x1F982
#define R_HFI_QTBL_INFO         0x1F984
#define R_HFI_QTBL_ADDR         0x1F985
#define R_GMU2HOST_INTR_INFO    0x1F992
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
#define EPI_GMU_LOG_DUMP        0x1000UL

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
    PVOID Maps[6]; // control C/A/B, ITCM, DTCM, RSCC; retained after failed teardown
};
static GMU_TEST* g_KeptGmu = NULL; // debugger-visible ownership of quarantined allocations

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
static VOID GmuText(PWCHAR Text, SIZE_T Cch, PCWSTR Name, ULONG Value)
{
    WCHAR line[72];
    RtlStringCchPrintfW(line,RTL_NUMBER_OF(line),L"%s=%08X; ",Name,Value);
    RtlStringCchCatW(Text,Cch,line);
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
                     PWCHAR Text, SIZE_T TextCch, BOOLEAN* KeepCx)
{
    PAGED_CODE();
    if (KeepCx == NULL) { return EPI_GMU_E_PRECOND; }
    *KeepCx=FALSE;
    if (g_KeptGmu != NULL) { *KeepCx=TRUE; return EPI_GMU_E_TEARDOWN; }
    if (Cc == NULL || Text == NULL || TextCch==0) { return EPI_GMU_E_PRECOND; }
    EpiSetDword(Diag,L"GpuGmuKeepCx",0);
    EpiSetDword(Diag,L"GpuGmuFaultStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetDword(Diag,L"GpuGmuDisableStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    EpiSetDword(Diag,L"GpuGmuBadIndex",0xFFFFFFFFUL);
    EpiSetValue(Diag,L"GpuGmuSetup",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuSmmuBadWrite",REG_BINARY,NULL,0);
    EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,NULL,0);
    EpiSetDword(Diag,L"GpuGmuRsccBadIndex",0xFFFFFFFFUL);
    EpiSetDword(Diag,L"GpuGmuRsccRestoreBadIndex",0xFFFFFFFFUL);
    EpiSetDword(Diag,L"GpuGmuRsccRestoreStatus",static_cast<ULONG>(STATUS_NOT_FOUND));
    GMU_TEST* s=static_cast<GMU_TEST*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(GMU_TEST),EPI_GMU_TAG));
    if (s==NULL) { return EPI_GMU_E_NOMEM; }
    ULONG result=EPI_GMU_OK, configured=0, rsccConfigured=0;
    NTSTATUS st=STATUS_SUCCESS;
    BOOLEAN smmuAttempted=FALSE, released=FALSE, rsccSaved=FALSE;
    // No GPU PDC, RSCC handshake, HFI messages or explicit GX requests.
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
        const ULONG clocks[]={0x1078,0x109C,0x1098,0x1204,0x5000};
        if (!(READ_REGISTER_ULONG(Cc+0x106C/4)&0x80000000UL) ||
            (READ_REGISTER_ULONG(Cc+0x100C/4)&0x80000001UL)!=1) { result=EPI_GMU_E_PRECOND; goto cleanup; }
        for (ULONG i=0;i<RTL_NUMBER_OF(clocks);++i)
        { if (READ_REGISTER_ULONG(Cc+clocks[i]/4)&0x80000000UL) { result=EPI_GMU_E_PRECOND; goto cleanup; } }
    }
    for (ULONG i=0;i<RTL_NUMBER_OF(bases);++i)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart=bases[i];
        s->Maps[i]=MmMapIoSpaceEx(pa,i==5 ? EPI_GMU_RSCC_SIZE : (i<3 ? PAGE_SIZE : EPI_GMU_TCM_SIZE),PAGE_READWRITE|PAGE_NOCACHE);
        if (s->Maps[i]==NULL) { result=EPI_GMU_E_NOMEM; goto cleanup; }
    }
    if (READ_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET))!=1)
    { result=EPI_GMU_E_TEARDOWN; goto keep; } // never collapse a pre-existing running GMU
    if ((READ_REGISTER_ULONG(GmuReg(s,0x1F8D0))&0xC0)!=0xC0)
    { result=EPI_GMU_E_PRECOND; goto cleanup; }
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
    if (READ_REGISTER_ULONG(GmuReg(s,0x23B07))!=0 || READ_REGISTER_ULONG(GmuReg(s,0x23B08))!=0 ||
        READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[5])+0x101)!=0)
    { st=STATUS_INVALID_DEVICE_STATE; result=EPI_GMU_E_PRECOND; goto cleanup; }
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
    }
    EpiSetValue(Diag,L"GpuGmuSetup",REG_BINARY,setup,sizeof(setup));
    if (!GmuCopy(s,Params,Fw,FwLen))
    {
        EpiSetDword(Diag,L"GpuGmuBadIndex",0xFF);
        st=STATUS_IO_DEVICE_ERROR; result=EPI_GMU_E_TCM; goto cleanup;
    }
    GmuText(Text,TextCch,L"gmu_fw_ver",READ_REGISTER_ULONG(static_cast<volatile ULONG*>(s->Maps[4])+EPI_GMU_VER_DTCM_OFF/4));
    smmuAttempted=TRUE;
    st=EpiSmmuEnable(s->Smmu,Cc,static_cast<volatile ULONG*>(s->Maps[1]));
    if (!NT_SUCCESS(st))
    {
        // GpuSmmuBadWrite: four little-endian ULONGs {Step, Offset (bytes), Value, Readback}.
        // Keep the Enable status in st even when the failure was not a write mismatch.
        ULONG badWrite[4]={};
        if (NT_SUCCESS(EpiSmmuQueryLastError(s->Smmu,&badWrite[0],&badWrite[1],&badWrite[2],&badWrite[3])))
        { EpiSetValue(Diag,L"GpuSmmuBadWrite",REG_BINARY,badWrite,sizeof(badWrite)); }
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
    EpiSetDword(Diag,L"GpuGmuNtStatus",static_cast<ULONG>(st));
    if (rsccSaved) { EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,rscc,sizeof(rscc)); }
    if (released)
    {
        // Reset write/readback is mandatory even if breadcrumb persistence fails here.
        GmuStep(Params,0x630);
        WRITE_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET),1);
        if (READ_REGISTER_ULONG(GmuReg(s,R_CM3_SYSRESET))!=1) { result=EPI_GMU_E_TEARDOWN; goto keep; }
        KeStallExecutionProcessor(100);
    }
    if (smmuAttempted)
    {
        st=EpiSmmuDisable(s->Smmu);
        EpiSetDword(Diag,L"GpuGmuDisableStatus",static_cast<ULONG>(st));
        if (!NT_SUCCESS(st)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
    // Processor reset and SMMU detached; restore address/config registers before freeing buffers.
    for (ULONG i=configured; i-- > 2;)
    {
        st=GmuWrite(s,Params,setup[i].Reg,setup[i].Original,0x640+i,setup[i].CmpMask);
        if (!NT_SUCCESS(st)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
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
            if (rscc[i].Restored!=rscc[i].Original) { st=STATUS_IO_DEVICE_ERROR; }
            if (!NT_SUCCESS(st) && NT_SUCCESS(restoreStatus))
            { restoreStatus=st; EpiSetDword(Diag,L"GpuGmuRsccRestoreBadIndex",i); }
        }
        EpiSetValue(Diag,L"GpuGmuRsccSetup",REG_BINARY,rscc,sizeof(rscc));
        EpiSetDword(Diag,L"GpuGmuRsccRestoreStatus",static_cast<ULONG>(restoreStatus));
        if (!NT_SUCCESS(restoreStatus)) { result=EPI_GMU_E_TEARDOWN; goto keep; }
    }
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
    { if (s->Maps[i]!=NULL) { MmUnmapIoSpace(s->Maps[i],i==5 ? EPI_GMU_RSCC_SIZE : (i<3 ? PAGE_SIZE : EPI_GMU_TCM_SIZE)); } }
    ExFreePoolWithTag(s,EPI_GMU_TAG);
    EpiSetDword(Diag,L"GpuGmuStatus",result); GmuText(Text,TextCch,L"gmu_status",result);
    return result;
keep:
    g_KeptGmu=s; *KeepCx=TRUE;
    EpiSetDword(Diag,L"GpuGmuStatus",result); EpiSetDword(Diag,L"GpuGmuKeepCx",1);
    GmuText(Text,TextCch,L"gmu_keep_cx",1);
    return result;
}
