// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - G1c-3: GMU CM3 boot test. New file, not derived from KMDOD.
#pragma once
// Include after BDD.hxx / epi_hw.hxx. PASSIVE_LEVEL, serialized by the caller (the EpiGpuProbe CX window).

// EpiGpuProbe bits owned by this file
#define EPI_GMU_PROBE_BOOT      0x80UL      // G1c-3: boot the CM3 behind the adreno SMMU, then put it back in reset

// Result codes (EpiDiag GpuGmuStatus)
#define EPI_GMU_OK              0           // FW_INIT_RESULT reached the expected value, CM3 back in reset, teardown ok
#define EPI_GMU_E_PRECOND       1           // CM3 not in reset on entry: nothing touched
#define EPI_GMU_E_FW            2           // firmware block list malformed or a block matches no region
#define EPI_GMU_E_NOMEM         3           // memory or register map allocation failed
#define EPI_GMU_E_SMMU_MAP      4           // EpiSmmuCreate / EpiSmmuMap failed (SMMU not enabled)
#define EPI_GMU_E_SMMU_ENABLE   5           // EpiSmmuEnable failed (CM3 never released)
#define EPI_GMU_E_TCM           6           // setup register / TCM write or read-back failed (CM3 never released)
#define EPI_GMU_E_TIMEOUT       7           // CM3 released but FW_INIT_RESULT did not reach the expected value
#define EPI_GMU_E_TEARDOWN      8           // EpiSmmuDisable failed: memory, mappings and ctx are kept (leaked)

// EpiDiag GpuGmuBadIndex (REG_DWORD): zero-based setup index on failure, 0xFF for TCM copy failure,
// 0xFFFFFFFF when neither failed. GpuGmuSetup (REG_BINARY): 18 records of five little-endian ULONGs
// {Reg (GPU-base dword offset), Original, Value, Readback, CmpMask}, 20 bytes each, captured before cleanup.
// Comparison uses (Readback & CmpMask) == (Value & CmpMask); CmpMask 0 records readback without comparing.
// Readback is 0xFFFFFFFF until read; later entries after BadIndex were not attempted. A failed breadcrumb
// can leave the failing entry unread too (see GpuGmuNtStatus). Empty binary means setup was not reached.

// Runs inside the CX window: CX GDSC and all five G1b branches on, GX off. Cc maps GPU_CC (RW).
// Fw/FwLen: the a660_gmu.bin image already read by the caller.
// *KeepCx is set TRUE only on EPI_GMU_E_TEARDOWN: the caller must then NOT collapse CX or turn the branches off
// (epi_smmu.hxx contract: never collapse CX after an unsuccessful Disable).
// Text/TextCch: diagnostic "name=value; " entries are appended.
_IRQL_requires_(PASSIVE_LEVEL)
ULONG EpiGmuBootTest(HANDLE hParams, HANDLE hDiag, volatile ULONG* Cc, const UCHAR* Fw, ULONG FwLen,
                     PWCHAR Text, SIZE_T TextCch, BOOLEAN* KeepCx);
