// SPDX-License-Identifier: MIT
#pragma once
// CPU-only first-fit allocator for an already mapped 256-KB GPU region. Caller serializes access.
// Bitmap boundary markers reject partial frees and frees spanning adjacent allocations.
struct EPI_GPU_HEAP {
    ULONG Used[128],Freed[128],Starts[128],Ends[128];
    struct PENDING { ULONG Offset,Size,Fence; } Pending[32];
    ULONG LiveBytes,PeakBytes,HighWater,AllocCount,FreeCount,FailCount,ReuseCount,RetirePending;
};
inline VOID EpiGpuHeapInit(EPI_GPU_HEAP* H) { RtlZeroMemory(H,sizeof(*H)); }
inline BOOLEAN EpiGpuHeapBit(const ULONG* B,ULONG I) { return (B[I/32]&(1UL<<(I%32)))!=0; }
inline VOID EpiGpuHeapSet(ULONG* B,ULONG I,BOOLEAN Value)
{
    if (Value) { B[I/32]|=1UL<<(I%32); } else { B[I/32]&=~(1UL<<(I%32)); }
}
inline BOOLEAN EpiGpuHeapRange(ULONG Offset,ULONG Size)
{
    return Size!=0 && Size<=0x40000 && (Offset&63)==0 && Offset<0x40000 && ((Size+63)&~63UL)<=0x40000-Offset;
}
inline BOOLEAN EpiGpuHeapOwned(const EPI_GPU_HEAP* H,ULONG Offset,ULONG Size)
{
    if (!EpiGpuHeapRange(Offset,Size)) { return FALSE; }
    const ULONG first=Offset/64,count=(Size+63)/64;
    if (!EpiGpuHeapBit(H->Starts,first) || !EpiGpuHeapBit(H->Ends,first+count-1)) { return FALSE; }
    for (ULONG j=0;j<count;++j)
        if (!EpiGpuHeapBit(H->Used,first+j) || (j && EpiGpuHeapBit(H->Starts,first+j))) { return FALSE; }
    return TRUE;
}
inline ULONG EpiGpuHeapAlloc(EPI_GPU_HEAP* H,ULONG Size,ULONG Align)
{
    if (Size==0 || Size>0x40000 || (Align!=64 && Align!=128 && Align!=256)) { ++H->FailCount; return 0xFFFFFFFF; }
    const ULONG count=(Size+63)/64,step=Align/64;
    for (ULONG first=0;first<=4096-count;first+=step)
    {
        BOOLEAN available=TRUE,reused=FALSE;
        for (ULONG j=0;j<count;++j)
        {
            if (EpiGpuHeapBit(H->Used,first+j)) { available=FALSE; break; }
            if (EpiGpuHeapBit(H->Freed,first+j)) { reused=TRUE; }
        }
        if (!available) { continue; }
        for (ULONG j=0;j<count;++j) { EpiGpuHeapSet(H->Used,first+j,TRUE); }
        EpiGpuHeapSet(H->Starts,first,TRUE); EpiGpuHeapSet(H->Ends,first+count-1,TRUE);
        H->LiveBytes+=count*64; if (H->LiveBytes>H->PeakBytes) { H->PeakBytes=H->LiveBytes; }
        const ULONG end=(first+count)*64; if (end>H->HighWater) { H->HighWater=end; }
        ++H->AllocCount; if (reused) { ++H->ReuseCount; }
        return first*64;
    }
    ++H->FailCount; return 0xFFFFFFFF;
}
inline BOOLEAN EpiGpuHeapRelease(EPI_GPU_HEAP* H,ULONG Offset,ULONG Size)
{
    if (!EpiGpuHeapOwned(H,Offset,Size)) { ++H->FailCount; return FALSE; }
    const ULONG first=Offset/64,count=(Size+63)/64;
    for (ULONG j=0;j<count;++j) { EpiGpuHeapSet(H->Used,first+j,FALSE); EpiGpuHeapSet(H->Freed,first+j,TRUE); }
    EpiGpuHeapSet(H->Starts,first,FALSE); EpiGpuHeapSet(H->Ends,first+count-1,FALSE);
    H->LiveBytes-=count*64; ++H->FreeCount; return TRUE;
}
inline BOOLEAN EpiGpuHeapFree(EPI_GPU_HEAP* H,ULONG Offset,ULONG Size)
{
    for (ULONG i=0;i<32;++i) if (H->Pending[i].Size && H->Pending[i].Offset==Offset) { ++H->FailCount; return FALSE; }
    return EpiGpuHeapRelease(H,Offset,Size);
}
inline BOOLEAN EpiGpuHeapRetire(EPI_GPU_HEAP* H,ULONG Offset,ULONG Size,ULONG Fence)
{
    if (!EpiGpuHeapOwned(H,Offset,Size)) { ++H->FailCount; return FALSE; }
    ULONG empty=32;
    for (ULONG i=0;i<32;++i)
    {
        if (!H->Pending[i].Size) { if (empty==32) { empty=i; } }
        else if (H->Pending[i].Offset==Offset) { ++H->FailCount; return FALSE; }
    }
    if (empty==32) { ++H->FailCount; return FALSE; }
    H->Pending[empty].Offset=Offset; H->Pending[empty].Size=Size; H->Pending[empty].Fence=Fence;
    ++H->RetirePending; return TRUE;
}
inline BOOLEAN EpiGpuHeapReclaim(EPI_GPU_HEAP* H,ULONG Completed)
{
    for (ULONG i=0;i<32;++i) if (H->Pending[i].Size && H->Pending[i].Fence<=Completed)
    {
        if (!EpiGpuHeapRelease(H,H->Pending[i].Offset,H->Pending[i].Size)) { return FALSE; }
        H->Pending[i].Size=0; --H->RetirePending;
    }
    return TRUE;
}
