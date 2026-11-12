// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - GPU/GMU bring-up probes. New file, not derived from KMDOD.
//
// Clean room: register offsets are hardware facts (mainline Linux gcc-sc7280.c / gpucc-sc7280.c used as a
// datasheet only, no code taken). Nothing here is derived from qcdxkm.
//
// G1a (bit0) reads only. Later opt-in tests temporarily power CX and can write registers/TCM.
// G1a order:
//   1. GCC GPU branch clocks (GCC is always clocked; GCC_GPU_CFG_AHB_CLK is kept on as a critical clock).
//   2. Only if GCC_GPU_CFG_AHB_CLK reports on (CBCR bit31 CLK_OFF == 0): the GPU_CC registers below.
// GPU_CC is reached over GPU_CFG_AHB, so step 2 cannot be attempted with that clock off.
// Before every read the step number is written to Device Parameters\EpiGpuProbeStep and flushed, so if the
// bus hangs the last step survives the forced reboot.

#include "BDD.hxx"
#include "epi_hw.hxx"
#include "epi_gmu.hxx"
extern "C" {
#include <ntstrsafe.h>
}

#pragma code_seg("PAGE")

#define EPI_GCC_BASE            0x00100000ULL
#define EPI_GCC_MAP_OFF         0x71000         // map one page: GPU branch clocks 0x71004..0x71154
#define EPI_GCC_MAP_LEN         0x1000


#define EPI_CBCR_CLK_OFF        0x80000000UL

typedef struct _EPI_GPU_REG { ULONG Offset; PCWSTR Name; } EPI_GPU_REG;

// GCC offsets (from EPI_GCC_BASE)
static const EPI_GPU_REG g_EpiGccRegs[] = {
    { 0x71004, L"gcc_gpu_cfg_ahb"   },   // gate for everything in GPU_CC
    { 0x7100C, L"gcc_gpu_memnoc_gfx"},
    { 0x71018, L"gcc_gpu_snoc_dvm"  },
    { 0x71154, L"gcc_ddrss_gpu_axi" },
};

// GPU_CC offsets (from EPI_GPU_CC_BASE). Status/config registers only; the limiter clear (0x153C) is not read.
static const EPI_GPU_REG g_EpiGpuCcRegs[] = {
    { 0x0000, L"pll0_mode"     },
    { 0x0100, L"pll1_mode"     },
    { 0x1004, L"cxo_aon"       },
    { 0x100C, L"gx_gdscr"      },
    { 0x1064, L"gx_gmu"        },
    { 0x106C, L"cx_gdscr"      },
    { 0x1078, L"ahb"           },
    { 0x107C, L"crc_ahb"       },
    { 0x108C, L"cx_snoc_dvm"   },
    { 0x1090, L"sleep"         },
    { 0x1098, L"cx_gmu"        },
    { 0x109C, L"cxo"           },
    { 0x1120, L"gmu_rcg_cmd"   },
    { 0x1124, L"gmu_rcg_cfg"   },
    { 0x1178, L"hub_aon"       },
    { 0x117C, L"hub_rcg_cmd"   },
    { 0x1180, L"hub_rcg_cfg"   },
    { 0x1204, L"hub_cx_int"    },
    { 0x1534, L"limiter_status"},
    { 0x5000, L"hlos1_vote_smmu"},
};

#define EPI_GPU_NREGS (RTL_NUMBER_OF(g_EpiGccRegs) + RTL_NUMBER_OF(g_EpiGpuCcRegs))
#define EPI_GPU_TEXT_CCH 3072

static VOID EpiGpuStep(HANDLE hParams, ULONG Step)
{
    EpiSetDword(hParams, L"EpiGpuProbeStep", Step);
    ZwFlushKey(hParams);
}

// ---- G1b: CX power-up test (EpiGpuProbe bit1) ----
// G1b writes GPU_CC only (no GCC, no TZ). Optional G1c tests run within this CX window.
// The GMU and hub RCGs stay on XO (their reset source, cfg 0), so no
// PLL or GCC vote is touched. Steps:
//   1. CX GDSC on: clear SW_COLLAPSE (GDSCR bit0), poll PWR_ON (bit31) of GDS_HW_CTRL (0x1540) / GDSCR.
//   2. Set the enable bit (bit0) of the GMU-side branches, poll CLK_OFF (bit31) clear.
//   3. Only if ahb, cxo and cx_gmu run: read a few GMU registers (CX side; offsets are dwords from the GPU base).
//   4. Restore: write back the original CBCR values, then the original GDSCR (collapse), poll PWR_ON clear.
#define EPI_GDSC_SW_COLLAPSE    0x00000001UL
#define EPI_GDSC_PWR_ON         0x80000000UL
#define EPI_CC_CX_GDSCR         0x106C
#define EPI_CC_CX_GDS_HW_CTRL   0x1540
#define EPI_GPU_BASE            0x03D00000ULL
#define EPI_GMU_PAGE_A          0x03D7E000ULL   // CM3 / power / HFI registers (dword 0x1F800..)
#define EPI_GMU_PAGE_B          0x03D8E000ULL   // GMU AO registers (dword 0x23B00..)

typedef struct _EPI_CC_BRANCH { ULONG Offset; PCWSTR Name; BOOLEAN Needed; } EPI_CC_BRANCH;
static const EPI_CC_BRANCH g_EpiCxBranches[] = {
    { 0x1078, L"ahb",             TRUE  },
    { 0x109C, L"cxo",             TRUE  },
    { 0x1098, L"cx_gmu",          TRUE  },
    { 0x1204, L"hub_cx_int",      FALSE },
    { 0x5000, L"hlos1_vote_smmu", FALSE },
};

typedef struct _EPI_GMU_REG { ULONG Dword; PCWSTR Name; } EPI_GMU_REG;
static const EPI_GMU_REG g_EpiGmuRegs[] = {
    { 0x1F800, L"cm3_sysreset"     },
    { 0x1F801, L"cm3_boot_config"  },
    { 0x1F81C, L"cm3_fw_init"      },
    { 0x1F888, L"ao_counter_l"     },
    { 0x1F888, L"ao_counter_l2"    },   // read again after 100 us: does the always-on counter tick?
    { 0x1F8D0, L"sptprac_pwr_clk"  },
    { 0x1F8EC, L"rpmh_power_state" },
    { 0x1F980, L"hfi_ctrl_status"  },
    { 0x23B05, L"ao_host_intr"     },
    { 0x23B13, L"ahb_fence_status" },
};

static VOID EpiCat(PWCHAR Text, PCWSTR Name, ULONG V)
{
    WCHAR line[64];
    RtlStringCchPrintfW(line, RTL_NUMBER_OF(line), L"%s=%08X; ", Name, V);
    RtlStringCchCatW(Text, EPI_GPU_TEXT_CCH, line);
}

// Returns stall loops (x10 us) until (reg & Mask) == Want, or Max + 1 on timeout.
static ULONG EpiPoll(volatile ULONG* Reg, ULONG Mask, ULONG Want, ULONG Max)
{
    for (ULONG i = 0; i <= Max; ++i)
    {
        if ((READ_REGISTER_ULONG(Reg) & Mask) == Want) { return i; }
        KeStallExecutionProcessor(10);
    }
    return Max + 1;
}

// ---- G1c-0 (EpiGpuProbe bit2, read only) ----
// cmd-db (AOP command database, reserved memory outside the Windows RAM map) is copied raw to EpiDiag CmdDb and
// parsed on the PC. Inside the G1b CX window, the GPU RSCC (0x3DE0000) and GPU PDC (0xB290000) state is dumped
// as {phys, value} pairs to EpiDiag GpuRpmh. Offsets are dwords (mainline a6xx_gmu.xml / a6xx.xml, facts only).
#define EPI_CMDDB_BASE          0x80860000ULL
#define EPI_CMDDB_LEN           0x20000
#define EPI_RSCC_BASE           0x03DE0000ULL
#define EPI_RSCC_MAP_LEN        0x2000
#define EPI_RSCC_PROBE_WRITE    0x100UL
#define EPI_GPU_CX_FLAGS        (0xF6UL | EPI_RSCC_PROBE_WRITE)
#define EPI_PDC_BASE            0x0B290000ULL
#define EPI_PDC_MAP_OFF         0x4000          // dwords 0x1000..0x17FF
#define EPI_PDC_MAP_LEN         0x2000
#define EPI_RPMH_MAX_PAIRS      256

