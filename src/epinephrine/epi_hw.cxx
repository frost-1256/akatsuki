// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - read-only display block snapshot (WP2b-1). New file, not derived from KMDOD.

#include "BDD.hxx"
#include "epi_hw.hxx"
extern "C" {
#include <ntstrsafe.h>
}

#pragma code_seg("PAGE")

// Sub-blocks of MDP_REGS (offsets from EPI_MDP_REGS_BASE). This is exactly the set that
// MdssSnap v1.1+ read without faults in the bootloader state on spacewar (2026-11-06/07).
// Do NOT add pp0 (0x69000) / lm0 (0x44000): reading 0x0AE69000 caused WHEA_UNCORRECTABLE_ERROR.
typedef struct _EPI_BLOCK { ULONG Offset; ULONG Size; PCWSTR Name; } EPI_BLOCK;
static const EPI_BLOCK g_EpiBlocks[] = {
    { 0x00000, 0x020, L"mdss"     },  // MDSS wrapper: HW_VERSION +0x0, HW_INTR_STATUS +0x10 (mainline reads these at probe; not in MdssSnap)
    { 0x01000, 0x400, L"mdp_top"  },
    { 0x16000, 0x200, L"ctl0"     },
    { 0x36000, 0x300, L"intf1"    },
    { 0x81000, 0x200, L"dsc0_enc" },
    { 0x81F00, 0x040, L"dsc0_ctl" },
    { 0x94000, 0x400, L"dsi0"     },
    { 0x94400, 0x200, L"dsi0_phy" },
    { 0x94900, 0x280, L"dsi0_pll" },
};
#define EPI_MAX_BLOCK 0x400
#define EPI_RES_TEXT_CCH 1024

NTSTATUS EpiSetValue(HANDLE hKey, PCWSTR Name, ULONG Type, const VOID* Data, ULONG Size)
{
    PAGED_CODE();
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, Name);
    return ZwSetValueKey(hKey, &n, 0, Type, const_cast<PVOID>(Data), Size);
}

NTSTATUS EpiSetDword(HANDLE hKey, PCWSTR Name, ULONG Value)
{
    PAGED_CODE();
    return EpiSetValue(hKey, Name, REG_DWORD, &Value, sizeof(Value));
}

ULONG EpiQueryDword(HANDLE hKey, PCWSTR Name, ULONG Default)
{
    PAGED_CODE();
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, Name);
    DECLSPEC_ALIGN(8) UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    ULONG len = 0;
    NTSTATUS st = ZwQueryValueKey(hKey, &n, KeyValuePartialInformation, buf, sizeof(buf), &len);
    if (!NT_SUCCESS(st)) { return Default; }
    PKEY_VALUE_PARTIAL_INFORMATION p = reinterpret_cast<PKEY_VALUE_PARTIAL_INFORMATION>(buf);
    if (p->Type != REG_DWORD || p->DataLength != sizeof(ULONG)) { return Default; }
    return *reinterpret_cast<ULONG UNALIGNED*>(p->Data);
}

NTSTATUS EpiOpenDiag(PDEVICE_OBJECT Pdo, HANDLE* phParams, HANDLE* phDiag)
{
    PAGED_CODE();
    *phParams = NULL;
    *phDiag = NULL;
    HANDLE hParams = NULL;
    NTSTATUS st = IoOpenDeviceRegistryKey(Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ | KEY_WRITE, &hParams);
    if (!NT_SUCCESS(st)) { return st; }

    HANDLE hDiag = NULL;
    UNICODE_STRING diagName;
    RtlInitUnicodeString(&diagName, L"EpiDiag");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &diagName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, hParams, NULL);
    st = ZwCreateKey(&hDiag, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_VOLATILE, NULL);
    if (!NT_SUCCESS(st)) { ZwClose(hParams); return st; }
    *phParams = hParams;
    *phDiag = hDiag;
    return STATUS_SUCCESS;
}

