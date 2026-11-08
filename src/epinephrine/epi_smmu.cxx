// SPDX-License-Identifier: MIT
// Original implementation. Hardware facts: Arm SMMUv2/SMMU-500, arm-smmu.h,
// io-pgtable-arm.c (register/descriptor definitions only; no Linux code copied).
#ifdef EPI_SMMU_HOST_TEST
#include "nt_shim.hxx"
#else
#include "BDD.hxx"
#include "epi_hw.hxx"
#endif
#include "epi_smmu.hxx"

static const ULONG Tag = 'smEE';
static const SIZE_T Page = 4096;
static const ULONGLONG IovaLimit = 1ULL << 32, PhysLimit = 1ULL << 36;
static const SIZE_T RegisterLength = 0x17000;
static const ULONG Cb = 0x10000;
// Snapshot order also restores TCR before TTBR; SCTLR/SMR are handled separately.
static const ULONG ConfigOffsets[] = {
    0x1800, 0x1000, Cb+0x10, Cb+0x30, Cb+0x20, Cb+0x24,
    Cb+0x28, Cb+0x2C, Cb+0x38, Cb+0x3C, 0xC00
};
struct EPI_TABLE { ULONGLONG* Va; ULONGLONG Pa; };
struct EPI_LEAVES { EPI_LEAVES* Next; ULONG Key; EPI_TABLE Table; };
struct EPI_SMMU {
    HANDLE Params;
    EPI_TABLE Root, L2[4];
    EPI_LEAVES* Leaves;
    ULONG Pages;
    volatile ULONG* Regs;
    volatile ULONG* Cc;
    volatile ULONG* Gmu;
    ULONG Original[RTL_NUMBER_OF(ConfigOffsets)];
    ULONG Sctlr, Smr;
    BOOLEAN Saved, Enabled;
    ULONG BadWriteStep, BadWriteOffset, BadWriteValue, BadWriteReadback;
    BOOLEAN HasBadWrite; // first mismatch for this context's lifetime; Disable must not overwrite it
    EPI_SMMU_DISABLE_WRITE DisableWrites[EPI_SMMU_DISABLE_WRITE_MAX];
    ULONG DisableWriteCount;
};
static_assert(RTL_NUMBER_OF(ConfigOffsets)==EPI_SMMU_DISABLE_WRITE_MAX,"SMMU disable diagnostic capacity");
static PVOID volatile Owner = NULL;