typedef struct _EPI_DW_RANGE { ULONG First; ULONG Count; } EPI_DW_RANGE;
static const EPI_DW_RANGE g_EpiRsccRanges[] = {
    { 0x000, 0x10 },    // RSC_STATUS0_DRV0, PDC_SEQ_START/MATCH, SLAVE_ID, HIDDEN_TCS_CMD0
    { 0x082, 0x0B },    // timestamp units
    { 0x100, 0x02 },    // OVERRIDE_START_ADDR, SEQ_BUSY_DRV0
    { 0x180, 0x06 },    // SEQ_MEM_0_DRV0..
    { 0x346, 0x01 },    // TCS0..3 DRV0 status
    { 0x3EE, 0x01 },
    { 0x496, 0x01 },
    { 0x53E, 0x01 },
};
static const EPI_DW_RANGE g_EpiPdcRanges[] = {
    { 0x1140, 0x10 },   // GPU_ENABLE_PDC, GPU_SEQ_START_ADDR ..
    { 0x1570, 0x70 },   // GPU_TCS1..3 control / enable bank / commands
};

typedef struct _EPI_PAIRS { PULONG Buf; ULONG N; } EPI_PAIRS;

static VOID EpiDumpRanges(HANDLE hParams, ULONG Step, EPI_PAIRS* Out, volatile ULONG* Map, ULONGLONG MapPhys,
                          ULONGLONG BlockPhys, const EPI_DW_RANGE* R, ULONG Nr)
{
    for (ULONG i = 0; i < Nr; ++i)
    {
        EpiGpuStep(hParams, Step + i);
        for (ULONG k = 0; k < R[i].Count && Out->N < EPI_RPMH_MAX_PAIRS; ++k)
        {
            const ULONGLONG phys = BlockPhys + (ULONGLONG)(R[i].First + k) * 4;
            Out->Buf[Out->N * 2] = (ULONG)phys;
            Out->Buf[Out->N * 2 + 1] = READ_REGISTER_ULONG(Map + (ULONG)(phys - MapPhys) / 4);
            Out->N++;
        }
    }
}

// Copies cmd-db to EpiDiag CmdDb (trailing zero dwords trimmed). Returns 0 ok, 1 map failed, 2 no memory,
// 3 bad magic (still stored).
static ULONG EpiCmdDbDump(HANDLE hParams, HANDLE hDiag)
{
    ULONG st = 0;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = EPI_CMDDB_BASE;
    EpiGpuStep(hParams, 0x400);
    PVOID map = MmMapIoSpaceEx(pa, EPI_CMDDB_LEN, PAGE_READONLY | PAGE_NOCACHE);
    if (map == NULL) { return 1; }
    PULONG buf = static_cast<PULONG>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_CMDDB_LEN, 'gpEE'));
    if (buf == NULL) { MmUnmapIoSpace(map, EPI_CMDDB_LEN); return 2; }
    volatile ULONG* m = static_cast<volatile ULONG*>(map);
    EpiGpuStep(hParams, 0x401);
    ULONG last = 0;
    for (ULONG i = 0; i < EPI_CMDDB_LEN / 4; ++i)
    {
        buf[i] = READ_REGISTER_ULONG(m + i);
        if (buf[i] != 0) { last = i + 1; }
    }
    MmUnmapIoSpace(map, EPI_CMDDB_LEN);
    if (buf[1] != 0x0C0330DB) { st = 3; }          // magic db 30 03 0c
    EpiSetDword(hDiag, L"CmdDbLen", last * 4);
    if (last != 0) { EpiSetValue(hDiag, L"CmdDb", REG_BINARY, buf, last * 4); }
    ExFreePoolWithTag(buf, 'gpEE');
    return st;
}

// ---- G1c-1 (EpiGpuProbe bit4, read only) ----
// adreno SMMU (MMU1, 0x3DA0000, SMMU-500; register layout from the Arm SMMUv2 architecture / mainline arm-smmu.h,
// facts only). Read inside the CX window with hlos1_vote_gpu_smmu on. Global space 0: sCR0, IDR0..7, sGFSR,
// then SMR/S2CR for the first stream map groups. With DSDT v34 qcsmmu no longer binds MMU1.
#define EPI_ASMMU_BASE          0x03DA0000ULL
#define EPI_ASMMU_MAP_LEN       0x1000
#define EPI_ASMMU_MAX_SMR       32

static VOID EpiSmmuDump(HANDLE hParams, HANDLE hDiag, PWCHAR Text)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = EPI_ASMMU_BASE;
    PVOID map = MmMapIoSpaceEx(pa, EPI_ASMMU_MAP_LEN, PAGE_READONLY | PAGE_NOCACHE);
    if (map == NULL) { EpiCat(Text, L"smmu_map", 0); return; }
    volatile ULONG* g = static_cast<volatile ULONG*>(map);
    ULONG v[16 + 2 * EPI_ASMMU_MAX_SMR] = {};
    EpiGpuStep(hParams, 0x350);
    v[0] = READ_REGISTER_ULONG(g + 0x000 / 4);                       // sCR0
    EpiCat(Text, L"smmu_scr0", v[0]);
    EpiGpuStep(hParams, 0x351);
    for (ULONG i = 0; i < 8; ++i) { v[1 + i] = READ_REGISTER_ULONG(g + (0x020 + i * 4) / 4); }   // IDR0..7
    v[9] = READ_REGISTER_ULONG(g + 0x048 / 4);                       // sGFSR
    EpiCat(Text, L"smmu_idr0", v[1]);
    EpiCat(Text, L"smmu_idr1", v[2]);
    EpiCat(Text, L"smmu_idr2", v[3]);
    EpiCat(Text, L"smmu_idr7", v[8]);
    EpiCat(Text, L"smmu_sgfsr", v[9]);
    ULONG nSmr = v[1] & 0xFF;                                         // IDR0.NUMSMRG
    if (nSmr > EPI_ASMMU_MAX_SMR) { nSmr = EPI_ASMMU_MAX_SMR; }
    EpiGpuStep(hParams, 0x352);
    for (ULONG i = 0; i < nSmr; ++i)
    {
        v[16 + i] = READ_REGISTER_ULONG(g + (0x800 + i * 4) / 4);                     // SMRn
        v[16 + EPI_ASMMU_MAX_SMR + i] = READ_REGISTER_ULONG(g + (0xC00 + i * 4) / 4); // S2CRn
    }
    MmUnmapIoSpace(map, EPI_ASMMU_MAP_LEN);
    EpiSetValue(hDiag, L"GpuSmmu", REG_BINARY, v, sizeof(v));
}

// ---- G1c-2a (bit5): SMMU register access test, no DMA/page tables ----
// Register facts only: Arm SMMUv2 / SMMU-500 and mainline arm-smmu.h.
// IDR1 on this device places GR1 at +0x1000, CB0 at +0x10000.
#define EPI_SMMU_WRITE_LEN 0x17000
typedef struct _EPI_SMMU_WRITE {
    ULONG Offset, Original, Written, Readback, Restored;
} EPI_SMMU_WRITE;

