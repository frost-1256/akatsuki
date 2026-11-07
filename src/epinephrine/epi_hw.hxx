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
