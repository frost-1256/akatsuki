// SPDX-License-Identifier: MIT
#pragma once
// Include after BDD.hxx / ntddk.h. All calls: PASSIVE_LEVEL, serialized by caller.
// Exclusive adreno SMMU ownership (DSDT v34+, qcsmmu must not own MMU1).
struct EPI_SMMU;

#define EPI_SMMU_READ       0x01UL
#define EPI_SMMU_WRITE      0x02UL
#define EPI_SMMU_PRIVILEGED 0x04UL
// READ required; omit WRITE for RO, add PRIVILEGED for privileged-only.
// Leaf memory: Normal non-cacheable, outer-shareable, executable (GMU code).

// Software only; tables belong to ctx, mapped data belongs to caller.
// Params is an optional borrowed Device Parameters key for flushed breadcrumbs.
NTSTATUS EpiSmmuCreate(HANDLE Params, EPI_SMMU** Context);
NTSTATUS EpiSmmuDestroy(EPI_SMMU* Context); // refuses while hardware may reference tables

// Disabled contexts only. Iova/Phys/Size must be 4KB aligned, Size nonzero.
// Round firmware lengths UP before calling; caller owns the full padded allocation.
// Phys is physically contiguous, nonpaged, Normal non-cacheable RAM below 2^36.
// IOVA range [0,2^32). Overlap/hole errors change no leaf mappings.
NTSTATUS EpiSmmuMap(EPI_SMMU* Context, ULONGLONG Iova, PHYSICAL_ADDRESS Phys, SIZE_T Size, ULONG Flags);
NTSTATUS EpiSmmuUnmap(EPI_SMMU* Context, ULONGLONG Iova, SIZE_T Size);

// CX and ALL five G1b branches must be on. Cc maps GPU_CC; GmuA maps
// physical 0x03D7E000 (CM3_SYSRESET at offset 0). Both are borrowed and must
// remain mapped/powered until Disable SUCCEEDS. CM3 must be reset, GX off.
// Enable snapshots registers, installs stream 5 / mask 0x400 -> CB0 and syncs TLB.
// Disable requires CM3 reset again; restores saved registers and syncs TLB.
// On failed Enable, call Disable too: failed rollback retains ownership/tables.
// On failed Disable: retain ctx, data, mappings and CX power; retry or recover
// by device reset. Never free memory/collapse CX on an unsuccessful teardown.
NTSTATUS EpiSmmuEnable(EPI_SMMU* Context, volatile ULONG* Cc, volatile ULONG* GmuA);
NTSTATUS EpiSmmuDisable(EPI_SMMU* Context);

// Read-only, valid while enabled, including while CM3 runs. Does not clear faults.
// Caller keeps CX/branches on and serializes against Disable/CX teardown.
NTSTATUS EpiSmmuQueryFault(EPI_SMMU* Context, ULONG* Fsr, ULONGLONG* Far, ULONG* Fsynr0);
