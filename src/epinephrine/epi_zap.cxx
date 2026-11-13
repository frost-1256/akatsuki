// SPDX-License-Identifier: MIT
// Original PAS/query implementation from the register/ELF facts in G0_GPU_SURVEY sections 9.21.23 / 9.21.26.
#ifdef EPI_GMU_HOST_TEST
#include "gmu_shim.hxx"
#else
#include "BDD.hxx"
#include "epi_hw.hxx"
#endif
#include "epi_zap.hxx"

// Permanent after INIT succeeds (or a monitor session cannot be completed). No PAS shutdown/unload is attempted.
static PVOID volatile ZapOwner=NULL;
static PVOID ZapRetainedMetadata=NULL;
#ifdef EPI_GMU_HOST_TEST
static BOOLEAN ZapBootClaim=FALSE;
#endif
// EpiDiag is volatile. Its child survives driver reload, but disappears at the next full boot.
static NTSTATUS ZapClaimBoot(HANDLE Diag,HANDLE* Claim)
{
#ifdef EPI_GMU_HOST_TEST
    (void)Diag;
    if (ZapBootClaim) { return STATUS_DEVICE_BUSY; }
    ZapBootClaim=TRUE; *Claim=reinterpret_cast<HANDLE>(1); return STATUS_SUCCESS;
#else
    UNICODE_STRING name; RtlInitUnicodeString(&name,L"ZapOnce"); OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa,&name,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,Diag,NULL);
    ULONG disposition=0;
    NTSTATUS st=ZwCreateKey(Claim,KEY_READ|KEY_WRITE|DELETE,&oa,0,NULL,REG_OPTION_VOLATILE,&disposition);
    if (!NT_SUCCESS(st)) { return st; }
    if (disposition!=REG_CREATED_NEW_KEY) { ZwClose(*Claim); *Claim=NULL; return STATUS_DEVICE_BUSY; }
    return ZwFlushKey(*Claim);