static BOOLEAN EpiSmmuWriteTest(HANDLE hParams, HANDLE hDiag, volatile ULONG* Cc,
                                PVOID GmuA, PWCHAR Text)
{
    EPI_SMMU_WRITE r[] = {
        { 0x10000, 0, 0x106, 0, 0 },    // SCTLR: HUPCF/AFE/TRE; M/CFIE/CFRE off
        { 0x01000, 0, 0x1F300, 0, 0 },  // CBAR0: stage 1, stage 2 bypass, WB, NSH
        { 0x01800, 0, 1, 0, 0 },        // CBA2R0: AArch64
        { 0x00C00, 0, 0, 0, 0 },        // S2CR0: translate, CB0, default privilege
        { 0x00800, 0, 0x84000005, 0, 0 }// SMR0: valid, mask 0x400, SID 5; publish last
    };
    ULONG status = 0, written = 0;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = EPI_ASMMU_BASE;
    EpiGpuStep(hParams, 0x35E);
    // Never reset an already running processor or power down an active GX.
    if (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(GmuA)) != 1 ||
        (READ_REGISTER_ULONG(static_cast<volatile ULONG*>(GmuA) + 0x340 / 4) & 0xC0) != 0xC0 || // GX HM off
        (READ_REGISTER_ULONG(Cc + 0x100C / 4) & 0x80000001UL) != 1 ||
        (READ_REGISTER_ULONG(Cc + 0x5000 / 4) & EPI_CBCR_CLK_OFF) != 0)
    { status = 2; }
    PVOID map = status == 0 ? MmMapIoSpaceEx(pa, EPI_SMMU_WRITE_LEN, PAGE_READWRITE | PAGE_NOCACHE) : NULL;
    if (status == 0 && map == NULL) { status = 1; }
    if (map != NULL)
    {
        volatile ULONG* s = static_cast<volatile ULONG*>(map);
        if (READ_REGISTER_ULONG(s + 0x20 / 4) != 0x4C017E06 ||
            READ_REGISTER_ULONG(s + 0x24 / 4) != 0x30000007 ||
            READ_REGISTER_ULONG(s + 0x28 / 4) != 0x5511 ||
            READ_REGISTER_ULONG(s + 0x3C / 4) != 0x24) { status = 3; }
        if (status == 0)
        {
            // Read ALL originals before writing. Refuse occupied streams/contexts.
            for (ULONG i = 0; i < RTL_NUMBER_OF(r); ++i)
            { r[i].Original = READ_REGISTER_ULONG(s + r[i].Offset / 4); }
            for (ULONG i = 0; i < 6; ++i)
            { if (READ_REGISTER_ULONG(s + (0x800 + i * 4) / 4) & 0x80000000UL) { status = 4; } }
            for (ULONG i = 0; i < 7; ++i)
            { if (READ_REGISTER_ULONG(s + (0x10000 + i * 0x1000) / 4) & 0x61) { status = 4; } }
        }
        if (status == 0)
        {
            for (ULONG i = 0; i < RTL_NUMBER_OF(r); ++i)
            {
                EpiGpuStep(hParams, 0x360 + i);
                WRITE_REGISTER_ULONG(s + r[i].Offset / 4, r[i].Written);
                r[i].Readback = READ_REGISTER_ULONG(s + r[i].Offset / 4);
                ++written;
                if (r[i].Readback != r[i].Written) { status = 5; break; }
            }
            for (ULONG i = written; i-- > 0;)
            {
                EpiGpuStep(hParams, 0x365 + (RTL_NUMBER_OF(r) - 1 - i));
                WRITE_REGISTER_ULONG(s + r[i].Offset / 4, r[i].Original);
                r[i].Restored = READ_REGISTER_ULONG(s + r[i].Offset / 4);
                if (r[i].Restored != r[i].Original) { status = 6; }
            }
        }
        MmUnmapIoSpace(map, EPI_SMMU_WRITE_LEN);
    }
    EpiSetValue(hDiag, L"GpuSmmuW", REG_BINARY, r, sizeof(r));
    EpiSetDword(hDiag, L"GpuSmmuWStatus", status);
    EpiCat(Text, L"smmu_w_status", status);
    EpiCat(Text, L"smr0_before", r[4].Original);
    EpiCat(Text, L"smr0_written", r[4].Readback);
    EpiCat(Text, L"smr0_restored", r[4].Restored);
    return written != 0;
}

// ---- RSCC write/readback/restore test (bit8), CM3 held in reset throughout ----
// Register/value facts: G0_GPU_SURVEY.md section 9.10. No PDC, handshake or RPMH_CTRL writes.
static NTSTATUS EpiRsccStep(HANDLE Params, ULONG Step)
{
    const NTSTATUS st = EpiSetDword(Params, L"EpiGpuProbeStep", Step);
    return NT_SUCCESS(st) ? ZwFlushKey(Params) : st;
}

static ULONG EpiRsccWriteTest(HANDLE Params, HANDLE Diag, volatile ULONG* Cc,
                             PVOID GmuA, PVOID GmuB, PWCHAR Text)
{
    // Diagnostic ABI: ten records of {RSCC dword offset, original, value, readback, restored}.
    struct RSCC_WRITE { ULONG Dword, Original, Value, Readback, Restored; } r[] = {
        {0x004,0,0x01000000,0xFFFFFFFF,0xFFFFFFFF},
        {0x00E,0,0,0xFFFFFFFF,0xFFFFFFFF},
        {0x010,0,0,0xFFFFFFFF,0xFFFFFFFF},
        {0x012,0,0x80000000,0xFFFFFFFF,0xFFFFFFFF},
        {0x100,0,0,0xFFFFFFFF,0xFFFFFFFF},
        {0x180,0,0xEAAAE5A0,0xFFFFFFFF,0xFFFFFFFF},
        {0x181,0,0xE1A1EBAB,0xFFFFFFFF,0xFFFFFFFF},
        {0x182,0,0xA2E0A581,0xFFFFFFFF,0xFFFFFFFF},
        {0x183,0,0xECAC82E2,0xFFFFFFFF,0xFFFFFFFF},
        {0x184,0,0x0020EDAD,0xFFFFFFFF,0xFFFFFFFF}
    };
    // Preserve every HIDDEN_TCS ADDR, PDC slave ID and PDC sequence/match value.
    ULONG status = 0, written = 0, badIndex = 0xFFFFFFFF, restoreBadIndex = 0xFFFFFFFF;
    PVOID map = NULL;
    volatile ULONG* a = static_cast<volatile ULONG*>(GmuA);
    volatile ULONG* b = static_cast<volatile ULONG*>(GmuB);
    const ULONG clocks[] = {0x1078,0x109C,0x1098,0x1204,0x5000};
    if (!NT_SUCCESS(EpiRsccStep(Params, 0x900))) { status = 5; }
    if (status == 0)
    {
        for (ULONG i = 0; i < RTL_NUMBER_OF(clocks); ++i)
        { if (READ_REGISTER_ULONG(Cc + clocks[i]/4) & EPI_CBCR_CLK_OFF) { status = 2; } }
        if (!(READ_REGISTER_ULONG(Cc + EPI_CC_CX_GDSCR/4) & EPI_GDSC_PWR_ON) ||
            READ_REGISTER_ULONG(a) != 1 || (READ_REGISTER_ULONG(a + 0x340/4) & 0xC0) != 0xC0 ||
            (READ_REGISTER_ULONG(Cc + 0x100C/4) & 0x80000001UL) != 1 ||
            READ_REGISTER_ULONG(b + (0x23B07-0x23800)) != 0 ||
            READ_REGISTER_ULONG(b + (0x23B08-0x23800)) != 0) { status = 2; }
    }
    if (status == 0)
    {
        PHYSICAL_ADDRESS pa; pa.QuadPart = EPI_RSCC_BASE;
        map = MmMapIoSpaceEx(pa, EPI_RSCC_MAP_LEN, PAGE_READWRITE | PAGE_NOCACHE);
        if (map == NULL) { status = 1; }
    }
    if (map != NULL)
    {
        volatile ULONG* s = static_cast<volatile ULONG*>(map);
        if (!NT_SUCCESS(EpiRsccStep(Params, 0x901))) { status = 5; }
        if (status == 0 && READ_REGISTER_ULONG(s + 0x101) != 0) { status = 2; }
        // Snapshot ALL targets before any write. Never release or reset CM3 here.
        if (status == 0)
        {
            for (ULONG i = 0; i < RTL_NUMBER_OF(r); ++i)
            { r[i].Original = READ_REGISTER_ULONG(s + r[i].Dword); }
            for (ULONG i = 0; i < RTL_NUMBER_OF(r); ++i)
            {
                if (!NT_SUCCESS(EpiRsccStep(Params, 0x910+i)))
                { status = 5; badIndex = i; break; }
                WRITE_REGISTER_ULONG(s + r[i].Dword, r[i].Value);
                ++written;
                r[i].Readback = READ_REGISTER_ULONG(s + r[i].Dword);
                if (r[i].Readback != r[i].Value) { status = 3; badIndex = i; break; }
            }
            for (ULONG i = written; i-- > 0;)
            {
                // Restoration is mandatory even if breadcrumb persistence fails.
                if (!NT_SUCCESS(EpiRsccStep(Params, 0x930+i)) && status != 4) { status = 5; }
                WRITE_REGISTER_ULONG(s + r[i].Dword, r[i].Original);
                r[i].Restored = READ_REGISTER_ULONG(s + r[i].Dword);
                if (r[i].Restored != r[i].Original)
                {
                    status = 4;
                    if (restoreBadIndex == 0xFFFFFFFF) { restoreBadIndex = i; }
                }
            }
        }
        MmUnmapIoSpace(map, EPI_RSCC_MAP_LEN);
    }
    EpiSetValue(Diag, L"GpuRsccW", REG_BINARY, r, sizeof(r));
    EpiSetDword(Diag, L"GpuRsccWStatus", status);
    EpiSetDword(Diag, L"GpuRsccWCount", written);
    EpiSetDword(Diag, L"GpuRsccWBadIndex", badIndex);
    EpiSetDword(Diag, L"GpuRsccWRestoreBadIndex", restoreBadIndex);
    EpiCat(Text, L"rscc_w_status", status);
    EpiCat(Text, L"rscc_w_count", written);
    EpiCat(Text, L"rscc_w_bad", badIndex);
    return status;
}

