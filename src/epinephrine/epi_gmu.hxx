// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - G1c-3: GMU CM3 boot test. New file, not derived from KMDOD.
#pragma once
// Include after BDD.hxx / epi_hw.hxx. PASSIVE_LEVEL, serialized by the caller (the EpiGpuProbe CX window).

// EpiGpuProbe bits owned by this file
#define EPI_GMU_PROBE_BOOT      0x80UL      // G1c-3: boot the CM3 behind the adreno SMMU, then put it back in reset
#define EPI_GMU_PROBE_HFI_START 0x200UL     // modifier: with 0x80 (0x280), add CORE_FW_START / START after PERF/BW
#define EPI_GMU_PROBE_GX_VOTE   0x400UL     // modifier: requires 0x280 (0x680 total), add GBIF QSB / GX_BW_PERF_VOTE
#define EPI_GMU_PROBE_CLOCK    0x800UL     // modifier: requires 0x680 (0xE80 total), GMU 200 MHz / hub 150 MHz
#define EPI_GMU_PROBE_GX_ON    0x1000UL    // modifier: requires 0xE80 (0x1E80 total), OOB GPU_SET + slumber / rpmh_stop
#define EPI_GMU_PROBE_GX_REG   0x2000UL    // modifier: requires 0x1E80 (0x3E80 total), read six GX registers under OOB
#define EPI_GMU_PROBE_GX_HALT  0x4000UL    // modifier: requires 0x3E80 (0x7E80 total), clear CX/RBBM GBIF halt under OOB
#define EPI_GMU_PROBE_SECVID   0x8000UL    // modifier: requires 0x7E80 (0xFE80 total), SECVID stage A only
#define EPI_GMU_PROBE_ADDR_MODE 0x10000UL // modifier: requires 0xFE80 (0x1FE80 total), ADDR_MODE stage B
#define EPI_GMU_PROBE_HWCG      0x20000UL // modifier: requires 0x1FE80 (0x3FE80 total), hwcg stage C
#define EPI_GMU_PROBE_GX_CFG    0x40000UL // modifier: requires 0x3FE80 (0x7FE80 total), stage D without CP_AHB_CNTL
#define EPI_GMU_PROBE_STAGE_E   0x80000UL // modifier: requires 0x7FE80 (0xFFE80 total), UBWC and CP protection

// Result codes (EpiDiag GpuGmuStatus)
#define EPI_GMU_OK              0           // FW init + all requested HFI ACKs succeeded, CM3 reset, teardown ok
#define EPI_GMU_E_PRECOND       1           // CM3 not in reset on entry: nothing touched
#define EPI_GMU_E_FW            2           // firmware block list malformed or a block matches no region
#define EPI_GMU_E_NOMEM         3           // memory or register map allocation failed
#define EPI_GMU_E_SMMU_MAP      4           // EpiSmmuCreate / EpiSmmuMap failed (SMMU not enabled)
#define EPI_GMU_E_SMMU_ENABLE   5           // EpiSmmuEnable failed (CM3 never released)
#define EPI_GMU_E_TCM           6           // setup register / TCM write or read-back failed (CM3 never released)
#define EPI_GMU_E_TIMEOUT       7           // CM3 released but FW_INIT_RESULT did not reach the expected value
#define EPI_GMU_E_TEARDOWN      8           // reset/disable/restore/teardown failed: memory, mappings and ctx are kept
#define EPI_GMU_E_RSCC          9           // RSCC initialization write/readback or breadcrumb failed (CM3 never released)
#define EPI_GMU_E_HFI           10          // HFI init or a requested message failed after successful FW init
#define EPI_GMU_E_GBIF          11          // GBIF precondition/read/write/breadcrumb failed (CM3 never released)
#define EPI_GMU_E_CLOCK         12          // clock setup failed (CM3 never released); failed restoration returns 8
#define EPI_GMU_E_GX            13          // OOB/slumber/optional GX initialization failed, but safe teardown succeeded

