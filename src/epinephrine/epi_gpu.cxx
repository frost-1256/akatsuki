// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - GPU G1a: read-only GPU_CC state probe. New file, not derived from KMDOD.
//
// Clean room: register offsets are hardware facts (mainline Linux gcc-sc7280.c / gpucc-sc7280.c used as a
// datasheet only, no code taken). Nothing here is derived from qcdxkm.
//
// Reads only, no writes. Order:
//   1. GCC GPU branch clocks (GCC is always clocked; GCC_GPU_CFG_AHB_CLK is kept on as a critical clock).
//   2. Only if GCC_GPU_CFG_AHB_CLK reports on (CBCR bit31 CLK_OFF == 0): the GPU_CC registers below.
// GPU_CC is reached over GPU_CFG_AHB, so step 2 cannot be attempted with that clock off.
// Before every read the step number is written to Device Parameters\EpiGpuProbeStep and flushed, so if the
// bus hangs the last step survives the forced reboot.

#include "BDD.hxx"
#include "epi_hw.hxx"
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
#define EPI_GPU_TEXT_CCH 1536

static VOID EpiGpuStep(HANDLE hParams, ULONG Step)
{
    EpiSetDword(hParams, L"EpiGpuProbeStep", Step);
    ZwFlushKey(hParams);
}

// ---- G1b: CX power-up test (EpiGpuProbe bit1) ----
// Writes GPU_CC only (no GCC, no TZ, no SMMU). The GMU and hub RCGs stay on XO (their reset source, cfg 0), so no
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

// GpuCxStatus: 0 done (GMU read), 10 CX GDSC did not power on, 11 a needed branch stayed off (GMU not read),
//              12 map failed, 13 CX GDSC did not collapse again on restore
static ULONG EpiGpuCxTest(HANDLE hParams, HANDLE hDiag, ULONG Flags, volatile ULONG* Cc, PWCHAR Text)
{
    ULONG status = 0;
    ULONG orig[RTL_NUMBER_OF(g_EpiCxBranches)] = {};
    BOOLEAN touched = FALSE;
    PVOID gmuA = NULL;
    PVOID gmuB = NULL;
    BOOLEAN allNeeded = TRUE;
    ULONG t = 0;

    volatile ULONG* gdscr = Cc + EPI_CC_CX_GDSCR / 4;
    volatile ULONG* hwctl = Cc + EPI_CC_CX_GDS_HW_CTRL / 4;
    const ULONG gdscOrig = READ_REGISTER_ULONG(gdscr);
    EpiCat(Text, L"cx_gdscr0", gdscOrig);
    EpiCat(Text, L"cx_hwctl0", READ_REGISTER_ULONG(hwctl));

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
    // 4. restore: branches in reverse order (original values; CLK_OFF is read-only), then collapse CX
    EpiGpuStep(hParams, 0x340);
    if (touched)
    {
        for (ULONG i = RTL_NUMBER_OF(g_EpiCxBranches); i-- > 0;)
        {
            WRITE_REGISTER_ULONG(Cc + g_EpiCxBranches[i].Offset / 4, orig[i]);
        }
    }
    WRITE_REGISTER_ULONG(gdscr, gdscOrig);
    t = EpiPoll(hwctl, EPI_GDSC_PWR_ON, 0, 1000);
    EpiCat(Text, L"cx_off_x10us", t);
    EpiCat(Text, L"cx_gdscr2", READ_REGISTER_ULONG(gdscr));
    EpiCat(Text, L"cx_hwctl2", READ_REGISTER_ULONG(hwctl));
    if (t > 1000 && status == 0) { status = 13; }
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
    //                 5 GCC_GPU_CFG_AHB_CLK off (GPU_CC not read)
    ULONG status = 0;
    ULONG n = 0;
    PULONG pairs = NULL;
    PWCHAR text = NULL;
    PVOID gcc = NULL;
    PVOID gpucc = NULL;
    ULONG flags = 0;
    ULONG cxStatus = 0xFFFFFFFF;
    PWCHAR cxText = NULL;

    // One shot: cleared and flushed before any hardware access, so a hang cannot repeat at the next start.
    // EpiGpuProbe bit0 = G1a read, bit1 = G1b CX power-up test (writes GPU_CC; implies the read),
    // bit2 = G1c-0: cmd-db copy + RSCC/PDC dump (runs inside the G1b CX window, so it implies bit1).
    flags = EpiQueryDword(hParams, L"EpiGpuProbe", 0);
    if (flags == 0) { status = 1; goto done; }
    EpiSetDword(hParams, L"EpiGpuProbe", 0);
    EpiGpuStep(hParams, 1);
    if (!pRes->GpuCcFound) { status = 2; goto done; }

    pairs = static_cast<PULONG>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_GPU_NREGS * 2 * sizeof(ULONG), 'gpEE'));
    text = static_cast<PWCHAR>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_GPU_TEXT_CCH * sizeof(WCHAR), 'gpEE'));
    cxText = static_cast<PWCHAR>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_GPU_TEXT_CCH * sizeof(WCHAR), 'gpEE'));
    if (pairs == NULL || text == NULL || cxText == NULL) { status = 3; goto done; }
    text[0] = L'\0';
    cxText[0] = L'\0';
    if (flags & 4) { EpiSetDword(hDiag, L"CmdDbStatus", EpiCmdDbDump(hParams, hDiag)); }

    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = EPI_GCC_BASE + EPI_GCC_MAP_OFF;
        gcc = MmMapIoSpaceEx(pa, EPI_GCC_MAP_LEN, PAGE_READONLY | PAGE_NOCACHE);
        pa.QuadPart = EPI_GPU_CC_BASE;
        gpucc = MmMapIoSpaceEx(pa, (SIZE_T)EPI_GPU_CC_LEN, ((flags & 6) ? PAGE_READWRITE : PAGE_READONLY) | PAGE_NOCACHE);
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
        if (flags & 6) { cxStatus = EpiGpuCxTest(hParams, hDiag, flags, c, cxText); }
    }

done:
    if (status != 1) { EpiGpuStep(hParams, 0xFFFF); }     // 0xFFFF = probe returned
    if (gpucc != NULL) { MmUnmapIoSpace(gpucc, (SIZE_T)EPI_GPU_CC_LEN); }
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