// Reopen only after a confirmed collapse; always return clocks and GDSCR to their originals.
// A zero SMR after this cycle cannot prove loss of the test value: it was restored first.
static ULONG EpiSmmuAfterCollapse(HANDLE hParams, HANDLE hDiag, volatile ULONG* Cc,
                                 ULONG GdscOrig, const ULONG* Orig, PWCHAR Text)
{
    ULONG status = 0;
    BOOLEAN branches = FALSE;
    PVOID map = NULL;
    volatile ULONG* gdsc = Cc + EPI_CC_CX_GDSCR / 4;
    volatile ULONG* hw = Cc + EPI_CC_CX_GDS_HW_CTRL / 4;
    EpiGpuStep(hParams, 0x36A);
    WRITE_REGISTER_ULONG(gdsc, GdscOrig & ~EPI_GDSC_SW_COLLAPSE);
    if (EpiPoll(hw, EPI_GDSC_PWR_ON, EPI_GDSC_PWR_ON, 1000) > 1000)
    { status = 1; goto restore; }
    branches = TRUE;
    for (ULONG i = 0; i < RTL_NUMBER_OF(g_EpiCxBranches); ++i)
    {
        volatile ULONG* b = Cc + g_EpiCxBranches[i].Offset / 4;
        EpiGpuStep(hParams, 0x36B + i);
        WRITE_REGISTER_ULONG(b, Orig[i] | 1);
        if (EpiPoll(b, EPI_CBCR_CLK_OFF, 0, 50) > 50) { status = 2; }
    }
    if (status == 0)
    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = EPI_ASMMU_BASE;
        map = MmMapIoSpaceEx(pa, EPI_ASMMU_MAP_LEN, PAGE_READONLY | PAGE_NOCACHE);
        if (map == NULL) { status = 3; }
        else
        {
            EpiGpuStep(hParams, 0x35F);
            const ULONG smr = READ_REGISTER_ULONG(static_cast<volatile ULONG*>(map) + 0x800 / 4);
            EpiSetDword(hDiag, L"GpuSmmuWAfterSmr0", smr);
            EpiCat(Text, L"smr0_after_cycle", smr);
        }
    }
restore:
    if (map != NULL) { MmUnmapIoSpace(map, EPI_ASMMU_MAP_LEN); }
    if (branches)
    {
        for (ULONG i = RTL_NUMBER_OF(g_EpiCxBranches); i-- > 0;)
        {
            EpiGpuStep(hParams, 0x341 + (RTL_NUMBER_OF(g_EpiCxBranches) - 1 - i));
            WRITE_REGISTER_ULONG(Cc + g_EpiCxBranches[i].Offset / 4, Orig[i]);
        }
    }
    EpiGpuStep(hParams, 0x346);
    WRITE_REGISTER_ULONG(gdsc, GdscOrig);
    if (EpiPoll(hw, EPI_GDSC_PWR_ON, 0, 1000) > 1000) { status = 4; }
    EpiSetDword(hDiag, L"GpuSmmuWCycleStatus", status);
    EpiCat(Text, L"smmu_cycle_status", status);
    return status;
}

// ---- G1c-2b (EpiGpuProbe bit6): GMU firmware TCM load test ----
// The GMU firmware (a660_gmu.bin from the stock vendor image, used as data) is a list of blocks
// { addr, size, type, value, data[size] } (format facts from mainline a6xx_gmu.c). Blocks with size 0 describe
// regions. GMU address 0x0..0x3FFF is the ITCM (written through dword 0x1B400), 0x10004000..0x10007FFF the DTCM
// on the a650 family (dword 0x1C400). Everything else (icache at 0x4000, dummy at 0x60000000) lives in memory
// behind the adreno SMMU and is only counted here. The CM3 stays in reset: no boot register is written.
// The file is read before the CX window; the TCM writes run inside it and are read back.
#define EPI_GMU_FW_PATH         L"\\DosDevices\\C:\\Epinephrine\\fw\\a660_gmu.bin"
#define EPI_GMU_FW_MAX          0x40000
#define EPI_GMU_ITCM_PHYS       (EPI_GPU_BASE + 0x1B400ULL * 4)   // 0x3D6D000
#define EPI_GMU_DTCM_PHYS       (EPI_GPU_BASE + 0x1C400ULL * 4)   // 0x3D71000
#define EPI_GMU_TCM_LEN         0x4000
#define EPI_GMU_ITCM_ADDR       0x00000000UL
#define EPI_GMU_DTCM_ADDR       0x10004000UL
#define EPI_GMU_FW_VER_DTCM_OFF 0x3FE0          // CORE_FW_VERSION (dword 0x1D3F8) sits in the DTCM
#define EPI_GMU_SYSRESET_DWORD  0x1F800

typedef struct _EPI_GMU_FW
{
    PUCHAR Data;
    ULONG Len;
    ULONG TcmBlocks;
    ULONG ItcmBytes;
    ULONG DtcmBytes;
    ULONG SmmuBytes;
    ULONG ExpectedVer;      // DTCM word at EPI_GMU_FW_VER_DTCM_OFF after the load, if a block covers it
} EPI_GMU_FW;

typedef struct _EPI_GMU_BLK { ULONG Addr; ULONG Size; ULONG Type; ULONG Value; } EPI_GMU_BLK;

static BOOLEAN EpiGmuInTcm(ULONG Addr, ULONG Size, ULONG Base)
{
    return (Addr >= Base && Size <= EPI_GMU_TCM_LEN && Addr - Base <= EPI_GMU_TCM_LEN - Size) ? TRUE : FALSE;
}