// EpiDiag GpuGmuBadIndex (REG_DWORD): zero-based setup index on failure, 0xFF for TCM copy failure,
// 0xFFFFFFFF when neither failed. GpuGmuSetup (REG_BINARY): 19 records of five little-endian ULONGs
// {Reg (GPU-base dword offset), Original, Value, Readback, CmpMask}, 20 bytes each, captured before cleanup.
// Comparison uses (Readback & CmpMask) == (Value & CmpMask); CmpMask 0 records readback without comparing.
// Readback is 0xFFFFFFFF until read; later entries after BadIndex were not attempted. A failed breadcrumb
// can leave the failing entry unread too (see GpuGmuNtStatus). Empty binary means setup was not reached.
// Setup index 18 is GMU_RPMH_CTRL (0x1F8E8), value 0xF11, full comparison mask.
// GpuGmuRsccSetup (REG_BINARY): ten 20-byte records {Dword, Original, Value, Readback, Restored}.
// Dword is relative to RSCC 0x03DE0000; unread Readback/Restored fields are 0xFFFFFFFF.
// Restoration compares only bit24 (mask 0x01000000) for STATUS0 dword 0x004; other targets compare all bits.
// Restored always records the raw, unmasked value; the diagnostic record layout is unchanged.
// GpuGmuRsccBadIndex / GpuGmuRsccRestoreBadIndex (REG_DWORD): first failing index, or 0xFFFFFFFF.
// GpuGmuRsccRestoreStatus (REG_DWORD): NTSTATUS of restoration, STATUS_NOT_FOUND if not attempted.
// GpuGmuRsccEntry: 16 bytes {Status0, SeqBusy, ControlReq, ControlAck} before RSCC writes.
// No rpmh_start: require initially collapsed CX and reject residual RSCC sleep/busy/requests at entry.
// HFI: GpuGmuHfiStatus / GpuGmuHfiIdleStatus (REG_DWORD NTSTATUS), STATUS_NOT_FOUND if not attempted.
// GpuGmuHfiCtrlStatus: startup HFI_CTRL_STATUS; GpuGmuHfiBwPublished: BW command was exposed to GMU.
// GpuGmuHfiMessages (REG_BINARY): six 148-byte records (PERF, BW, CORE_FW_START, START, GX vote, SLUMBER), 888 bytes total:
// {SentHeader, NtStatus, AckDwords, Ack[19], Skipped, FwErrors, LastFwError[4], LastSkippedDwords, LastSkipped[8]}.
// Skipped includes FW errors; LastSkipped holds up to eight leading DWORDs, with zero fill for shorter packets.
// A response is any non-id-100 packet with >=3 DWORDs and matching ret_header seq; id 126 is not required.
// GpuGmuHfiQueues: nine 28-byte records (252 bytes) {Stage, CmdRead, CmdWrite, RspRead, RspWrite, DebugRead, DebugWrite}.
// Stages: 0 before HFI init, 1 ready, 2 after PERF, 3 after BW, 4 after CORE_FW_START, 5 after START, 6 after VOTE,
// 7 after slumber/busy/queue checks, 8 after reset.
// GpuGmuHfiPower: four 24-byte records {Stage, RpmhPower, GxGdscr, GxHm, HfiCtrlStatus, IntrInfo}.
// Stages: 0 before HFI init, 1 after HFI trial, 2 after reset/idle wait, 3 after config restoration.
// GpuGmuHfiRscc: four 20-byte records {Stage, Status0, SeqBusy, ControlReq, ControlAck}; same stages as Power.
// Uncaptured queue/power fields are 0xFFFFFFFF. HFI binaries are empty if FW init did not succeed.
// After BW publication, require CM3 reset, RSCC SeqBusy/ControlReq/ControlAck=0, and GX off before teardown.
// If this run issued rpmh_stop, SeqBusy=1 is also accepted with Status0 bit16=1, ControlReq=0,
// GX off and CM3 reset; ControlAck=0 is required only for the ordinary SeqBusy=0 path.
// RPMh power state is recorded without requiring 0xF; RSCC quiescence does not prove CNOC vote withdrawal.
// In 0x680 only, GpuGmuHfiExtra: five 48-byte records (240 bytes), each:
// {Stage, GccVote, TcsStatus[4], CxBusy, CxBusy2, GbifQsb[4]}.
// Stages 0..3 match Power; stage 4 is immediately before GX vote. Unread fields are 0xFFFFFFFF.
// GpuGmuHfiExtraStatus: first read-breadcrumb failure, 0 on success, STATUS_NOT_FOUND if not attempted.
// GpuGmuGbifSetup: four 20-byte records {GPU dword Reg, Original, Value, Readback, Restored}, 80 bytes total.
// GpuGmuGbifStatus / GpuGmuGbifRestoreStatus: NTSTATUS; STATUS_NOT_FOUND when not attempted.
// GpuGmuGbifBadIndex / GpuGmuGbifRestoreBadIndex: first failing index, or 0xFFFFFFFF.
// GpuGmuClockStatus / GpuGmuClockRestoreStatus: NTSTATUS; STATUS_NOT_FOUND if not attempted.
// GpuGmuClockGcc (36 bytes): {Before, AfterEnable, BeforeDisable, AfterDisable,
// EnableOtherXor, DisableOtherXor, AddedByUs, EnableWritten, DisableWritten}; only bit16 is modified.
// GpuGmuClockRcgs: two 48-byte records (GMU/hub), each {CmdOffset, CfgOffset, OriginalCmd, OriginalCfg,
// TargetCfg, SetCmd, SetCfg, SetPolls, RestoreCmd, RestoreCfg, RestorePolls, PendingPolls}.
// Polls count 10-us waits (0..50); PendingPolls is the pre-restore UPDATE wait. Unread fields=0xFFFFFFFF.
// GpuGmuClockBranches: three rows {CxGmu, HubCxInt, HubAon}, before setup / after setup / after restore (36 bytes).
// Clock test requires original CFGs=0 (XO); an added GCC vote is retained if restoration fails.
// GpuGmuGx (340 bytes): 25 DWORDs followed by Snap[5][12]:
// {Attempted, OobStatus, OobPolls, OobInfo, HaltStatus, HaltPolls[2], HaltAck[2], SlumberStatus,
// BusyStatus, BusyPolls, PowerCounterOriginal, QueueEmpty[2], RpmhStopStatus, RpmhStopPolls,
// IdleStatus, IdlePolls, IdleLast, BusyLast, RpmhStopLast, BreadcrumbStatus, PowerCounterSaved, PowerCounterRestoreStatus}.
// Snap rows: {Stage, GxGdscr, GxHm, RpmhPower, Tcs0, Tcs1, Tcs2, Tcs3, SeqBusy, Status0, CxBusy, Pll0Mode}.
// Stages: 0 before OOB, 1 after OOB wait, 2 after release/slumber, 3 after reset/rpmh_stop, 4 after idle check.
// Status fields are NTSTATUS; not-attempted statuses=STATUS_NOT_FOUND, unread values=0xFFFFFFFF.
// BreadcrumbStatus keeps the first persistence failure. Attempted/PowerCounterSaved and QueueEmpty use 0/1.
// GpuSmmuCbar: five 12-byte records {Stage, QueryStatus, Cbar0}, using the same GX Snap stages.
// GpuSmmuDisableWrites: up to eleven 28-byte records {Step, Offset, Value, Readback, CmpMask, Status, Written}.
// All ConfigOffsets restore attempts are recorded. No-write records have Written=0 and Readback=0xFFFFFFFF.
// Mismatches after stream/CB disable and TLB sync do not stop this sweep; any failure still retains CX/ctx.
// GpuGmuGxReg: 44 bytes {Attempted, Reason, Gdscr, GxHm, Count, Value[6]}; unread values=0xFFFFFFFF.
// Reason: 0 completed, 1 not reached, 2 OOB ACK failed, 3 GDSCR off, 4 GX HM off, 5 map failed, 6 breadcrumb failed.
// Value order: STATUS (0x210), STATUS3 (0x213), INT_0_STATUS (0x201), CLOCK_CNTL (0xAE), GBIF_HALT/ACK (0x16/17).
// Count is completed reads (0..6); Attempted becomes 1 before the first read. Stop on a failed guard/flush.
// Six reads precede optional halt clears. The page is RW only when ClearHalt is requested; breadcrumbs 0x7C8..0x7CD.
// GpuGmuGxHalt: 72 bytes {Attempted, Reason, Gdscr, GxHm, Count, Status, CxIdleStatus,
// Before[4], After[4], FinalCx[2], CxPolls}; Before/After order: CX HALT/ACK, RBBM HALT/ACK.
// Reason 0 success; 1..6 match GxReg; 7 nonzero HALT readback; 8 incomplete prerequisite reads.
// Count is completed write/readback pairs (0..2). Status/CxIdleStatus are NTSTATUS; unattempted=STATUS_NOT_FOUND.
// FinalCx is the CX HALT/ACK after slumber/reset/idle; require HALT=0, otherwise KeepCx. ACK is diagnostic only.
// GpuGmuGxInit: 152 bytes {Attempted, Reason, Gdscr, GxHm, Count, Status, Before[16], After[16]}.
// Stage A slots 0..3: SECVID TSB_CNTL, TRUSTED_BASE low, high, TRUSTED_SIZE (all target 0).
// Stage B slots 4..15: CP, VSC, GRAS, RB, PC, HLSQ, VFD, VPC, UCHE, SP, TPL1, SECVID TSB (target 1).
// Reason 0 success; 1..6 match GxReg; 7 readback mismatch; 8 prerequisite stage incomplete.
// Count includes a mismatched write/readback (0..16); Attempted is set before the first write.
// Status is NTSTATUS (unattempted=STATUS_NOT_FOUND). Unread values=0xFFFFFFFF.
// Breadcrumbs 0x800..0x803 precede original reads, 0x810..0x813 precede writes; each flush is followed by a GX guard.
// Stage B uses 0x820..0x82B for original reads and 0x830..0x83B for writes; mismatch stops further writes.
// GpuGmuGxHwcg: 480 bytes {Attempted, Reason, Gdscr, GxHm, Count, Status, Skipped,
// CgcBefore[3], CgcAfter[3], ClockCntlBefore, ClockCntlAfter, SptpracBefore, SptpracAfterClear,
// SptpracAfterSet, Before[49], After[49], CgcRestored[3], CgcRestoreStatus}.
// CGC order: MODE, DELAY, HYST. Table order: a660_hwcg as listed in survey section 9.17.
// Count counts CGC/table/CLOCK_CNTL writes with readback (0..53); SPTPRAC is separate.
// Reason 0 success; 1..8 match GxInit; 9 reserved (formerly SP readback mismatch). First failure is retained.
// SPTPRAC/table readbacks are diagnostic only. CGC/CLOCK_CNTL and CGC restoration still compare strictly.
// Unread=0xFFFFFFFF; mismatched table/SPTPRAC values alone do not fail or stop the sequence.
// Status/RestoreStatus are NTSTATUS, unattempted=STATUS_NOT_FOUND. Skipped=1 if CLOCK_CNTL was already enabled.
// Breadcrumbs: originals 0x840..0x875, CGC 0x880..0x882, SP clear 0x883, table 0x890..0x8C0,
// mandatory SP set 0x8C1, CLOCK_CNTL 0x8C2, CGC restoration 0x8D0..0x8D2.
// GpuGmuGxCfg: 536 bytes {Attempted, Reason, Status, Count, Gdscr, GxHm, Writes[30],
// CxBefore[3], CxRestored[3], CxRestoreStatus, CxWritten}; Writes entries are {Reg, Value, Before, After}.
// Table order follows survey 9.18, splitting lo/high and omitting CP_AHB_CNTL entirely.
// QOS occurs twice; its two Before values are the same pre-stage original, with distinct After records.
// Cx order/mask bits: CX_BUSY_MASK, POWER_COUNTER_SELECT_0, POWER_COUNTER_ENABLE.
// SELECT uses (original & ~0xFF) | 0x20. All stage D readbacks are diagnostic only.
// Reason 0..6 match GxReg, 8=stage C incomplete. Unread=0xFFFFFFFF, unattempted status=STATUS_NOT_FOUND.
// Breadcrumbs: originals 0x900..0x91D, writes 0x940..0x95D, CX restoration 0x9A0..0x9A2.
// CX restoration compares strictly, attempts all written targets, and failure requires KeepCx.
// GpuGmuGxStageE: 776 bytes {Attempted, Reason, Status, Count, Gdscr, GxHm, Writes[47]}.
// Each entry is {Reg, Value, Before, After}, in survey 9.19 order: UBWC[5], CP_PROTECT_CNTL, protect[41].
// Protect entries 0..39 use 0x850..0x877; entry 40 uses 0x87F. 0x878..0x87E are never accessed.
// Readbacks are diagnostic only; no restoration. Count is completed write/readback pairs (0..47).
// Reason 0..6 match GxReg; 8=stage D incomplete. Unread=0xFFFFFFFF; unattempted Status=STATUS_NOT_FOUND.
// Breadcrumbs: original reads 0xA00..0xA2E, writes 0xA40..0xA6E; each flush precedes a GX guard.
// Stage F (bit20) remains unimplemented and is rejected before hardware access.