// Walk the translated resources; classify by type and address only.
static VOID EpiScanResources(const DXGK_DEVICE_INFO* pDeviceInfo, EPI_HW_RESOURCES* pRes, PWCHAR Text, size_t Cch)
{
    PAGED_CODE();
    const CM_RESOURCE_LIST* list = pDeviceInfo->TranslatedResourceList;
    Text[0] = L'\0';
    if (list == NULL) { return; }
    const CM_FULL_RESOURCE_DESCRIPTOR* full = &list->List[0];
    for (ULONG i = 0; i < list->Count; ++i)
    {
        const CM_PARTIAL_RESOURCE_LIST* pl = &full->PartialResourceList;
        for (ULONG j = 0; j < pl->Count; ++j)
        {
            const CM_PARTIAL_RESOURCE_DESCRIPTOR* d = &pl->PartialDescriptors[j];
            WCHAR line[96];
            line[0] = L'\0';
            switch (d->Type)
            {
            case CmResourceTypeMemory:
            {
                ULONGLONG start = (ULONGLONG)d->u.Memory.Start.QuadPart;
                ULONG len = d->u.Memory.Length;
                pRes->MemoryCount++;
                if (start == EPI_MDP_REGS_BASE && len == EPI_MDP_REGS_LEN)
                {
                    pRes->MdpFound = TRUE;
                    pRes->MdpBase = d->u.Memory.Start;
                    pRes->MdpLength = len;
                }
                else if (start == EPI_DP_PHY_REGS_BASE && len == EPI_DP_PHY_REGS_LEN)
                {
                    pRes->DpPhyFound = TRUE;
                }
                else if (start == EPI_GPU_CC_BASE && len == EPI_GPU_CC_LEN)
                {
                    pRes->GpuCcFound = TRUE;
                }
                RtlStringCchPrintfW(line, RTL_NUMBER_OF(line), L"mem %I64X+%X; ", start, len);
                break;
            }
            case CmResourceTypeInterrupt:
                pRes->InterruptCount++;
                RtlStringCchPrintfW(line, RTL_NUMBER_OF(line), L"irq v%X f%X; ", d->u.Interrupt.Vector, d->Flags);
                break;
            case CmResourceTypeConnection:
                pRes->ConnectionCount++;
                RtlStringCchPrintfW(line, RTL_NUMBER_OF(line), L"conn c%X t%X id%X:%X; ",
                                    d->u.Connection.Class, d->u.Connection.Type,
                                    d->u.Connection.IdHighPart, d->u.Connection.IdLowPart);
                break;
            default:
                RtlStringCchPrintfW(line, RTL_NUMBER_OF(line), L"type%u; ", d->Type);
                break;
            }
            RtlStringCchCatW(Text, Cch, line);
        }
        // Next full descriptor follows the last partial descriptor.
        full = reinterpret_cast<const CM_FULL_RESOURCE_DESCRIPTOR*>(&pl->PartialDescriptors[pl->Count]);
    }
}