static NTSTATUS AllocateTable(EPI_TABLE* T)
{
    T->Va = static_cast<ULONGLONG*>(MmAllocateNonCachedMemory(Page));
    if (T->Va == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    T->Pa = MmGetPhysicalAddress(T->Va).QuadPart;
    if ((T->Pa & (Page-1)) != 0 || T->Pa > PhysLimit-Page)
    {
        MmFreeNonCachedMemory(T->Va, Page); T->Va = NULL;
        return STATUS_CONFLICTING_ADDRESSES;
    }
    RtlZeroMemory(T->Va, Page);
    return STATUS_SUCCESS;
}
static VOID FreeTable(EPI_TABLE* T)
{
    if (T->Va != NULL) { MmFreeNonCachedMemory(T->Va, Page); T->Va = NULL; }
}
static EPI_LEAVES* FindLeaves(EPI_SMMU* C, ULONGLONG Iova)
{
    const ULONG key = static_cast<ULONG>(Iova >> 21);
    for (EPI_LEAVES* p = C->Leaves; p != NULL; p = p->Next)
    { if (p->Key == key) { return p; } }
    return NULL;
}
static BOOLEAN Empty(const EPI_TABLE* T)
{
    for (ULONG i = 0; i < 512; ++i) { if (T->Va[i] != 0) { return FALSE; } }
    return TRUE;
}
// Only called while detached from hardware; removes allocation-failure debris too.
static VOID Prune(EPI_SMMU* C)
{
    EPI_LEAVES** link = &C->Leaves;
    while (*link != NULL)
    {
        EPI_LEAVES* p = *link;
        if (!Empty(&p->Table)) { link = &p->Next; continue; }
        C->L2[p->Key >> 9].Va[p->Key & 511] = 0;
        *link = p->Next; FreeTable(&p->Table); ExFreePoolWithTag(p, Tag);
    }
    for (ULONG i = 0; i < 4; ++i)
    {
        if (C->L2[i].Va != NULL && Empty(&C->L2[i]))
        { C->Root.Va[i] = 0; FreeTable(&C->L2[i]); }
    }
}
static BOOLEAN ValidRange(ULONGLONG Iova, SIZE_T Size)
{
    return Size != 0 && ((Iova | Size) & (Page-1)) == 0 &&
           Iova < IovaLimit && Size <= IovaLimit-Iova;
}

NTSTATUS EpiSmmuCreate(HANDLE Params, EPI_SMMU** Context)
{
    PAGED_CODE();
    if (Context == NULL) { return STATUS_INVALID_PARAMETER; }
    *Context = NULL;
    EPI_SMMU* c = static_cast<EPI_SMMU*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(EPI_SMMU), Tag));
    if (c == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    c->Params = Params;
    NTSTATUS st = AllocateTable(&c->Root);
    if (!NT_SUCCESS(st)) { ExFreePoolWithTag(c, Tag); return st; }
    *Context = c; return STATUS_SUCCESS;
}
NTSTATUS EpiSmmuDestroy(EPI_SMMU* C)
{
    PAGED_CODE();
    if (C == NULL) { return STATUS_INVALID_PARAMETER; }
    if (C->Regs != NULL || C->Saved) { return STATUS_DEVICE_BUSY; }
    while (C->Leaves != NULL)
    {
        EPI_LEAVES* p = C->Leaves; C->Leaves = p->Next;
        FreeTable(&p->Table); ExFreePoolWithTag(p, Tag);
    }
    for (ULONG i = 0; i < 4; ++i) { FreeTable(&C->L2[i]); }
    FreeTable(&C->Root); ExFreePoolWithTag(C, Tag); return STATUS_SUCCESS;
}
NTSTATUS EpiSmmuMap(EPI_SMMU* C, ULONGLONG Iova, PHYSICAL_ADDRESS Phys, SIZE_T Size, ULONG Flags)
{
    PAGED_CODE();
    const ULONGLONG pa = Phys.QuadPart;
    if (C == NULL || !ValidRange(Iova, Size) || (pa & (Page-1)) != 0 ||
        pa >= PhysLimit || Size > PhysLimit-pa || !(Flags & EPI_SMMU_READ) ||
        (Flags & ~(EPI_SMMU_READ|EPI_SMMU_WRITE|EPI_SMMU_PRIVILEGED)) != 0)
    { return STATUS_INVALID_PARAMETER; }
    if (C->Regs != NULL) { return STATUS_DEVICE_BUSY; }
    // Preflight the whole request: overlap never partially replaces an old map.
    for (ULONGLONG a = Iova; a < Iova+Size; a += Page)
    {
        EPI_LEAVES* p = FindLeaves(C, a);
        if (p != NULL && p->Table.Va[(a >> 12) & 511] != 0) { return STATUS_CONFLICTING_ADDRESSES; }
    }
    // Allocate all tables before committing any leaves. Failure removes empty tables.
    for (ULONGLONG a = Iova; a < Iova+Size;)
    {
        const ULONG l1 = static_cast<ULONG>(a >> 30);
        NTSTATUS st = STATUS_SUCCESS;
        if (C->L2[l1].Va == NULL)
        {
            st = AllocateTable(&C->L2[l1]);
            if (NT_SUCCESS(st)) { C->Root.Va[l1] = C->L2[l1].Pa | 3 | (1ULL<<63); } // NSTable
        }
        if (!NT_SUCCESS(st)) { Prune(C); return st; }
        if (FindLeaves(C, a) == NULL)
        {
            EPI_LEAVES* p = static_cast<EPI_LEAVES*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(EPI_LEAVES), Tag));
            if (p == NULL) { Prune(C); return STATUS_INSUFFICIENT_RESOURCES; }
            st = AllocateTable(&p->Table);
            if (!NT_SUCCESS(st)) { ExFreePoolWithTag(p, Tag); Prune(C); return st; }
            p->Key = static_cast<ULONG>(a >> 21); p->Next = C->Leaves; C->Leaves = p;
            C->L2[l1].Va[p->Key & 511] = p->Table.Pa | 3 | (1ULL<<63);
        }
        a = (a | ((1ULL << 21)-1)) + 1;
    }
    // Page descriptor: AF, NS, outer-shareable, AttrIndx0=Normal NC (MAIR=0x44).
    // AP[1] read-only; AP[0] permits unprivileged access. Execute is allowed for CM3 code.
    ULONGLONG attr = 3 | (1ULL<<11) | (1ULL<<10) | (2ULL<<8) | (1ULL<<5); // nG: ASID invalidation covers leaves
    if (!(Flags & EPI_SMMU_WRITE)) { attr |= 1ULL<<7; }
    if (!(Flags & EPI_SMMU_PRIVILEGED)) { attr |= 1ULL<<6; }
    for (ULONGLONG a = Iova; a < Iova+Size; a += Page)
    { FindLeaves(C, a)->Table.Va[(a >> 12) & 511] = (pa+a-Iova) | attr; }
    C->Pages += static_cast<ULONG>(Size/Page);
    KeMemoryBarrier(); return STATUS_SUCCESS;
}
NTSTATUS EpiSmmuUnmap(EPI_SMMU* C, ULONGLONG Iova, SIZE_T Size)
{
    PAGED_CODE();
    if (C == NULL || !ValidRange(Iova, Size)) { return STATUS_INVALID_PARAMETER; }
    if (C->Regs != NULL) { return STATUS_DEVICE_BUSY; }
    for (ULONGLONG a = Iova; a < Iova+Size; a += Page)
    {
        EPI_LEAVES* p = FindLeaves(C, a);
        if (p == NULL || p->Table.Va[(a >> 12) & 511] == 0) { return STATUS_NOT_FOUND; }
    }
    for (ULONGLONG a = Iova; a < Iova+Size; a += Page)
    { FindLeaves(C, a)->Table.Va[(a >> 12) & 511] = 0; }
    C->Pages -= static_cast<ULONG>(Size/Page);
    Prune(C); KeMemoryBarrier(); return STATUS_SUCCESS;
}

