// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - G1c-3: GMU CM3 boot test. New file, not derived from KMDOD.
#pragma once
// Include after BDD.hxx / epi_hw.hxx. PASSIVE_LEVEL, serialized by the caller (the EpiGpuProbe CX window).

// EpiGpuProbe bits owned by this file
#define EPI_GMU_PROBE_BOOT      0x80UL      // G1c-3: boot the CM3 behind the adreno SMMU, then put it back in reset

// Result codes (EpiDiag GpuGmuStatus)
#define EPI_GMU_OK              0           // FW init + HFI PERF/BW ACKs succeeded, CM3 reset, teardown ok
#define EPI_GMU_E_PRECOND       1           // CM3 not in reset on entry: nothing touched
#define EPI_GMU_E_FW            2           // firmware block list malformed or a block matches no region
#define EPI_GMU_E_NOMEM         3           // memory or register map allocation failed
#define EPI_GMU_E_SMMU_MAP      4           // EpiSmmuCreate / EpiSmmuMap failed (SMMU not enabled)
#define EPI_GMU_E_SMMU_ENABLE   5           // EpiSmmuEnable failed (CM3 never released)
#define EPI_GMU_E_TCM           6           // setup register / TCM write or read-back failed (CM3 never released)
#define EPI_GMU_E_TIMEOUT       7           // CM3 released but FW_INIT_RESULT did not reach the expected value
#define EPI_GMU_E_TEARDOWN      8           // reset/disable/restore/teardown failed: memory, mappings and ctx are kept
#define EPI_GMU_E_RSCC          9           // RSCC initialization write/readback or breadcrumb failed (CM3 never released)
#define EPI_GMU_E_HFI           10          // HFI start / PERF_TABLE / BW_TABLE failed after successful FW init

// EpiDiag GpuGmuBadIndex (REG_DWORD): zero-based setup index on failure, 0xFF for TCM copy failure,
// 0xFFFFFFFF when neither failed. GpuGmuSetup (REG_BINARY): 19 records of five little-endian ULONGs
// {Reg (GPU-base dword offset), Original, Value, Readback, CmpMask}, 20 bytes each, captured before cleanup.
// Comparison uses (Readback & CmpMask) == (Value & CmpMask); CmpMask 0 records readback without comparing.
// Readback is 0xFFFFFFFF until read; later entries after BadIndex were not attempted. A failed breadcrumb
// can leave the failing entry unread too (see GpuGmuNtStatus). Empty binary means setup was not reached.
// Setup index 18 is GMU_RPMH_CTRL (0x1F8E8), value 0xF11, full comparison mask.
// GpuGmuRsccSetup (REG_BINARY): ten 20-byte records {Dword, Original, Value, Readback, Restored}.
// Dword is relative to RSCC 0x03DE0000; unread Readback/Restored fields are 0xFFFFFFFF.
// GpuGmuRsccBadIndex / GpuGmuRsccRestoreBadIndex (REG_DWORD): first failing index, or 0xFFFFFFFF.
// GpuGmuRsccRestoreStatus (REG_DWORD): NTSTATUS of restoration, STATUS_NOT_FOUND if not attempted.
// HFI: GpuGmuHfiStatus / GpuGmuHfiIdleStatus (REG_DWORD NTSTATUS), STATUS_NOT_FOUND if not attempted.
// GpuGmuHfiCtrlStatus: startup HFI_CTRL_STATUS; GpuGmuHfiBwPublished: BW command was exposed to GMU.
// GpuGmuHfiMessages (REG_BINARY): two 148-byte records (PERF, BW), each 37 DWORDs:
// {SentHeader, NtStatus, AckDwords, Ack[19], Skipped, FwErrors, LastFwError[4], LastSkippedDwords, LastSkipped[8]}.
// Skipped includes FW errors; LastSkipped holds up to eight leading DWORDs, with zero fill for shorter packets.
// A response is any non-id-100 packet with >=3 DWORDs and matching ret_header seq; id 126 is not required.
// GpuGmuHfiQueues: five 28-byte records {Stage, CmdRead, CmdWrite, RspRead, RspWrite, DebugRead, DebugWrite}.
// Stages: 0 before HFI init, 1 ready, 2 after PERF, 3 after BW, 4 after CM3 reset.
// GpuGmuHfiPower: four 24-byte records {Stage, RpmhPower, GxGdscr, GxHm, HfiCtrlStatus, IntrInfo}.
// Stages: 0 before HFI init, 1 after HFI trial, 2 after reset/idle wait, 3 after config restoration.
// GpuGmuHfiRscc: four 20-byte records {Stage, Status0, SeqBusy, ControlReq, ControlAck}; same stages as Power.
// Uncaptured queue/power fields are 0xFFFFFFFF. HFI binaries are empty if FW init did not succeed.
// After BW publication, require CM3 reset, RSCC SeqBusy/ControlReq/ControlAck=0, and GX off before teardown.
// RPMh power state is recorded without requiring 0xF; RSCC quiescence does not prove CNOC vote withdrawal.

// Runs inside the CX window: CX GDSC and all five G1b branches on, GX off. Cc maps GPU_CC (RW).
// Fw/FwLen: the a660_gmu.bin image already read by the caller.
// *KeepCx is set TRUE only on EPI_GMU_E_TEARDOWN: the caller must then NOT collapse CX or turn the branches off
// (epi_smmu.hxx contract: never collapse CX after an unsuccessful Disable).
// Text/TextCch: diagnostic "name=value; " entries are appended.
_IRQL_requires_(PASSIVE_LEVEL)
ULONG EpiGmuBootTest(HANDLE hParams, HANDLE hDiag, volatile ULONG* Cc, const UCHAR* Fw, ULONG FwLen,
                     PWCHAR Text, SIZE_T TextCch, BOOLEAN* KeepCx);
