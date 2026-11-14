// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - spacewar display block helpers. New file, not derived from KMDOD.
#pragma once
// Include after BDD.hxx (which brings ntddk.h and dispmprt.h).

// GPU0 _CRS entries Epinephrine cares about (spacewar DSDT v32/v33-akat).
// Identified by type + raw address + length, never by position in the list.
#define EPI_MDP_REGS_BASE    0x0AE00000ULL   // RESI "MDP_REGS"    (DISPLAY)
#define EPI_MDP_REGS_LEN     0x00200000ULL
#define EPI_DP_PHY_REGS_BASE 0x088E0000ULL   // RESI "DP_PHY_REGS" (DISPLAY)
#define EPI_DP_PHY_REGS_LEN  0x00010000ULL
#define EPI_GPU_CC_BASE      0x03D90000ULL   // RESI "GPU_CC"      (GPU G1a)
#define EPI_GPU_CC_LEN       0x00009000ULL

typedef struct _EPI_HW_RESOURCES
{
    BOOLEAN          MdpFound;
    PHYSICAL_ADDRESS MdpBase;
    ULONG            MdpLength;
    BOOLEAN          DpPhyFound;
    BOOLEAN          GpuCcFound;
    ULONG            MemoryCount;
    ULONG            InterruptCount;
    ULONG            ConnectionCount;
} EPI_HW_RESOURCES;

// Read-only diagnostic snapshot taken once in StartDevice (WP2b-1).
// Walks the translated resource list, maps only MDP_REGS sub-blocks that MdssSnap
// already read safely in the bootloader state, and writes the result under
//   <device key>\Device Parameters\EpiDiag
// Never writes hardware. Disabled when Device Parameters\EpiDisableHwSnap (DWORD) != 0.
// Failures are recorded and ignored; the caller's start path must not depend on it.
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiHwSnapshot(_In_ const DXGK_DEVICE_INFO* pDeviceInfo, _Out_ EPI_HW_RESOURCES* pRes);

// Read-only timing probe (WP2b-3a), StartDevice after EpiHwSnapshot. Samples INTF1 frame/line/tear counters and
// MDSS HW_INTR_STATUS every 250 us for ~100 ms (stalls the start path that long) and diffs INTF1 before/after.
// EpiDiag: ProbeStatus, ProbeSamples (REG_BINARY, 400 x {t_us, +0xAC, +0xB0, +0x298, +0x2B0, mdss+0x10, +0x1C4, tear+0x4}),
// ProbeIntfDiff ("off:before>after;"). Off by default; runs only when Device Parameters\EpiVsyncProbe != 0.
// EpiVsyncProbe bit1 (one shot) also reads the INTF TE interrupt block (MDP + 0x36800): ProbeTearBefore/After.
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiVsyncProbe(_In_ const EPI_HW_RESOURCES* pRes, _In_ PDEVICE_OBJECT Pdo);

// GPU G1a (epi_gpu.cxx): one-shot read-only GPU_CC state probe, StartDevice after EpiVsyncProbe.
// Runs only when Device Parameters\EpiGpuProbe != 0; that value is cleared and flushed before any access.
// Reads GCC GPU branch clocks first and GPU_CC only if GCC_GPU_CFG_AHB_CLK is on. Non-volatile breadcrumb
// Device Parameters\EpiGpuProbeStep (0x1xx GCC, 0x2xx GPU_CC index, 0xFFFF returned).
// EpiDiag: GpuProbeStatus, GpuProbe (REG_BINARY {phys, value} pairs), GpuProbeText.
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiGpuProbe(_In_ const EPI_HW_RESOURCES* pRes, _In_ PDEVICE_OBJECT Pdo);

// GPU P-1 (epi_gpu.cxx): stop the worker session that keeps the GPU running after StartDevice and wait up to 5 s
// for it to tear the GPU down. Cause: 1 StopDevice/unload, 2 non-D0 power state. No-op without a session.
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiGpuSessionStop(ULONG Cause);
// P-2: called by a present thread after a failed GPU present; asks the session worker to stop (cause 4), never waits.
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiGpuSessionRequestStop(ULONG Cause);

// EpiDiag registry helpers (shared by the snapshot and the panel loader).
// EpiOpenDiag opens <device key>\Device Parameters and its volatile EpiDiag subkey; close both handles with ZwClose.
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS EpiOpenDiag(PDEVICE_OBJECT Pdo, HANDLE* phParams, HANDLE* phDiag);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS EpiSetValue(HANDLE hKey, PCWSTR Name, ULONG Type, const VOID* Data, ULONG Size);
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS EpiSetDword(HANDLE hKey, PCWSTR Name, ULONG Value);
_IRQL_requires_(PASSIVE_LEVEL) ULONG EpiQueryDword(HANDLE hKey, PCWSTR Name, ULONG Default);

// ---- Panel (WP2b-2) ----
#include "epi_edid.hxx"

typedef struct _EPI_PANEL
{
    BOOLEAN          Valid;                 // EDID synthesized and timing usable
    BOOLEAN          UseEdid;               // EpiPanelFeatures bit0: report the EDID to the OS
    BOOLEAN          UseSignal;             // EpiPanelFeatures bit1: report the real TotalSize in target/monitor modes
    EPI_PANEL_TIMING Timing;
    UCHAR            Edid[EPI_EDID_SIZE];
} EPI_PANEL;

// Reads the panel XML from GPU0 _ROM (PCFG) with DxgkCbEvalAcpiMethod, synthesizes the base EDID block
// and records PanelStatus / PanelXmlLen / Edid / PanelTiming under EpiDiag.
// PanelStatus: 0 ok, 1 disabled (Device Parameters\EpiDisablePanel != 0), 2 no memory, 3 _ROM eval failed
// (PanelAcpiStatus), 4 unexpected _ROM output, 10 + EPI_EDID_* synthesis error (PanelMissingTag).
// Device Parameters\EpiPanelFeatures (DWORD, default 1 = EDID only) selects what is reported when Valid.
// bit1 reports the real TotalSize (with blanking) only. VSync/HSync/PixelRate stay NOTSPECIFIED: a display-only
// driver without VSync control (DxgkDdiControlInterrupt/GetScanLine) must not specify them, and pfnAddMode rejects
// such target modes with STATUS_GRAPHICS_INVALID_FREQUENCY (black screen, bisected 2026-11-07).
// On any failure Valid stays FALSE and the driver keeps the KMDOD behaviour (no EDID, POST mode only).
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiPanelLoad(_In_ const DXGKRNL_INTERFACE* pDxgk, _In_ PDEVICE_OBJECT Pdo, _Out_ EPI_PANEL* pPanel);