// Runs inside the CX window: CX GDSC and all five G1b branches on, GX off. Cc maps GPU_CC (RW).
// Fw/FwLen: the a660_gmu.bin image already read by the caller.
// *KeepCx is set TRUE only on EPI_GMU_E_TEARDOWN: the caller must then NOT collapse CX or turn the branches off
// (epi_smmu.hxx contract: never collapse CX after an unsuccessful Disable).
// Text/TextCch: diagnostic "name=value; " entries are appended.
// StartHfi: FALSE keeps PERF/BW-only behavior; TRUE adds CORE_FW_START then START (probe 0x280).
// VoteGx: requires StartHfi; adds GBIF QSB before release and GX vote after START (probe 0x680).
// FastClock: requires StartHfi/VoteGx; opt-in RCG setup/restore (probe 0xE80).
// GxOn: requires all three modifiers; OOB then slumber before reset, rpmh_stop after reset (probe 0x1E80).
// GxReg: requires GxOn and its prerequisites; optional six-register read under OOB (probe 0x3E80).
// ClearHalt: requires GxReg and its prerequisites; clear GBIF/RBBM halt after the six reads (probe 0x7E80).
// Secvid: requires ClearHalt and its prerequisites; stage A after halt clears, before OOB release (probe 0xFE80).
// AddrMode: requires Secvid and its prerequisites; stage B follows successful stage A (probe 0x1FE80).
// Hwcg: requires AddrMode and its prerequisites; stage C follows successful stage B (probe 0x3FE80).
// GxCfg: requires Hwcg and its prerequisites; stage D follows stage C under OOB (probe 0x7FE80).
// StageE: requires GxCfg and its prerequisites; follows successful stage D under OOB (probe 0xFFE80).
// Unrequested/unreached message NtStatus is STATUS_NOT_FOUND; unrecorded queue indices are 0xFFFFFFFF.
_IRQL_requires_(PASSIVE_LEVEL)
ULONG EpiGmuBootTest(HANDLE hParams, HANDLE hDiag, volatile ULONG* Cc, const UCHAR* Fw, ULONG FwLen,
                     PWCHAR Text, SIZE_T TextCch, BOOLEAN* KeepCx, BOOLEAN StartHfi = FALSE,
                     BOOLEAN VoteGx = FALSE, BOOLEAN FastClock = FALSE, BOOLEAN GxOn = FALSE, BOOLEAN GxReg = FALSE,
                     BOOLEAN ClearHalt = FALSE, BOOLEAN Secvid = FALSE, BOOLEAN AddrMode = FALSE, BOOLEAN Hwcg = FALSE,
                     BOOLEAN GxCfg = FALSE, BOOLEAN StageE = FALSE);
