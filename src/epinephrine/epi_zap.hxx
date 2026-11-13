// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#define EPI_ZAP_FILE_SIZE 14264UL
#define EPI_ZAP_PROBE_QUERY 0x40000000UL // standalone Z-1b; incompatible with every other probe bit
struct EPI_SMC_REGS { ULONGLONG X[8]; };
static_assert(sizeof(EPI_SMC_REGS)==64,"SMC register ABI");
extern "C" VOID EpiSmcInvoke(EPI_SMC_REGS* Registers);
struct EPI_ZAP_CALL {
    ULONGLONG Function, ArgInfo, Args[3], Result[4], Session;
    ULONG Calls, Interrupted, Busy, Completed;
};
static_assert(sizeof(EPI_ZAP_CALL)==96,"PAS call ABI");
struct EPI_ZAP_TRACE {
    ULONG Step, FileSize, Header[13], Phdr[3][8];
    ULONG SupportX1Ignored; // Z-1c: replaces former padding at byte156; 1 once Call[0] x0=0 ignores x1, 0 before that
    ULONGLONG MetadataPa;
    ULONG MetadataBytes, TargetBytes, TargetReadback[4], InitSucceeded, Consumed, MetadataKept;
    NTSTATUS Status;
    EPI_ZAP_CALL Call[4];
};
static_assert(sizeof(EPI_ZAP_TRACE)==592,"PAS trace ABI");
static_assert(offsetof(EPI_ZAP_TRACE,SupportX1Ignored)==156 && offsetof(EPI_ZAP_TRACE,MetadataPa)==160 &&
              offsetof(EPI_ZAP_TRACE,Call)==208,
              "Z-1c must preserve the former padding and every later field offset");
// GpuGmuZ1Pas REG_BINARY: this exact layout. Header has the 13 decoded ELF header fields after e_ident.
// SupportX1Ignored: 0 while not yet applied, 1 after completed IS_SUPPORTED with x0=0 (raw x1 retained).
// Old Z-1 traces and unreached PAS snapshots have FFFFFFFF at that offset. No later ABI offsets change.
typedef NTSTATUS (*EPI_ZAP_CHECKPOINT)(PVOID Context,ULONG Step);
BOOLEAN EpiZapValidate(const UCHAR* Image,ULONG Length);
NTSTATUS EpiZapLoad(HANDLE Params,HANDLE Diag,const UCHAR* Image,ULONG Length,EPI_ZAP_TRACE* Trace,
                    EPI_ZAP_CHECKPOINT Checkpoint,PVOID Context);
struct EPI_ZAP_QUERY_ROW {
    ULONGLONG Function,Arg,X0,X1,X2,X3,X6;
    ULONG Calls,Flags; // bit0 interrupted, bit1 busy, bit2 retry limit reached
};
struct EPI_ZAP_QUERY_TRACE {
    ULONG Count,Status; // Count: rows with at least one returned SMC; Status: first infrastructure/limit NTSTATUS
    EPI_ZAP_QUERY_ROW Row[12];
};
static_assert(sizeof(EPI_ZAP_QUERY_ROW)==64,"SCM query row ABI");
static_assert(sizeof(EPI_ZAP_QUERY_TRACE)==776,"SCM query trace ABI");
// Read-only queries only: no PAS state, ZapOnce, file/physical-memory access, or GPU/GMU/SMMU access.
NTSTATUS EpiZapQuery(HANDLE Params,HANDLE Diag,EPI_ZAP_QUERY_TRACE* Trace);
