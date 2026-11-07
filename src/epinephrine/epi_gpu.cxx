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

    // One shot: cleared and flushed before any hardware access, so a hang cannot repeat at the next start.
    if (EpiQueryDword(hParams, L"EpiGpuProbe", 0) == 0) { status = 1; goto done; }
    EpiSetDword(hParams, L"EpiGpuProbe", 0);
    EpiGpuStep(hParams, 1);
    if (!pRes->GpuCcFound) { status = 2; goto done; }

    pairs = static_cast<PULONG>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_GPU_NREGS * 2 * sizeof(ULONG), 'gpEE'));
    text = static_cast<PWCHAR>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_GPU_TEXT_CCH * sizeof(WCHAR), 'gpEE'));
    if (pairs == NULL || text == NULL) { status = 3; goto done; }
    text[0] = L'\0';

    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = EPI_GCC_BASE + EPI_GCC_MAP_OFF;
        gcc = MmMapIoSpaceEx(pa, EPI_GCC_MAP_LEN, PAGE_READONLY | PAGE_NOCACHE);
        pa.QuadPart = EPI_GPU_CC_BASE;
        gpucc = MmMapIoSpaceEx(pa, (SIZE_T)EPI_GPU_CC_LEN, PAGE_READONLY | PAGE_NOCACHE);
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
    if (text != NULL) { ExFreePoolWithTag(text, 'gpEE'); }
    if (pairs != NULL) { ExFreePoolWithTag(pairs, 'gpEE'); }
    ZwClose(hDiag);
    ZwClose(hParams);
}
