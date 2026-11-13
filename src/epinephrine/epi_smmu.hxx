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
// Software-only opt-in before Enable. SMR1 ID0/mask0x400 and SMR2 ID1/mask0x400 share CB0.
// SMR0 remains GMU ID5/mask0x400. Enable/Disable snapshot and restore both GPU SMR/S2CR pairs.
NTSTATUS EpiSmmuConfigureGpuStreams(EPI_SMMU* Context);
struct EPI_SMMU_GPU_STREAMS_TRACE {
    ULONG Configured, SmrOriginal[2], S2crOriginal[2], SmrReadback[2], S2crReadback[2];
    ULONG SmrDisabled[2], SmrRestored[2], S2crRestored[2];
    NTSTATUS RestoreStatus;
};
static_assert(sizeof(EPI_SMMU_GPU_STREAMS_TRACE)==64,"SMMU GPU streams diagnostic ABI");
// Array order is SMR/S2CR index 1, then 2. Configured=1 means the two-stream opt-in is enabled.
// Software-only copy, retained across Disable. Unread values=0xFFFFFFFF; unattempted restore=STATUS_NOT_FOUND.
NTSTATUS EpiSmmuQueryGpuStreams(EPI_SMMU* Context, EPI_SMMU_GPU_STREAMS_TRACE* Trace);

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
// ConfigureGpuStreams optionally adds GPU streams 0 and 1 / mask 0x400 -> CB0 using SMR1/2 and S2CR1/2.
// Disable requires CM3 reset again; restores saved registers and syncs TLB.
// After verified stream/CB disable and TLB sync, attempt all ConfigOffsets restores on readback mismatches.
// Any failure retains ownership/tables; final SCTLR/SMR restores run only if every config restore succeeded.
// On failed Enable, call Disable too: failed rollback retains ownership/tables.
// On failed Disable: retain ctx, data, mappings and CX power; retry or recover
// by device reset. Never free memory/collapse CX on an unsuccessful teardown.
NTSTATUS EpiSmmuEnable(EPI_SMMU* Context, volatile ULONG* Cc, volatile ULONG* GmuA);
NTSTATUS EpiSmmuDisable(EPI_SMMU* Context);

// Software-only query of the FIRST Write readback mismatch during the context's lifetime.
// Survives Disable/rollback; no hardware access and does not clear the record.
// Offset is in bytes from the SMMU register base (0x03DA0000).
// All outputs required. STATUS_NOT_FOUND if no mismatch; outputs unchanged on failure.
NTSTATUS EpiSmmuQueryLastError(EPI_SMMU* Context, ULONG* Step, ULONG* Offset, ULONG* Value, ULONG* Readback);

// Read CBAR0 only; requires the live register mapping and powered CX/branches. Does not write or clear state.
NTSTATUS EpiSmmuQueryCbar(EPI_SMMU* Context, ULONG* Value);

#define EPI_SMMU_DISABLE_WRITE_MAX 11
struct EPI_SMMU_DISABLE_WRITE {
    ULONG Step, Offset, Value, Readback, CmpMask;
    NTSTATUS Status;
    ULONG Written;
};
static_assert(sizeof(EPI_SMMU_DISABLE_WRITE)==28,"SMMU disable diagnostic ABI");
// Software-only copy of the last ConfigOffsets restoration sweep, including successes and mismatches.
// CBAR0 restoration uses CmpMask=0 when its saved TYPE[17:16]=0 on this S1-only device; raw readback is retained.
// Other comparisons and pre-write persistence failures are unchanged.
// Count receives the available record count; capacity must cover it. STATUS_NOT_FOUND if no sweep reached.
NTSTATUS EpiSmmuQueryDisableWrites(EPI_SMMU* Context, EPI_SMMU_DISABLE_WRITE* Writes, ULONG Capacity, ULONG* Count);

// Read-only, valid while enabled, including while CM3 runs. Does not clear faults.
// Caller keeps CX/branches on and serializes against Disable/CX teardown.
NTSTATUS EpiSmmuQueryFault(EPI_SMMU* Context, ULONG* Fsr, ULONGLONG* Far, ULONG* Fsynr0);
// Read-only GR0 snapshot: sGFSR, sGFSYNR0, sGFSYNR1, sGFSYNR2 (byte offsets 0x48/0x50/0x54/0x58).
// No acknowledgement/clear writes. Values must have room for four ULONGs; unchanged on failure.
NTSTATUS EpiSmmuQueryGlobalFault(EPI_SMMU* Context, ULONG* Values);