VOID EpiHwSnapshot(_In_ const DXGK_DEVICE_INFO* pDeviceInfo, _Out_ EPI_HW_RESOURCES* pRes)
{
    PAGED_CODE();
    RtlZeroMemory(pRes, sizeof(*pRes));

    HANDLE hParams = NULL;
    HANDLE hDiag = NULL;
    if (!NT_SUCCESS(EpiOpenDiag(pDeviceInfo->PhysicalDeviceObject, &hParams, &hDiag))) { return; }

    // Version of this snapshot format; bump when blocks or values change.
    EpiSetDword(hDiag, L"SnapVersion", 1);

    PWCHAR text = static_cast<PWCHAR>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_RES_TEXT_CCH * sizeof(WCHAR), 'ipEE'));
    if (text != NULL)
    {
        EpiScanResources(pDeviceInfo, pRes, text, EPI_RES_TEXT_CCH);
        EpiSetValue(hDiag, L"Resources", REG_SZ, text, (ULONG)((wcslen(text) + 1) * sizeof(WCHAR)));
        ExFreePoolWithTag(text, 'ipEE');
    }
    EpiSetDword(hDiag, L"MdpFound", pRes->MdpFound);
    EpiSetDword(hDiag, L"DpPhyFound", pRes->DpPhyFound);
    EpiSetDword(hDiag, L"MemoryCount", pRes->MemoryCount);
    EpiSetDword(hDiag, L"InterruptCount", pRes->InterruptCount);
    EpiSetDword(hDiag, L"ConnectionCount", pRes->ConnectionCount);

    // 0 = done, 1 = disabled by EpiDisableHwSnap, 2 = MDP_REGS not in resources, 3 = no buffer
    ULONG status = 0;
    if (EpiQueryDword(hParams, L"EpiDisableHwSnap", 0) != 0)
    {
        status = 1;
    }
    else if (!pRes->MdpFound)
    {
        status = 2;
    }
    else
    {
        PULONG buf = static_cast<PULONG>(ExAllocatePool2(POOL_FLAG_NON_PAGED, EPI_MAX_BLOCK, 'ipEE'));
        if (buf == NULL)
        {
            status = 3;
        }
        else
        {
            ULONG mapFail = 0;
            for (ULONG b = 0; b < RTL_NUMBER_OF(g_EpiBlocks); ++b)
            {
                const EPI_BLOCK* blk = &g_EpiBlocks[b];
                if (blk->Offset + blk->Size > pRes->MdpLength || blk->Size > EPI_MAX_BLOCK) { mapFail++; continue; }
                PHYSICAL_ADDRESS pa;
                pa.QuadPart = pRes->MdpBase.QuadPart + blk->Offset;
                PVOID va = MmMapIoSpaceEx(pa, blk->Size, PAGE_READONLY | PAGE_NOCACHE);
                if (va == NULL) { mapFail++; continue; }
                for (ULONG off = 0; off < blk->Size; off += sizeof(ULONG))
                {
                    buf[off / sizeof(ULONG)] = READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG*>(static_cast<PUCHAR>(va) + off));
                }
                MmUnmapIoSpace(va, blk->Size);
                EpiSetValue(hDiag, blk->Name, REG_BINARY, buf, blk->Size);
            }
            ExFreePoolWithTag(buf, 'ipEE');
            EpiSetDword(hDiag, L"MapFailures", mapFail);
        }
    }
    EpiSetDword(hDiag, L"SnapStatus", status);

    ZwClose(hDiag);
    ZwClose(hParams);
}

// ---- VSync probe (WP2b-3a): read-only timing observation ----
// Candidates (mainline DPU names): INTF_FRAME_COUNT +0xAC, INTF_LINE_COUNT +0xB0, tear-check +0x298 / +0x2B0,
// MDSS HW_INTR_STATUS +0x10. All of them were already read safely by the WP2b-1 snapshot.
#define EPI_PROBE_INTF_OFF      0x36000
#define EPI_PROBE_INTF_SIZE     0x300
#define EPI_PROBE_SAMPLES       400     // x 250 us = ~100 ms (6 frames at 60 Hz)
#define EPI_PROBE_STALL_US      250
#define EPI_PROBE_FIELDS        8       // t_us, +0xAC, +0xB0, +0x298, +0x2B0, mdss+0x10, +0x1C4, tear+0x4
// INTF TE interrupt block (mainline MDP_INTF_REV_7xxx_TEAR_OFF = INTF + 0x800: EN +0x0, STATUS +0x4, CLEAR +0x8).
// Not read before WP2b-3c; read only when EpiVsyncProbe bit1 is set, and that bit is cleared first (one shot),
// so a fault on this address cannot repeat at every boot.
#define EPI_PROBE_TEAR_OFF      0x36800
#define EPI_PROBE_TEAR_SIZE     0x40
#define EPI_PROBE_DIFF_CCH      1024