// Walks the block list. Returns 0 ok, 3 malformed. Fills the counters in Fw.
static ULONG EpiGmuFwParse(EPI_GMU_FW* Fw)
{
    ULONG off = 0;
    while (off < Fw->Len)
    {
        if (Fw->Len - off < sizeof(EPI_GMU_BLK)) { return 3; }
        const EPI_GMU_BLK* b = reinterpret_cast<const EPI_GMU_BLK*>(Fw->Data + off);
        const ULONG dataOff = off + sizeof(EPI_GMU_BLK);
        if (b->Size > Fw->Len - dataOff || (b->Size & 3) != 0 || (b->Addr & 3) != 0) { return 3; }
        if (b->Size != 0)
        {
            if (EpiGmuInTcm(b->Addr, b->Size, EPI_GMU_ITCM_ADDR)) { Fw->ItcmBytes += b->Size; ++Fw->TcmBlocks; }
            else if (EpiGmuInTcm(b->Addr, b->Size, EPI_GMU_DTCM_ADDR))
            {
                Fw->DtcmBytes += b->Size;
                ++Fw->TcmBlocks;
                const ULONG verAddr = EPI_GMU_DTCM_ADDR + EPI_GMU_FW_VER_DTCM_OFF;
                if (verAddr >= b->Addr && verAddr - b->Addr < b->Size)
                {
                    Fw->ExpectedVer = *reinterpret_cast<const ULONG*>(Fw->Data + dataOff + (verAddr - b->Addr));
                }
            }
            else { Fw->SmmuBytes += b->Size; }
        }
        off = dataOff + b->Size;
    }
    return 0;
}

// Reads and parses the firmware file. Returns 0 ok, 1 open failed, 2 bad size / read failed, 3 malformed,
// 9 no memory. On success Fw->Data is a 'gpEE' pool buffer owned by the caller.
static ULONG EpiGmuFwRead(HANDLE hParams, EPI_GMU_FW* Fw)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    FILE_STANDARD_INFORMATION info = {};
    ULONG st = 0;

    EpiGpuStep(hParams, 0x410);
    RtlInitUnicodeString(&path, EPI_GMU_FW_PATH);
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                                 NULL, 0)))
    {
        return 1;
    }
    if (!NT_SUCCESS(ZwQueryInformationFile(h, &iosb, &info, sizeof(info), FileStandardInformation)) ||
        info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > EPI_GMU_FW_MAX)
    {
        st = 2;
        goto out;
    }
    Fw->Len = (ULONG)info.EndOfFile.QuadPart;
    Fw->Data = static_cast<PUCHAR>(ExAllocatePool2(POOL_FLAG_PAGED, Fw->Len, 'gpEE'));
    if (Fw->Data == NULL) { st = 9; goto out; }
    {
        LARGE_INTEGER pos;
        pos.QuadPart = 0;
        if (!NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, Fw->Data, Fw->Len, &pos, NULL)) ||
            iosb.Information != Fw->Len)
        {
            st = 2;
            goto out;
        }
    }
    EpiGpuStep(hParams, 0x411);
    st = EpiGmuFwParse(Fw);
out:
    if (st != 0 && Fw->Data != NULL) { ExFreePoolWithTag(Fw->Data, 'gpEE'); Fw->Data = NULL; }
    ZwClose(h);
    return st;
}