#endif
}
static NTSTATUS ZapReleaseBoot(HANDLE Claim,BOOLEAN Consumed)
{
    if (Claim==NULL) { return STATUS_SUCCESS; }
#ifdef EPI_GMU_HOST_TEST
    if (!Consumed) { ZapBootClaim=FALSE; }
    return STATUS_SUCCESS;
#else
    NTSTATUS st=Consumed ? STATUS_SUCCESS : ZwDeleteKey(Claim);
    ZwClose(Claim); return st;
#endif
}
static ULONG ZapWord(const UCHAR* P) { ULONG v; RtlCopyMemory(&v,P,4); return v; }
BOOLEAN EpiZapValidate(const UCHAR* Image,ULONG Length)
{
    if (Image==NULL || Length!=EPI_ZAP_FILE_SIZE) { return FALSE; }
    const UCHAR ident[]={0x7F,'E','L','F',1,1,1,0,0,0,0,0,0,0,0,0};
    for (ULONG i=0;i<16;++i) { if (Image[i]!=ident[i]) { return FALSE; } }
    // ELF32: EXEC, machine 164, version 1, entry 5000, PH at 34, no SH, flags 3.
    const ULONG header[]={0x00A40002,1,0x5000,0x34,0,3,0x00200034,3,0};
    for (ULONG i=0;i<9;++i) { if (ZapWord(Image+16+4*i)!=header[i]) { return FALSE; } }
    const ULONG ph[3][8]={
        {0,0,0,0,0x94,0,0x07000000,0},
        {0,0x1000,0x6000,0x6000,0x19A0,0x2000,0x02200000,0x1000},
        {1,0x3000,0x5000,0x5000,0x7B8,0x7B8,0x08000007,0x100000}
    };
    for (ULONG i=0;i<3;++i) for (ULONG j=0;j<8;++j)
    { if (ZapWord(Image+52+32*i+4*j)!=ph[i][j]) { return FALSE; } }
    return TRUE;
}
static NTSTATUS ZapPersist(HANDLE Diag,EPI_ZAP_TRACE* T)
{
    NTSTATUS st=EpiSetValue(Diag,L"GpuGmuZ1Pas",REG_BINARY,T,sizeof(*T));
    return NT_SUCCESS(st) ? ZwFlushKey(Diag) : st;
}
static NTSTATUS ZapPoint(HANDLE Diag,EPI_ZAP_TRACE* T,EPI_ZAP_CHECKPOINT Checkpoint,PVOID Context,ULONG Step)
{
    T->Step=Step;
    NTSTATUS st=ZapPersist(Diag,T);
    return NT_SUCCESS(st) ? Checkpoint(Context,Step) : st;
}
static NTSTATUS ZapInvoke(EPI_SMC_REGS* Registers)
{
#if defined(_M_ARM64) || defined(EPI_GMU_HOST_TEST)
    EpiSmcInvoke(Registers); return STATUS_SUCCESS;
#else
    (void)Registers; return STATUS_NOT_SUPPORTED;
#endif
}
static NTSTATUS ZapQuerySave(HANDLE Diag,EPI_ZAP_QUERY_TRACE* T)
{
    NTSTATUS st=EpiSetValue(Diag,L"GpuZapQuery",REG_BINARY,T,sizeof(*T));
    return NT_SUCCESS(st) ? ZwFlushKey(Diag) : st;
}
static VOID ZapQueryError(EPI_ZAP_QUERY_TRACE* T,NTSTATUS Error)
{
    if (NT_SUCCESS(static_cast<NTSTATUS>(T->Status))) { T->Status=static_cast<ULONG>(Error); }
}
NTSTATUS EpiZapQuery(HANDLE Params,HANDLE Diag,EPI_ZAP_QUERY_TRACE* T)
{
    PAGED_CODE();
    if (T==NULL) { return STATUS_INVALID_PARAMETER; }
    RtlFillMemory(T,sizeof(*T),0xFF); T->Count=0; T->Status=STATUS_SUCCESS;
    const ULONGLONG functions[]={0x42000601,0xC2000601,0x42000601,0x42000601,0x42000601,0x42000601,
                                0x42000207,0x42000207,0x42000207,0x42000207,0x42000207,0x42000207};
    const ULONGLONG args[]={0x02000601,0x02000601,0x02000207,0x02000201,0x02000202,0x02000205,1,18,6,4,9,13};
    for (ULONG i=0;i<12;++i)
    { T->Row[i].Function=functions[i]; T->Row[i].Arg=args[i]; T->Row[i].Calls=0; T->Row[i].Flags=0; }
    NTSTATUS st=ZapQuerySave(Diag,T);
    BOOLEAN stop=!NT_SUCCESS(st);
    if (stop) { ZapQueryError(T,st); }
    for (ULONG i=0;i<12 && !stop;++i)
    {
        EPI_ZAP_QUERY_ROW* q=&T->Row[i];
        ULONG interrupted=0,busy=0;
        EPI_SMC_REGS r={{q->Function,1,q->Arg,0,0,0,0,0}};
        for (;;)
        {
            st=EpiSetDword(Params,L"EpiGpuProbeStep",0x1600+i);
            if (NT_SUCCESS(st)) { st=ZwFlushKey(Params); }
            if (!NT_SUCCESS(st)) { ZapQueryError(T,st); stop=TRUE; break; }
            st=ZapInvoke(&r);
            if (!NT_SUCCESS(st)) { ZapQueryError(T,st); stop=TRUE; break; }
            ++q->Calls; T->Count=i+1;
            q->X0=r.X[0]; q->X1=r.X[1]; q->X2=r.X[2]; q->X3=r.X[3]; q->X6=r.X[6];
            const BOOLEAN resume=r.X[0]==1;
            const BOOLEAN retry=r.X[0]==static_cast<ULONGLONG>(-12LL);
            BOOLEAN limit=FALSE;
            if (resume)
            {
                q->Flags|=1;
                if (++interrupted==1024) { limit=TRUE; ZapQueryError(T,STATUS_IO_TIMEOUT); }
            }
            if (retry)
            {
                q->Flags|=2;
                if (busy==20) { limit=TRUE; ZapQueryError(T,STATUS_DEVICE_BUSY); }
            }
            if (limit) { q->Flags|=4; }
            // Persist every returned SMC, including intermediate INTERRUPTED/EBUSY and the final limit result.
            st=ZapQuerySave(Diag,T);
            if (!NT_SUCCESS(st)) { ZapQueryError(T,st); stop=TRUE; break; }
            if (limit || (!resume && !retry)) { break; }
            if (resume)
            {
                r.X[0]=1; r.X[1]=1; r.X[2]=q->Arg; r.X[3]=0; r.X[4]=0; r.X[5]=0; r.X[7]=0;
                continue; // x6 is the monitor's returned session ID.
            }
            ++busy;
#ifdef EPI_GMU_HOST_TEST
            KeStallExecutionProcessor(30000);
#else
            LARGE_INTEGER delay; delay.QuadPart=-300000;
            st=KeDelayExecutionThread(KernelMode,FALSE,&delay);
            if (!NT_SUCCESS(st)) { ZapQueryError(T,st); break; }
#endif
            r={{q->Function,1,q->Arg,0,0,0,0,0}}; // EBUSY restarts the original std/fast call with x6=0.
        }
    }
    st=EpiSetDword(Params,L"EpiGpuProbeStep",0xFFFF);
    if (NT_SUCCESS(st)) { st=ZwFlushKey(Params); }
    if (!NT_SUCCESS(st)) { ZapQueryError(T,st); }
    st=ZapQuerySave(Diag,T);
    if (!NT_SUCCESS(st)) { ZapQueryError(T,st); }
    return static_cast<NTSTATUS>(T->Status);
}
static NTSTATUS ZapCall(HANDLE Diag,EPI_ZAP_TRACE* T,EPI_ZAP_CHECKPOINT Checkpoint,PVOID Context,
                        ULONG Index,ULONG Cmd,ULONG Info,ULONGLONG Arg1,ULONGLONG Arg2,ULONG Expected)
{
    EPI_ZAP_CALL* C=&T->Call[Index];
    C->Function=0x42000200ULL|Cmd; C->ArgInfo=Info; C->Args[0]=13; C->Args[1]=Arg1; C->Args[2]=Arg2;
    C->Calls=0; C->Interrupted=0; C->Busy=0; C->Completed=0;
    for (;;)
    {
        EPI_SMC_REGS r={{C->Function,C->ArgInfo,13,Arg1,Arg2,0,0,0}};
        for (;;)
        {
            NTSTATUS st=ZapPoint(Diag,T,Checkpoint,Context,0x1420+Index*4);
            if (!NT_SUCCESS(st)) { return st; }
            ++C->Calls;
            st=ZapInvoke(&r); if (!NT_SUCCESS(st)) { C->Completed=1; return st; }
            for (ULONG i=0;i<4;++i) { C->Result[i]=r.X[i]; }
            C->Session=r.X[6];
            C->Completed=(r.X[0]==0 || (r.X[0]>>63)!=0) ? 1UL : 0UL;
            if (r.X[0]!=1) { break; }
            ++C->Interrupted;
            if (C->Interrupted==1024) { return STATUS_IO_TIMEOUT; }
            // Keep args and the returned session ID, but resume with x0=1.
            r.X[0]=1; r.X[1]=Info; r.X[2]=13; r.X[3]=Arg1; r.X[4]=Arg2; r.X[5]=0; r.X[7]=0;
        }
        if (r.X[0]==static_cast<ULONGLONG>(-12LL))
        {
            if (C->Busy==20) { return STATUS_DEVICE_BUSY; }
            ++C->Busy;
            NTSTATUS st=ZapPoint(Diag,T,Checkpoint,Context,0x1421+Index*4);
            if (!NT_SUCCESS(st)) { return st; }
#ifdef EPI_GMU_HOST_TEST
            KeStallExecutionProcessor(30000);
#else
            LARGE_INTEGER delay; delay.QuadPart=-300000; // 30 ms at PASSIVE_LEVEL, not a busy wait.
            st=KeDelayExecutionThread(KernelMode,FALSE,&delay);
            if (!NT_SUCCESS(st)) { return st; }
#endif
            continue;
        }
        // WAITQ_SLEEP and all unexpected positive results stop here; never invoke WAITQ services.
        C->Completed=(r.X[0]==0 || (r.X[0]>>63)!=0) ? 1UL : 0UL;
        if (r.X[0]!=0 || (Index!=0 && r.X[1]!=Expected)) { return STATUS_IO_DEVICE_ERROR; }
        if (Index==0) { T->SupportX1Ignored=1; } // IS_SUPPORTED is advisory in Z-1c; retain its raw x1.
        if (Index==1) { T->InitSucceeded=1; T->Consumed=1; }
        return ZapPoint(Diag,T,Checkpoint,Context,0x1422+Index*4);
    }
}
NTSTATUS EpiZapLoad(HANDLE Params,HANDLE Diag,const UCHAR* Image,ULONG Length,EPI_ZAP_TRACE* T,
                    EPI_ZAP_CHECKPOINT Checkpoint,PVOID Context)
{
    PAGED_CODE(); (void)Params;
    if (T==NULL || Checkpoint==NULL) { return STATUS_INVALID_PARAMETER; }
    RtlFillMemory(T,sizeof(*T),0xFF); T->Step=0x1400; T->FileSize=Length;
    T->SupportX1Ignored=0;
    T->InitSucceeded=0; T->Consumed=0; T->MetadataKept=0; T->MetadataBytes=0x1A34; T->TargetBytes=0x1000;
    T->Status=STATUS_NOT_FOUND;
    if (!EpiZapValidate(Image,Length)) { T->Status=STATUS_INVALID_PARAMETER; ZapPersist(Diag,T); return T->Status; }
    // Store the 13 decoded ELF header fields, including all seven 16-bit fields.
    const ULONG offsets[]={16,18,20,24,28,32,36,40,42,44,46,48,50};
    for (ULONG i=0;i<13;++i)
    {
        const ULONG o=offsets[i];
        T->Header[i]=(i<2 || i>=7) ? static_cast<ULONG>(Image[o])|(static_cast<ULONG>(Image[o+1])<<8) : ZapWord(Image+o);
    }
    RtlCopyMemory(T->Phdr,Image+52,sizeof(T->Phdr));
    if (InterlockedCompareExchangePointer(&ZapOwner,reinterpret_cast<PVOID>(1),NULL)!=NULL)
    { T->Consumed=1; T->Status=STATUS_DEVICE_BUSY; ZapPersist(Diag,T); return T->Status; }
    PVOID metadata=NULL, target=NULL; HANDLE claim=NULL;
    NTSTATUS st=ZapPoint(Diag,T,Checkpoint,Context,0x1400);
    if (NT_SUCCESS(st))
    {
        st=ZapPoint(Diag,T,Checkpoint,Context,0x1405);
        if (NT_SUCCESS(st))
        {
            st=ZapClaimBoot(Diag,&claim);
            if (st==STATUS_DEVICE_BUSY) { T->Consumed=1; }
        }
    }
    if (NT_SUCCESS(st)) { st=ZapCall(Diag,T,Checkpoint,Context,0,7,1,0,0,1); }
    if (NT_SUCCESS(st))
    {
        st=ZapPoint(Diag,T,Checkpoint,Context,0x1401);
        if (NT_SUCCESS(st))
        {
            PHYSICAL_ADDRESS low,high,boundary; low.QuadPart=0; high.QuadPart=0xFFFFFFFFFLL; boundary.QuadPart=0;
            metadata=MmAllocateContiguousMemorySpecifyCache(0x2000,low,high,boundary,MmNonCached);
            if (metadata==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
            else
            {
                T->MetadataPa=MmGetPhysicalAddress(metadata).QuadPart;
                if (T->MetadataPa&0xFFF) { st=STATUS_INVALID_PARAMETER; }
                else
                {
                    RtlZeroMemory(metadata,0x2000); RtlCopyMemory(metadata,Image,0x94);
                    RtlCopyMemory(static_cast<UCHAR*>(metadata)+0x94,Image+0x1000,0x19A0); KeMemoryBarrier();
                }
            }
        }
    }
    if (NT_SUCCESS(st)) { st=ZapCall(Diag,T,Checkpoint,Context,1,1,0x82,T->MetadataPa,0,0); }
    // Check raw results even if persisting the successful return failed. INIT must never be repeated.
    if (T->Call[1].Calls!=0xFFFFFFFF && T->Call[1].Calls!=0 && T->Call[1].Result[0]==0 && T->Call[1].Result[1]==0)
    { T->InitSucceeded=1; T->Consumed=1; }
    if (NT_SUCCESS(st)) { st=ZapCall(Diag,T,Checkpoint,Context,2,2,3,0x8B110000,0x1000,0); }
    if (NT_SUCCESS(st))
    {
        st=ZapPoint(Diag,T,Checkpoint,Context,0x1402);
        if (NT_SUCCESS(st))
        {
            PHYSICAL_ADDRESS pa; pa.QuadPart=0x8B110000;
            target=MmMapIoSpaceEx(pa,0x1000,PAGE_READWRITE|PAGE_NOCACHE);
            if (target==NULL) { st=STATUS_INSUFFICIENT_RESOURCES; }
        }
        if (NT_SUCCESS(st))
        {
            // Last durable marker before the first potentially hyp-protected store at 8B110000.
            st=ZapPoint(Diag,T,Checkpoint,Context,0x1403);
            if (NT_SUCCESS(st))
            {
                // Use aligned MMIO stores rather than a memcpy implementation that may use unaligned SIMD.
                volatile ULONG* dst=static_cast<volatile ULONG*>(target);
                for (ULONG i=0;i<0x1000/4;++i)
                { WRITE_REGISTER_ULONG(dst+i,i<0x7B8/4 ? ZapWord(Image+0x3000+i*4) : 0); }
                KeMemoryBarrier();
                for (ULONG i=0;i<4;++i) { T->TargetReadback[i]=READ_REGISTER_ULONG(static_cast<volatile ULONG*>(target)+i); }
                for (ULONG i=0;i<4;++i) if (T->TargetReadback[i]!=ZapWord(Image+0x3000+i*4)) { st=STATUS_IO_DEVICE_ERROR; }
            }
        }
        if (target!=NULL) { MmUnmapIoSpace(target,0x1000); target=NULL; }
        if (NT_SUCCESS(st)) { st=ZapPoint(Diag,T,Checkpoint,Context,0x1404); }
    }
    if (NT_SUCCESS(st)) { st=ZapCall(Diag,T,Checkpoint,Context,3,5,1,0,0,0); }
    // A positive monitor status may leave a session referring to metadata. Quarantine rather than free it.
    for (ULONG i=0;i<4;++i) if (T->Call[i].Calls!=0xFFFFFFFF && T->Call[i].Calls!=0 && T->Call[i].Completed==0)
    {
        T->Consumed=1;
        if (metadata!=NULL) { ZapRetainedMetadata=metadata; metadata=NULL; T->MetadataKept=1; }
    }
    if (metadata!=NULL) { MmFreeContiguousMemorySpecifyCache(metadata,0x2000,MmNonCached); }
    const NTSTATUS released=ZapReleaseBoot(claim,T->Consumed!=0);
    if (!NT_SUCCESS(released)) { T->Consumed=1; if (NT_SUCCESS(st)) { st=released; } }
    if (!T->Consumed) { InterlockedExchangePointer(&ZapOwner,NULL); }
    // On consumed paths keep ZapOwner non-NULL even after driver teardown.
    T->Status=st;
    const NTSTATUS saved=ZapPersist(Diag,T);
    if (NT_SUCCESS(st) && !NT_SUCCESS(saved)) { T->Status=saved; }
    return T->Status;
}