static BOOLEAN WindowReady(EPI_SMMU* C, BOOLEAN Reset)
{
    const ULONG clocks[] = {0x1078,0x109C,0x1098,0x1204,0x5000};
    // GPU_CC can be accessed via critical GCC_GPU_CFG_AHB; check it before GMU.
    if (!(READ_REGISTER_ULONG(C->Cc+0x106C/4) & 0x80000000UL)) { return FALSE; }
    for (ULONG i = 0; i < RTL_NUMBER_OF(clocks); ++i)
    { if (READ_REGISTER_ULONG(C->Cc+clocks[i]/4) & 0x80000000UL) { return FALSE; } }
    if (Reset && (READ_REGISTER_ULONG(C->Gmu) != 1 ||
        (READ_REGISTER_ULONG(C->Cc+0x100C/4) & 0x80000001UL) != 1 ||
        (READ_REGISTER_ULONG(C->Gmu+0x340/4) & 0xC0) != 0xC0)) { return FALSE; }
    return TRUE;
}
static ULONG ConfigCompareMask(ULONG Offset)
{
    // Ignore TCR2 bits 0x60 in readback comparisons, both on setup and restore.
    return Offset == Cb+0x10 ? 0xFFFFFF9FUL : 0xFFFFFFFFUL;
}
static NTSTATUS Write(EPI_SMMU* C, ULONG Offset, ULONG Value, ULONG Step,
                      ULONG CmpMask = 0xFFFFFFFFUL, ULONG* Readback = NULL, BOOLEAN* Written = NULL)
{
    if (Written != NULL) { *Written = FALSE; }
    if (C->Params != NULL)
    {
        NTSTATUS st = EpiSetDword(C->Params, L"EpiGpuProbeStep", Step);
        if (!NT_SUCCESS(st)) { return st; }
        st = ZwFlushKey(C->Params); if (!NT_SUCCESS(st)) { return st; }
    }
    WRITE_REGISTER_ULONG(C->Regs+Offset/4, Value);
    const ULONG readback = READ_REGISTER_ULONG(C->Regs+Offset/4);
    if (Written != NULL) { *Written = TRUE; }
    if (Readback != NULL) { *Readback = readback; }
    if ((readback & CmpMask) == (Value & CmpMask)) { return STATUS_SUCCESS; }
    if (!C->HasBadWrite)
    {
        C->BadWriteStep = Step; C->BadWriteOffset = Offset;
        C->BadWriteValue = Value; C->BadWriteReadback = readback;
        C->HasBadWrite = TRUE;
    }
    return STATUS_IO_DEVICE_ERROR;
}
NTSTATUS EpiSmmuQueryLastError(EPI_SMMU* C, ULONG* Step, ULONG* Offset, ULONG* Value, ULONG* Readback)
{
    PAGED_CODE();
    if (C == NULL || Step == NULL || Offset == NULL || Value == NULL || Readback == NULL)
    { return STATUS_INVALID_PARAMETER; }
    if (!C->HasBadWrite) { return STATUS_NOT_FOUND; }
    *Step = C->BadWriteStep; *Offset = C->BadWriteOffset;
    *Value = C->BadWriteValue; *Readback = C->BadWriteReadback;
    return STATUS_SUCCESS;
}
NTSTATUS EpiSmmuQueryCbar(EPI_SMMU* C, ULONG* Value)
{
    PAGED_CODE();
    if (C == NULL || Value == NULL) { return STATUS_INVALID_PARAMETER; }
    if (C->Regs == NULL || !WindowReady(C, FALSE)) { return STATUS_INVALID_DEVICE_STATE; }
    *Value = READ_REGISTER_ULONG(C->Regs+0x1000/4);
    return STATUS_SUCCESS;
}
NTSTATUS EpiSmmuQueryDisableWrites(EPI_SMMU* C, EPI_SMMU_DISABLE_WRITE* Writes, ULONG Capacity, ULONG* Count)
{
    PAGED_CODE();
    if (C == NULL || Count == NULL) { return STATUS_INVALID_PARAMETER; }
    *Count = C->DisableWriteCount;
    if (*Count == 0) { return STATUS_NOT_FOUND; }
    if (Writes == NULL || Capacity < *Count) { return STATUS_INVALID_PARAMETER; }
    for (ULONG i=0;i<*Count;++i) { Writes[i]=C->DisableWrites[i]; }
    return STATUS_SUCCESS;
}
static NTSTATUS Sync(EPI_SMMU* C, ULONG Step)
{
    // TLBIASID / TLBSYNC are command registers, not readable config registers.
    if (C->Params != NULL)
    {
        NTSTATUS st = EpiSetDword(C->Params, L"EpiGpuProbeStep", Step);
        if (!NT_SUCCESS(st)) { return st; }
        st = ZwFlushKey(C->Params); if (!NT_SUCCESS(st)) { return st; }
    }
    KeMemoryBarrier();
    WRITE_REGISTER_ULONG(C->Regs+(Cb+0x610)/4, 1); // ASID 1
    WRITE_REGISTER_ULONG(C->Regs+(Cb+0x7F0)/4, 0xFFFFFFFFUL);
    for (ULONG i = 0; i < 1000; ++i)
    {
        if (!(READ_REGISTER_ULONG(C->Regs+(Cb+0x7F4)/4) & 1))
        { KeMemoryBarrier(); return STATUS_SUCCESS; }
        KeStallExecutionProcessor(10);
    }
    return STATUS_IO_TIMEOUT;
}
static VOID ReleaseHardware(EPI_SMMU* C)
{
    MmUnmapIoSpace(const_cast<ULONG*>(C->Regs), RegisterLength);
    C->Regs = NULL; C->Cc = NULL; C->Gmu = NULL; C->Saved = FALSE; C->Enabled = FALSE;
    InterlockedExchangePointer(&Owner, NULL);
}
NTSTATUS EpiSmmuDisable(EPI_SMMU* C)
{
    PAGED_CODE();
    if (C == NULL) { return STATUS_INVALID_PARAMETER; }
    if (C->Regs == NULL) { return STATUS_SUCCESS; }
    if (!WindowReady(C, TRUE)) { return STATUS_INVALID_DEVICE_STATE; }
    // Keep tables and ownership on ANY failure; caller may retry teardown.
    NTSTATUS st = Write(C, 0x800, C->Smr & ~0x80000000UL, 0x520);
    if (!NT_SUCCESS(st)) { return st; }
    st = Write(C, Cb, 0, 0x521); if (!NT_SUCCESS(st)) { return st; }
    C->Enabled = FALSE;
    st = Sync(C, 0x522); if (!NT_SUCCESS(st)) { return st; }
    // Stream invalid, CB disabled, TLB sync complete: remaining config writes cannot start translation.
    // Continue only through MMIO readback mismatches. Persistence failures still stop the sweep.
    NTSTATUS restoreStatus = STATUS_SUCCESS;
    C->DisableWriteCount = 0;
    for (ULONG i = 0; i < RTL_NUMBER_OF(ConfigOffsets); ++i)
    {
        EPI_SMMU_DISABLE_WRITE* w = &C->DisableWrites[i];
        w->Step=0x530+i; w->Offset=ConfigOffsets[i]; w->Value=C->Original[i];
        w->Readback=0xFFFFFFFF; w->CmpMask=ConfigCompareMask(w->Offset);
        // Allow retained CBAR0 bits when its saved TYPE[17:16]=0 on this S1-only device; stream/CB are stopped.
        // Still write the original and record raw readback, but do not fail this inactive TYPE=0 restoration.
        if (w->Offset==0x1000 && (w->Value&0x00030000UL)==0) { w->CmpMask=0; }
        BOOLEAN written=FALSE;
        st = Write(C,w->Offset,w->Value,w->Step,w->CmpMask,&w->Readback,&written);
        w->Status=st; w->Written=written ? 1 : 0; C->DisableWriteCount=i+1;
        if (!NT_SUCCESS(st))
        {
            if (NT_SUCCESS(restoreStatus)) { restoreStatus=st; }
            if (!written) { return restoreStatus; }
        }
    }
    // Any mismatch keeps the CB/stream disabled and ownership/tables pinned; do not run the final restores.
    if (!NT_SUCCESS(restoreStatus)) { return restoreStatus; }
    st = Write(C, Cb, C->Sctlr, 0x53B); if (!NT_SUCCESS(st)) { return st; }
    st = Write(C, 0x800, C->Smr, 0x53C); if (!NT_SUCCESS(st)) { return st; }
    ReleaseHardware(C); return STATUS_SUCCESS;
}
NTSTATUS EpiSmmuEnable(EPI_SMMU* C, volatile ULONG* Cc, volatile ULONG* GmuA)
{
    PAGED_CODE();
    if (C == NULL || Cc == NULL || GmuA == NULL) { return STATUS_INVALID_PARAMETER; }
    if (C->Regs != NULL || C->Pages == 0) { return STATUS_INVALID_DEVICE_STATE; }
    if (InterlockedCompareExchangePointer(&Owner, C, NULL) != NULL) { return STATUS_DEVICE_BUSY; }
    C->Cc = Cc; C->Gmu = GmuA;
    if (!WindowReady(C, TRUE))
    { C->Cc = NULL; C->Gmu = NULL; InterlockedExchangePointer(&Owner, NULL); return STATUS_INVALID_DEVICE_STATE; }
    PHYSICAL_ADDRESS pa; pa.QuadPart = 0x03DA0000ULL;
    C->Regs = static_cast<volatile ULONG*>(MmMapIoSpaceEx(pa, RegisterLength, PAGE_READWRITE|PAGE_NOCACHE));
    if (C->Regs == NULL)
    { C->Cc = NULL; C->Gmu = NULL; InterlockedExchangePointer(&Owner, NULL); return STATUS_INSUFFICIENT_RESOURCES; }
    volatile ULONG* s = C->Regs;
    // Only the observed stage-1-only layout, USFCFG fault policy, no bypass/EXID modes.
    if (READ_REGISTER_ULONG(s+0x20/4) != 0x4C017E06 || READ_REGISTER_ULONG(s+0x24/4) != 0x30000007 ||
        READ_REGISTER_ULONG(s+0x28/4) != 0x5511 || READ_REGISTER_ULONG(s+0x3C/4) != 0x24 ||
        READ_REGISTER_ULONG(s) != 0x002C0406)
    { ReleaseHardware(C); return STATUS_NOT_SUPPORTED; }
    for (ULONG i = 0; i < 6; ++i)
    { if (READ_REGISTER_ULONG(s+(0x800+i*4)/4) & 0x80000000UL) { ReleaseHardware(C); return STATUS_DEVICE_BUSY; } }
    for (ULONG i = 0; i < 7; ++i)
    { if (READ_REGISTER_ULONG(s+(Cb+i*0x1000)/4) & 0x61) { ReleaseHardware(C); return STATUS_DEVICE_BUSY; } }
    // Preserve all registers before any write. Never write sCR0 or clear fault status.
    C->Sctlr = READ_REGISTER_ULONG(s+Cb/4); C->Smr = READ_REGISTER_ULONG(s+0x800/4);
    for (ULONG i = 0; i < RTL_NUMBER_OF(ConfigOffsets); ++i)
    { C->Original[i] = READ_REGISTER_ULONG(s+ConfigOffsets[i]/4); }
    C->Saved = TRUE;
    const ULONG values[] = {
        1, 0x1F300, 0x38011, 0x802020, // VA64, S1, SEP7/AS16/PA36, EPD1/OSH/NC/T0SZ32
        static_cast<ULONG>(C->Root.Pa), static_cast<ULONG>(C->Root.Pa>>32) | 0x10000,
        0, 0x10000, 0x44, 0, 0       // TTBR1 disabled; ASID1, MAIR NC, S2CR CB0
    };
    KeMemoryBarrier();
    NTSTATUS st = Write(C, Cb, 0, 0x500);
    for (ULONG i = 0; NT_SUCCESS(st) && i < RTL_NUMBER_OF(values); ++i)
    { st = Write(C, ConfigOffsets[i], values[i], 0x501+i, ConfigCompareMask(ConfigOffsets[i])); }
    if (NT_SUCCESS(st)) { st = Sync(C, 0x50C); }
    // ASIDPNE/HUPCF/CFRE/AFE/TRE/M: record faults for QueryFault,
    // but CFIE=0 (no IRQ) and CFCFG=0 (no stall on a fault).
    // Ignore SCTLR bit12 (ASIDPNE) in this enable write's readback comparison.
    if (NT_SUCCESS(st)) { st = Write(C, Cb, 0x1127, 0x50D, 0xFFFFEFFFUL); }
    if (NT_SUCCESS(st)) { st = Write(C, 0x800, 0x84000005, 0x50E); }
    if (NT_SUCCESS(st)) { C->Enabled = TRUE; return STATUS_SUCCESS; }
    const NTSTATUS cleanup = EpiSmmuDisable(C);
    return NT_SUCCESS(cleanup) ? st : cleanup;
}
NTSTATUS EpiSmmuQueryFault(EPI_SMMU* C, ULONG* Fsr, ULONGLONG* Far, ULONG* Fsynr0)
{
    PAGED_CODE();
    if (C == NULL || Fsr == NULL || Far == NULL || Fsynr0 == NULL) { return STATUS_INVALID_PARAMETER; }
    *Fsr = 0; *Far = 0; *Fsynr0 = 0;
    if (!C->Enabled || !WindowReady(C, FALSE)) { return STATUS_INVALID_DEVICE_STATE; }
    // Double-read FSR and FAR high to reject an obviously changing snapshot.
    for (ULONG i = 0; i < 3; ++i)
    {
        ULONG fsr = READ_REGISTER_ULONG(C->Regs+(Cb+0x58)/4);
        ULONG hi = READ_REGISTER_ULONG(C->Regs+(Cb+0x64)/4);
        ULONG lo = READ_REGISTER_ULONG(C->Regs+(Cb+0x60)/4);
        ULONG syn = READ_REGISTER_ULONG(C->Regs+(Cb+0x68)/4);
        if (hi == READ_REGISTER_ULONG(C->Regs+(Cb+0x64)/4) &&
            fsr == READ_REGISTER_ULONG(C->Regs+(Cb+0x58)/4))
        { *Fsr = fsr; *Far = (static_cast<ULONGLONG>(hi)<<32)|lo; *Fsynr0 = syn; return STATUS_SUCCESS; }
    }
    return STATUS_RETRY;
}