// Inside the CX window (GMU branches on). GmuA maps the CM3 page (dword 0x1F800 at offset 0).
// Returns 0 loaded and verified, 4 CM3 not in reset (nothing written), 5 map failed, 6 read-back mismatch.
static ULONG EpiGmuFwLoadTcm(HANDLE hParams, const EPI_GMU_FW* Fw, PVOID GmuA, PWCHAR Text)
{
    const ULONG sysreset = READ_REGISTER_ULONG(static_cast<volatile ULONG*>(GmuA));
    EpiCat(Text, L"fw_cm3_sysreset", sysreset);
    if ((sysreset & 1) == 0) { return 4; }

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = EPI_GMU_ITCM_PHYS;
    PVOID itcm = MmMapIoSpaceEx(pa, EPI_GMU_TCM_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = EPI_GMU_DTCM_PHYS;
    PVOID dtcm = MmMapIoSpaceEx(pa, EPI_GMU_TCM_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    ULONG st = 0;
    if (itcm == NULL || dtcm == NULL) { st = 5; goto out; }
    {
        volatile ULONG* it = static_cast<volatile ULONG*>(itcm);
        volatile ULONG* dt = static_cast<volatile ULONG*>(dtcm);
        EpiCat(Text, L"fw_itcm0_before", READ_REGISTER_ULONG(it));
        EpiCat(Text, L"fw_ver_before", READ_REGISTER_ULONG(dt + EPI_GMU_FW_VER_DTCM_OFF / 4));

        ULONG mismatch = 0;
        for (ULONG pass = 0; pass < 2; ++pass)          // pass 0 writes, pass 1 reads back
        {
            EpiGpuStep(hParams, 0x370 + pass);
            ULONG off = 0;
            while (off < Fw->Len)
            {
                const EPI_GMU_BLK* b = reinterpret_cast<const EPI_GMU_BLK*>(Fw->Data + off);
                const ULONG* src = reinterpret_cast<const ULONG*>(Fw->Data + off + sizeof(EPI_GMU_BLK));
                volatile ULONG* dst = NULL;
                if (b->Size != 0 && EpiGmuInTcm(b->Addr, b->Size, EPI_GMU_ITCM_ADDR))
                {
                    dst = it + (b->Addr - EPI_GMU_ITCM_ADDR) / 4;
                }
                else if (b->Size != 0 && EpiGmuInTcm(b->Addr, b->Size, EPI_GMU_DTCM_ADDR))
                {
                    dst = dt + (b->Addr - EPI_GMU_DTCM_ADDR) / 4;
                }
                if (dst != NULL)
                {
                    for (ULONG i = 0; i < b->Size / 4; ++i)
                    {
                        if (pass == 0) { WRITE_REGISTER_ULONG(dst + i, src[i]); }
                        else if (READ_REGISTER_ULONG(dst + i) != src[i]) { ++mismatch; }
                    }
                }
                off += sizeof(EPI_GMU_BLK) + b->Size;
            }
        }
        EpiGpuStep(hParams, 0x372);
        const ULONG ver = READ_REGISTER_ULONG(dt + EPI_GMU_FW_VER_DTCM_OFF / 4);
        EpiCat(Text, L"fw_mismatch", mismatch);
        EpiCat(Text, L"fw_ver", ver);
        EpiCat(Text, L"fw_ver_expected", Fw->ExpectedVer);
        if (mismatch != 0) { st = 6; }
    }
out:
    if (dtcm != NULL) { MmUnmapIoSpace(dtcm, EPI_GMU_TCM_LEN); }
    if (itcm != NULL) { MmUnmapIoSpace(itcm, EPI_GMU_TCM_LEN); }
    return st;
}

// GpuCxStatus: 0 done (GMU read), 10 CX GDSC did not power on, 11 a needed branch stayed off (GMU not read),
//              12 map failed, 13 CX GDSC did not collapse again on restore,
//              14 SMMU/CM3 boot/RSCC write test needs initially collapsed CX, 15 post-collapse SMMU read cycle failed,
//              16 GMU boot test failed (GpuGmuStatus contains the reason), 17 RSCC write test failed
static ULONG EpiGpuCxTest(HANDLE hParams, HANDLE hDiag, ULONG Flags, volatile ULONG* Cc, PWCHAR Text,
                          const EPI_GMU_FW* Fw, PULONG FwStatus, BOOLEAN* KeepCx)
{
    *KeepCx = FALSE;
    ULONG status = 0;
    ULONG orig[RTL_NUMBER_OF(g_EpiCxBranches)] = {};
    BOOLEAN touched = FALSE;
    PVOID gmuA = NULL;
    PVOID gmuB = NULL;
    BOOLEAN allNeeded = TRUE;
    BOOLEAN smmuWritten = FALSE;
    ULONG t = 0;

    volatile ULONG* gdscr = Cc + EPI_CC_CX_GDSCR / 4;
    volatile ULONG* hwctl = Cc + EPI_CC_CX_GDS_HW_CTRL / 4;
    const ULONG gdscOrig = READ_REGISTER_ULONG(gdscr);
    EpiCat(Text, L"cx_gdscr0", gdscOrig);
    EpiCat(Text, L"cx_hwctl0", READ_REGISTER_ULONG(hwctl));

    if (Flags & (0x20 | EPI_GMU_PROBE_BOOT))
    {
        EpiSetDword(hDiag, L"GpuSmmuWStatus", 7);
        EpiSetDword(hDiag, L"GpuSmmuWCycleStatus", 7);
        EpiSetDword(hDiag, L"GpuSmmuWAfterSmr0", 0xFFFFFFFF);
    }
    if (Flags & (0x20 | EPI_GMU_PROBE_BOOT | EPI_RSCC_PROBE_WRITE))
    {
        if ((gdscOrig & (EPI_GDSC_PWR_ON | EPI_GDSC_SW_COLLAPSE)) != EPI_GDSC_SW_COLLAPSE)
        { return 14; }
    }

    // 1. CX GDSC on
    EpiGpuStep(hParams, 0x300);
    WRITE_REGISTER_ULONG(gdscr, gdscOrig & ~EPI_GDSC_SW_COLLAPSE);
    t = EpiPoll(hwctl, EPI_GDSC_PWR_ON, EPI_GDSC_PWR_ON, 1000);
    EpiCat(Text, L"cx_on_x10us", t);
    EpiCat(Text, L"cx_gdscr1", READ_REGISTER_ULONG(gdscr));
    EpiCat(Text, L"cx_hwctl1", READ_REGISTER_ULONG(hwctl));
    if (t > 1000 && (READ_REGISTER_ULONG(gdscr) & EPI_GDSC_PWR_ON) == 0) { status = 10; goto restore; }

    // 2. branches
    touched = TRUE;
    for (ULONG i = 0; i < RTL_NUMBER_OF(g_EpiCxBranches); ++i)
    {
        volatile ULONG* cbcr = Cc + g_EpiCxBranches[i].Offset / 4;
        EpiGpuStep(hParams, 0x310 + i);
        orig[i] = READ_REGISTER_ULONG(cbcr);
        WRITE_REGISTER_ULONG(cbcr, orig[i] | 1);
        t = EpiPoll(cbcr, EPI_CBCR_CLK_OFF, 0, 50);
        EpiCat(Text, g_EpiCxBranches[i].Name, READ_REGISTER_ULONG(cbcr));
        if (t > 50 && g_EpiCxBranches[i].Needed) { allNeeded = FALSE; }
    }
    EpiCat(Text, L"gmu_rcg_cmd", READ_REGISTER_ULONG(Cc + 0x1120 / 4));
    if (!allNeeded) { status = 11; goto restore; }

    // 3. GMU registers
    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = EPI_GMU_PAGE_A;
        gmuA = MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
        pa.QuadPart = EPI_GMU_PAGE_B;
        gmuB = MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
        if (gmuA == NULL || gmuB == NULL) { status = 12; goto restore; }
        for (ULONG i = 0; i < RTL_NUMBER_OF(g_EpiGmuRegs); ++i)
        {
            const ULONGLONG phys = EPI_GPU_BASE + (ULONGLONG)g_EpiGmuRegs[i].Dword * 4;
            const BOOLEAN inB = (phys >= EPI_GMU_PAGE_B) ? TRUE : FALSE;
            volatile ULONG* base = static_cast<volatile ULONG*>(inB ? gmuB : gmuA);
            const ULONG off = (ULONG)(phys - (inB ? EPI_GMU_PAGE_B : EPI_GMU_PAGE_A));
            EpiGpuStep(hParams, 0x320 + i);
            EpiCat(Text, g_EpiGmuRegs[i].Name, READ_REGISTER_ULONG(base + off / 4));
            if (i == 3) { KeStallExecutionProcessor(100); }
        }
    }

    // Independent trial: no SMMU/TCM/CM3 boot or PDC probes in this CX window.
    if (Flags & EPI_RSCC_PROBE_WRITE)
    {
        if (EpiRsccWriteTest(hParams, hDiag, Cc, gmuA, gmuB, Text) != 0) { status = 17; }
        goto restore;
    }

    // 3a. G1c-1: adreno SMMU ID / stream mapping (read only)
    if (Flags & 0x10) { EpiSmmuDump(hParams, hDiag, Text); }

    // 3a. G1c-2a: write/read-back/restore while CM3 is reset and GX is off.
    if (Flags & 0x20) { smmuWritten = EpiSmmuWriteTest(hParams, hDiag, Cc, gmuA, Text); }

    // 3a'. G1c-2b: GMU firmware ITCM/DTCM load + read-back (CM3 stays in reset)
    if ((Flags & 0x40) && Fw->Data != NULL) { *FwStatus = EpiGmuFwLoadTcm(hParams, Fw, gmuA, Text); }

    if (Flags & EPI_GMU_PROBE_BOOT)
    {
        const ULONG gmuStatus = EpiGmuBootTest(hParams, hDiag, Cc, Fw->Data, Fw->Len,
                                               Text, EPI_GPU_TEXT_CCH, KeepCx,
                                               (Flags & EPI_GMU_PROBE_HFI_START) ? TRUE : FALSE,
                                               (Flags & EPI_GMU_PROBE_GX_VOTE) ? TRUE : FALSE,
                                               (Flags & EPI_GMU_PROBE_CLOCK) ? TRUE : FALSE,
                                               (Flags & EPI_GMU_PROBE_GX_ON) ? TRUE : FALSE,
                                               (Flags & EPI_GMU_PROBE_GX_REG) ? TRUE : FALSE,
                                               (Flags & EPI_GMU_PROBE_GX_HALT) ? TRUE : FALSE,
                                               (Flags & EPI_GMU_PROBE_SECVID) ? TRUE : FALSE,
                                               (Flags & EPI_GMU_PROBE_ADDR_MODE) ? TRUE : FALSE);
        EpiSetDword(hDiag, L"GpuGmuStatus", gmuStatus);
        if (gmuStatus != EPI_GMU_OK) { status = 16; }
        // No further probes after boot, especially when teardown retained the CX window.
        goto restore;
    }

    // 3b. G1c-0: RSCC / GPU PDC state (read only)
    if (Flags & 4)
    {
        EPI_PAIRS out = {};
        out.Buf = static_cast<PULONG>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_RPMH_MAX_PAIRS * 2 * sizeof(ULONG), 'gpEE'));
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = EPI_RSCC_BASE;
        PVOID rscc = MmMapIoSpaceEx(pa, EPI_RSCC_MAP_LEN, PAGE_READONLY | PAGE_NOCACHE);
        pa.QuadPart = EPI_PDC_BASE + EPI_PDC_MAP_OFF;
        PVOID pdc = MmMapIoSpaceEx(pa, EPI_PDC_MAP_LEN, PAGE_READONLY | PAGE_NOCACHE);
        if (out.Buf != NULL && rscc != NULL && pdc != NULL)
        {
            EpiDumpRanges(hParams, 0x330, &out, static_cast<volatile ULONG*>(rscc), EPI_RSCC_BASE, EPI_RSCC_BASE,
                          g_EpiRsccRanges, RTL_NUMBER_OF(g_EpiRsccRanges));
            // GPU PDC read: EpiGpuProbe bit3 only. Reading GPU_ENABLE_PDC (0xB294500) reset the phone on
            // 2026-11-07 (breadcrumb 0x338), probably a hypervisor stage-2 fault.
            if (Flags & 8)
            {
                EpiDumpRanges(hParams, 0x338, &out, static_cast<volatile ULONG*>(pdc), EPI_PDC_BASE + EPI_PDC_MAP_OFF,
                              EPI_PDC_BASE, g_EpiPdcRanges, RTL_NUMBER_OF(g_EpiPdcRanges));
            }
            // GMU side RSCC handshake registers (GMU page B)
            volatile ULONG* b = static_cast<volatile ULONG*>(gmuB);
            EpiCat(Text, L"rscc_ctrl_req", READ_REGISTER_ULONG(b + (0x23B07 * 4 - 0x8E000) / 4));
            EpiCat(Text, L"rscc_ctrl_ack", READ_REGISTER_ULONG(b + (0x23B08 * 4 - 0x8E000) / 4));
            EpiSetValue(hDiag, L"GpuRpmh", REG_BINARY, out.Buf, out.N * 2 * sizeof(ULONG));
        }
        EpiCat(Text, L"rpmh_pairs", out.N);
        if (pdc != NULL) { MmUnmapIoSpace(pdc, EPI_PDC_MAP_LEN); }
        if (rscc != NULL) { MmUnmapIoSpace(rscc, EPI_RSCC_MAP_LEN); }
        if (out.Buf != NULL) { ExFreePoolWithTag(out.Buf, 'gpEE'); }
    }