VOID EpiVsyncProbe(_In_ const EPI_HW_RESOURCES* pRes, _In_ PDEVICE_OBJECT Pdo)
{
    PAGED_CODE();
    HANDLE hParams = NULL;
    HANDLE hDiag = NULL;
    if (!NT_SUCCESS(EpiOpenDiag(Pdo, &hParams, &hDiag))) { return; }

    // 0 = done, 1 = not enabled (EpiVsyncProbe == 0), 2 = no MDP_REGS, 3 = no memory, 4 = map failed
    ULONG status = 0;
    PULONG samples = NULL;
    PULONG before = NULL;
    PULONG after = NULL;
    PWCHAR diff = NULL;
    PVOID intf = NULL;
    PVOID mdss = NULL;
    PVOID tear = NULL;
    ULONG tearBefore[EPI_PROBE_TEAR_SIZE / 4] = {};
    ULONG tearAfter[EPI_PROBE_TEAR_SIZE / 4] = {};
    ULONG probeFlags = 0;
    const ULONG sampleBytes = EPI_PROBE_SAMPLES * EPI_PROBE_FIELDS * sizeof(ULONG);

    probeFlags = EpiQueryDword(hParams, L"EpiVsyncProbe", 0);
    if (probeFlags == 0) { status = 1; goto done; }
    if (probeFlags & 2)
    {
        EpiSetDword(hParams, L"EpiVsyncProbe", probeFlags & ~2UL);   // one shot (see EPI_PROBE_TEAR_OFF)
        EpiSetDword(hParams, L"EpiProbeTearAttempt", 1);              // non-volatile breadcrumb, 2 = read ok
        ZwFlushKey(hParams);                                          // must reach disk before the read
    }
    // bit2 (WP2b-3d, one shot): write-1-to-clear the INTF (+0x1C8) and TE (+0x808) interrupt status during
    // sampling, so each latched bit shows when it fires. The enables stay 0, so no interrupt is raised. These are
    // the only hardware writes Epinephrine makes; mainline's IRQ handler does the same writes.
    const BOOLEAN doClear = (probeFlags & 4) ? TRUE : FALSE;
    if (doClear)
    {
        EpiSetDword(hParams, L"EpiVsyncProbe", probeFlags & ~6UL);
        EpiSetDword(hParams, L"EpiProbeClearAttempt", 1);             // 2 = finished
        ZwFlushKey(hParams);
    }
    const ULONG mapProt = (doClear ? PAGE_READWRITE : PAGE_READONLY) | PAGE_NOCACHE;
    if (!pRes->MdpFound || pRes->MdpLength < EPI_PROBE_INTF_OFF + EPI_PROBE_INTF_SIZE) { status = 2; goto done; }

    samples = static_cast<PULONG>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sampleBytes, 'ipEE'));
    before = static_cast<PULONG>(ExAllocatePool2(POOL_FLAG_NON_PAGED, EPI_PROBE_INTF_SIZE, 'ipEE'));
    after = static_cast<PULONG>(ExAllocatePool2(POOL_FLAG_NON_PAGED, EPI_PROBE_INTF_SIZE, 'ipEE'));
    diff = static_cast<PWCHAR>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_PROBE_DIFF_CCH * sizeof(WCHAR), 'ipEE'));
    if (samples == NULL || before == NULL || after == NULL || diff == NULL) { status = 3; goto done; }

    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = pRes->MdpBase.QuadPart + EPI_PROBE_INTF_OFF;
        intf = MmMapIoSpaceEx(pa, EPI_PROBE_INTF_SIZE, mapProt);
        pa.QuadPart = pRes->MdpBase.QuadPart;
        mdss = MmMapIoSpaceEx(pa, 0x20, PAGE_READONLY | PAGE_NOCACHE);
        if (intf == NULL || mdss == NULL) { status = 4; goto done; }
        if (probeFlags & 6)
        {
            pa.QuadPart = pRes->MdpBase.QuadPart + EPI_PROBE_TEAR_OFF;
            tear = MmMapIoSpaceEx(pa, EPI_PROBE_TEAR_SIZE, mapProt);
            if (tear == NULL) { status = 4; goto done; }
        }
    }

    {
        volatile ULONG* r = static_cast<volatile ULONG*>(intf);
        volatile ULONG* m = static_cast<volatile ULONG*>(mdss);
        volatile ULONG* te = static_cast<volatile ULONG*>(tear);
        for (ULONG i = 0; i < EPI_PROBE_INTF_SIZE / sizeof(ULONG); ++i) { before[i] = READ_REGISTER_ULONG(r + i); }
        if (te != NULL)
        {
            for (ULONG i = 0; i < EPI_PROBE_TEAR_SIZE / 4; ++i) { tearBefore[i] = READ_REGISTER_ULONG(te + i); }
            EpiSetDword(hParams, L"EpiProbeTearAttempt", 2);
        }
        if (doClear)
        {
            WRITE_REGISTER_ULONG(r + 0x1C8 / 4, 0xFFFFFFFF);
            if (te != NULL) { WRITE_REGISTER_ULONG(te + 2, 0xFFFFFFFF); }
        }

        LARGE_INTEGER freq;
        const LARGE_INTEGER t0 = KeQueryPerformanceCounter(&freq);
        for (ULONG s = 0; s < EPI_PROBE_SAMPLES; ++s)
        {
            PULONG o = samples + s * EPI_PROBE_FIELDS;
            const LARGE_INTEGER t = KeQueryPerformanceCounter(NULL);
            o[0] = (ULONG)(((t.QuadPart - t0.QuadPart) * 1000000) / freq.QuadPart);
            o[1] = READ_REGISTER_ULONG(r + 0xAC / 4);
            o[2] = READ_REGISTER_ULONG(r + 0xB0 / 4);
            o[3] = READ_REGISTER_ULONG(r + 0x298 / 4);
            o[4] = READ_REGISTER_ULONG(r + 0x2B0 / 4);
            o[5] = READ_REGISTER_ULONG(m + 0x10 / 4);
            o[6] = READ_REGISTER_ULONG(r + 0x1C4 / 4);
            o[7] = te != NULL ? READ_REGISTER_ULONG(te + 1) : 0;
            if (doClear)
            {
                if (o[6] != 0) { WRITE_REGISTER_ULONG(r + 0x1C8 / 4, o[6]); }
                if (o[7] != 0 && te != NULL) { WRITE_REGISTER_ULONG(te + 2, o[7]); }
            }
            KeStallExecutionProcessor(EPI_PROBE_STALL_US);
        }

        for (ULONG i = 0; i < EPI_PROBE_INTF_SIZE / sizeof(ULONG); ++i) { after[i] = READ_REGISTER_ULONG(r + i); }
        if (te != NULL)
        {
            for (ULONG i = 0; i < EPI_PROBE_TEAR_SIZE / 4; ++i) { tearAfter[i] = READ_REGISTER_ULONG(te + i); }
            EpiSetValue(hDiag, L"ProbeTearBefore", REG_BINARY, tearBefore, sizeof(tearBefore));
            EpiSetValue(hDiag, L"ProbeTearAfter", REG_BINARY, tearAfter, sizeof(tearAfter));
        }
        if (doClear) { EpiSetDword(hParams, L"EpiProbeClearAttempt", 2); }
    }

    diff[0] = 0;
    for (ULONG i = 0; i < EPI_PROBE_INTF_SIZE / sizeof(ULONG); ++i)
    {
        if (before[i] == after[i]) { continue; }
        WCHAR item[40];
        if (NT_SUCCESS(RtlStringCchPrintfW(item, RTL_NUMBER_OF(item), L"%X:%X>%X;", i * 4, before[i], after[i])))
        {
            RtlStringCchCatW(diff, EPI_PROBE_DIFF_CCH, item);   // truncation is fine
        }
    }
    EpiSetValue(hDiag, L"ProbeSamples", REG_BINARY, samples, sampleBytes);
    EpiSetValue(hDiag, L"ProbeIntfDiff", REG_SZ, diff, (ULONG)((wcslen(diff) + 1) * sizeof(WCHAR)));

done:
    EpiSetDword(hDiag, L"ProbeStatus", status);
    if (intf != NULL) { MmUnmapIoSpace(intf, EPI_PROBE_INTF_SIZE); }
    if (mdss != NULL) { MmUnmapIoSpace(mdss, 0x20); }
    if (tear != NULL) { MmUnmapIoSpace(tear, EPI_PROBE_TEAR_SIZE); }
    if (diff != NULL) { ExFreePoolWithTag(diff, 'ipEE'); }
    if (after != NULL) { ExFreePoolWithTag(after, 'ipEE'); }
    if (before != NULL) { ExFreePoolWithTag(before, 'ipEE'); }
    if (samples != NULL) { ExFreePoolWithTag(samples, 'ipEE'); }
    ZwClose(hDiag);
    ZwClose(hParams);
}
