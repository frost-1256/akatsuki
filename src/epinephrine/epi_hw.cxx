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