restore:
    if (*KeepCx)
    {
        // EpiGmuBootTest owns quarantined memory; its SMMU ctx borrows Cc.
        // Leave every branch/CX ON and keep the outer GPU_CC mapping alive too.
        EpiCat(Text, L"cx_kept", 1);
        if (gmuB != NULL) { MmUnmapIoSpace(gmuB, PAGE_SIZE); }
        if (gmuA != NULL) { MmUnmapIoSpace(gmuA, PAGE_SIZE); }
        EpiGpuStep(hParams, 0x6FF);
        return status;
    }
    // 4. restore: branches in reverse order (original values; CLK_OFF is read-only), then collapse CX
    EpiGpuStep(hParams, 0x340);
    if (touched)
    {
        for (ULONG i = RTL_NUMBER_OF(g_EpiCxBranches); i-- > 0;)
        {
            EpiGpuStep(hParams, 0x341 + (RTL_NUMBER_OF(g_EpiCxBranches) - 1 - i));
            WRITE_REGISTER_ULONG(Cc + g_EpiCxBranches[i].Offset / 4, orig[i]);
        }
    }
    EpiGpuStep(hParams, 0x346);
    WRITE_REGISTER_ULONG(gdscr, gdscOrig);
    t = EpiPoll(hwctl, EPI_GDSC_PWR_ON, 0, 1000);
    EpiCat(Text, L"cx_off_x10us", t);
    EpiCat(Text, L"cx_gdscr2", READ_REGISTER_ULONG(gdscr));
    EpiCat(Text, L"cx_hwctl2", READ_REGISTER_ULONG(hwctl));
    if (t > 1000 && status == 0) { status = 13; }
    if (smmuWritten && t <= 1000 && status == 0)
    {
        if (EpiSmmuAfterCollapse(hParams, hDiag, Cc, gdscOrig, orig, Text) != 0) { status = 15; }
    }
    if (gmuB != NULL) { MmUnmapIoSpace(gmuB, PAGE_SIZE); }
    if (gmuA != NULL) { MmUnmapIoSpace(gmuA, PAGE_SIZE); }
    EpiGpuStep(hParams, 0x3FF);
    return status;
}

