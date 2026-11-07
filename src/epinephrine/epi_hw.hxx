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

typedef struct _EPI_HW_RESOURCES
{
    BOOLEAN          MdpFound;
    PHYSICAL_ADDRESS MdpBase;
    ULONG            MdpLength;
    BOOLEAN          DpPhyFound;
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
    BOOLEAN          UseSignal;             // EpiPanelFeatures bit1: report real signal timing in target/monitor modes
    EPI_PANEL_TIMING Timing;
    UCHAR            Edid[EPI_EDID_SIZE];
} EPI_PANEL;

// Reads the panel XML from GPU0 _ROM (PCFG) with DxgkCbEvalAcpiMethod, synthesizes the base EDID block
// and records PanelStatus / PanelXmlLen / Edid / PanelTiming under EpiDiag.
// PanelStatus: 0 ok, 1 disabled (Device Parameters\EpiDisablePanel != 0), 2 no memory, 3 _ROM eval failed
// (PanelAcpiStatus), 4 unexpected _ROM output, 10 + EPI_EDID_* synthesis error (PanelMissingTag).
// Device Parameters\EpiPanelFeatures (DWORD, default 1 = EDID only) selects what is reported when Valid.
// bit1 (real signal timing in target/monitor modes) gives a black screen on spacewar (2026-11-07, open issue).
// On any failure Valid stays FALSE and the driver keeps the KMDOD behaviour (no EDID, POST mode only).
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiPanelLoad(_In_ const DXGKRNL_INTERFACE* pDxgk, _In_ PDEVICE_OBJECT Pdo, _Out_ EPI_PANEL* pPanel);