VOID EpiGpuProbe(_In_ const EPI_HW_RESOURCES* pRes, _In_ PDEVICE_OBJECT Pdo)
{
    PAGED_CODE();
    HANDLE hParams = NULL;
    HANDLE hDiag = NULL;
    if (!NT_SUCCESS(EpiOpenDiag(Pdo, &hParams, &hDiag))) { return; }

    // GpuProbeStatus: 0 done, 1 not enabled, 2 GPU_CC not in _CRS, 3 no memory, 4 map failed,
    //                 5 GCC_GPU_CFG_AHB_CLK off (GPU_CC not read), 6 incompatible probe flags,
    //                 7 RSCC/HFI modifier one-shot persistence failed (no hardware access)
    ULONG status = 0;
    ULONG n = 0;
    PULONG pairs = NULL;
    PWCHAR text = NULL;
    PVOID gcc = NULL;
    PVOID gpucc = NULL;
    ULONG flags = 0;
    ULONG cxStatus = 0xFFFFFFFF;
    PWCHAR cxText = NULL;
    EPI_GMU_FW fw = {};
    ULONG fwStatus = 0xFFFFFFFF;
    BOOLEAN keepCx = FALSE;

    // One shot: cleared and flushed before any hardware access, so a hang cannot repeat at the next start.
    // EpiGpuProbe bit0 = G1a read, bit1 = G1b CX power-up test (writes GPU_CC; implies the read),
    // bit2 = G1c-0: cmd-db copy + RSCC dump (runs inside the G1b CX window, so it implies bit1),
    // bit3 = also read GPU PDC (resets the phone, do not use), bit4 = G1c-1 adreno SMMU read (CX window),
    // bit5 = G1c-2a SMMU write/read-back/restore, bit6 = G1c-2b TCM load,
    // bit7 = G1c-3 CM3 boot/stop through SMMU (same firmware path, CX window; no PDC),
    // bit8 = independent RSCC write/read-back/restore, CM3 reset; only bits0/1 may accompany it,
    // bit9 = with bit7 (0x280), extend HFI with CORE_FW_START / START; bit9 alone is rejected,
    // bit10 = requires bits7/9 (0x680), add GBIF QSB and GX_BW_PERF_VOTE,
    // bit11 = requires bits7/9/10 (0xE80), add GMU/hub clock setup/restore,
    // bit12 = requires bits7/9/10/11 (0x1E80), add OOB GPU_SET and slumber/rpmh_stop,
    // bit13 = requires bit12 and its prerequisites (0x3E80), read six GX registers once before OOB release,
    // bit14 = requires bit13 and its prerequisites (0x7E80), clear CX/RBBM GBIF halt after those reads.
    // bit15 = requires bit14 and its prerequisites (0xFE80), SECVID stage A.
    // bit16 = requires bit15 and its prerequisites (0x1FE80), ADDR_MODE stage B.
    flags = EpiQueryDword(hParams, L"EpiGpuProbe", 0);
    if (flags == 0) { status = 1; goto done; }
    if (flags & EPI_RSCC_PROBE_WRITE)
    {
        EpiSetDword(hDiag, L"GpuRsccWStatus", 7);
        EpiSetValue(hDiag, L"GpuRsccW", REG_BINARY, NULL, 0);
        EpiSetDword(hDiag, L"GpuRsccWCount", 0);
        EpiSetDword(hDiag, L"GpuRsccWBadIndex", 0xFFFFFFFF);
        EpiSetDword(hDiag, L"GpuRsccWRestoreBadIndex", 0xFFFFFFFF);
        const NTSTATUS cleared = EpiSetDword(hParams, L"EpiGpuProbe", 0);
        if (!NT_SUCCESS(cleared) || !NT_SUCCESS(ZwFlushKey(hParams))) { status = 7; goto done; }
        if (flags & ~(EPI_RSCC_PROBE_WRITE | 3UL))
        { EpiSetDword(hDiag, L"GpuRsccWStatus", 6); status = 6; goto done; }
    }
    else if (flags & (EPI_GMU_PROBE_HFI_START | EPI_GMU_PROBE_GX_VOTE | EPI_GMU_PROBE_CLOCK |
                      EPI_GMU_PROBE_GX_ON | EPI_GMU_PROBE_GX_REG | EPI_GMU_PROBE_GX_HALT |
                      EPI_GMU_PROBE_SECVID | EPI_GMU_PROBE_ADDR_MODE))
    {
        const NTSTATUS cleared = EpiSetDword(hParams, L"EpiGpuProbe", 0);
        if (!NT_SUCCESS(cleared) || !NT_SUCCESS(ZwFlushKey(hParams))) { status = 7; goto done; }
        if (!(flags & EPI_GMU_PROBE_BOOT) ||
            ((flags & EPI_GMU_PROBE_GX_VOTE) && !(flags & EPI_GMU_PROBE_HFI_START)) ||
            ((flags & EPI_GMU_PROBE_CLOCK) &&
             (flags & (EPI_GMU_PROBE_HFI_START | EPI_GMU_PROBE_GX_VOTE)) !=
                      (EPI_GMU_PROBE_HFI_START | EPI_GMU_PROBE_GX_VOTE)) ||
            ((flags & EPI_GMU_PROBE_GX_ON) &&
             (flags & (EPI_GMU_PROBE_HFI_START | EPI_GMU_PROBE_GX_VOTE | EPI_GMU_PROBE_CLOCK)) !=
                      (EPI_GMU_PROBE_HFI_START | EPI_GMU_PROBE_GX_VOTE | EPI_GMU_PROBE_CLOCK)) ||
            ((flags & EPI_GMU_PROBE_GX_REG) && !(flags & EPI_GMU_PROBE_GX_ON)) ||
            ((flags & EPI_GMU_PROBE_GX_HALT) && !(flags & EPI_GMU_PROBE_GX_REG)) ||
            ((flags & EPI_GMU_PROBE_SECVID) && !(flags & EPI_GMU_PROBE_GX_HALT)) ||
            ((flags & EPI_GMU_PROBE_ADDR_MODE) && !(flags & EPI_GMU_PROBE_SECVID)))
        { status = 6; goto done; }
    }
    else { EpiSetDword(hParams, L"EpiGpuProbe", 0); }
    EpiGpuStep(hParams, 1);
    if (!pRes->GpuCcFound) { status = 2; goto done; }

    pairs = static_cast<PULONG>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_GPU_NREGS * 2 * sizeof(ULONG), 'gpEE'));
    text = static_cast<PWCHAR>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_GPU_TEXT_CCH * sizeof(WCHAR), 'gpEE'));
    cxText = static_cast<PWCHAR>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_GPU_TEXT_CCH * sizeof(WCHAR), 'gpEE'));
    if (pairs == NULL || text == NULL || cxText == NULL) { status = 3; goto done; }
    text[0] = L'\0';
    cxText[0] = L'\0';
    if (flags & 4) { EpiSetDword(hDiag, L"CmdDbStatus", EpiCmdDbDump(hParams, hDiag)); }
    if (flags & (0x40 | EPI_GMU_PROBE_BOOT))
    {
        fwStatus = EpiGmuFwRead(hParams, &fw);
        EpiCat(cxText, L"fw_len", fw.Len);
        EpiCat(cxText, L"fw_tcm_blocks", fw.TcmBlocks);
        EpiCat(cxText, L"fw_itcm_bytes", fw.ItcmBytes);
        EpiCat(cxText, L"fw_dtcm_bytes", fw.DtcmBytes);
        EpiCat(cxText, L"fw_smmu_bytes", fw.SmmuBytes);
    }

    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = EPI_GCC_BASE + EPI_GCC_MAP_OFF;
        gcc = MmMapIoSpaceEx(pa, EPI_GCC_MAP_LEN, PAGE_READONLY | PAGE_NOCACHE);
        pa.QuadPart = EPI_GPU_CC_BASE;
        gpucc = MmMapIoSpaceEx(pa, (SIZE_T)EPI_GPU_CC_LEN, ((flags & EPI_GPU_CX_FLAGS) ? PAGE_READWRITE : PAGE_READONLY) | PAGE_NOCACHE);
        if (gcc == NULL || gpucc == NULL) { status = 4; goto done; }
    }

    {
        volatile ULONG* g = static_cast<volatile ULONG*>(gcc);
        ULONG ahb = EPI_CBCR_CLK_OFF;
        for (ULONG i = 0; i < RTL_NUMBER_OF(g_EpiGccRegs); ++i)
        {
            const ULONG off = g_EpiGccRegs[i].Offset;
            EpiGpuStep(hParams, 0x100 + i);
            const ULONG v = READ_REGISTER_ULONG(g + (off - EPI_GCC_MAP_OFF) / 4);
            if (i == 0) { ahb = v; }
            pairs[n * 2] = (ULONG)EPI_GCC_BASE + off;
            pairs[n * 2 + 1] = v;
            ++n;
            WCHAR line[64];
            RtlStringCchPrintfW(line, RTL_NUMBER_OF(line), L"%s=%08X; ", g_EpiGccRegs[i].Name, v);
            RtlStringCchCatW(text, EPI_GPU_TEXT_CCH, line);
        }
        if (ahb & EPI_CBCR_CLK_OFF) { status = 5; goto done; }

        volatile ULONG* c = static_cast<volatile ULONG*>(gpucc);
        for (ULONG i = 0; i < RTL_NUMBER_OF(g_EpiGpuCcRegs); ++i)
        {
            const ULONG off = g_EpiGpuCcRegs[i].Offset;
            EpiGpuStep(hParams, 0x200 + i);
            const ULONG v = READ_REGISTER_ULONG(c + off / 4);
            pairs[n * 2] = (ULONG)EPI_GPU_CC_BASE + off;
            pairs[n * 2 + 1] = v;
            ++n;
            WCHAR line[64];
            RtlStringCchPrintfW(line, RTL_NUMBER_OF(line), L"%s=%08X; ", g_EpiGpuCcRegs[i].Name, v);
            RtlStringCchCatW(text, EPI_GPU_TEXT_CCH, line);
        }
        if (flags & EPI_GPU_CX_FLAGS) { cxStatus = EpiGpuCxTest(hParams, hDiag, flags, c, cxText, &fw, &fwStatus, &keepCx); }
    }

done:
    if (status != 1) { EpiGpuStep(hParams, 0xFFFF); }     // 0xFFFF = probe returned
    if (gpucc != NULL && !keepCx) { MmUnmapIoSpace(gpucc, (SIZE_T)EPI_GPU_CC_LEN); }
    if (gcc != NULL) { MmUnmapIoSpace(gcc, EPI_GCC_MAP_LEN); }
    if (pairs != NULL && n != 0) { EpiSetValue(hDiag, L"GpuProbe", REG_BINARY, pairs, n * 2 * sizeof(ULONG)); }
    if (text != NULL && text[0] != L'\0')
    {
        size_t cch = 0;
        RtlStringCchLengthW(text, EPI_GPU_TEXT_CCH, &cch);
        EpiSetValue(hDiag, L"GpuProbeText", REG_SZ, text, (ULONG)((cch + 1) * sizeof(WCHAR)));
    }
    EpiSetDword(hDiag, L"GpuProbeStatus", status);
    if (cxStatus != 0xFFFFFFFF) { EpiSetDword(hDiag, L"GpuCxStatus", cxStatus); }
    // GpuFwStatus: 0 TCM loaded + verified, 1 open failed, 2 bad size / read, 3 malformed, 4 CM3 not in reset,
    //              5 TCM map failed, 6 read-back mismatch, 9 no memory
    if (fwStatus != 0xFFFFFFFF) { EpiSetDword(hDiag, L"GpuFwStatus", fwStatus); }
    if (fw.Data != NULL) { ExFreePoolWithTag(fw.Data, 'gpEE'); }
    if (cxText != NULL && cxText[0] != L'\0')
    {
        size_t cch = 0;
        RtlStringCchLengthW(cxText, EPI_GPU_TEXT_CCH, &cch);
        EpiSetValue(hDiag, L"GpuCxText", REG_SZ, cxText, (ULONG)((cch + 1) * sizeof(WCHAR)));
    }
    if (cxText != NULL) { ExFreePoolWithTag(cxText, 'gpEE'); }
    if (text != NULL) { ExFreePoolWithTag(text, 'gpEE'); }
    if (pairs != NULL) { ExFreePoolWithTag(pairs, 'gpEE'); }
    ZwClose(hDiag);
    ZwClose(hParams);
}
