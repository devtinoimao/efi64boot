/*
 * efi64boot M2 — fake 64-bit EFI environment (core services only).
 *
 * main.c captures the 32-bit firmware context into SAVED32 before the
 * transition. The 64-bit world rebuilds a 64-bit EFI_SYSTEM_TABLE whose
 * core boot-services entries are thunks: each drops to 32-bit, calls the
 * real firmware service (svc_table32[i]), and returns.
 *
 * NOTE (M3): protocol INTERFACES handed to the x64 app (ConOut, FS) still
 * contain 32-bit pointers — handled in the next iteration via runtime
 * stub generation. Core boot services work end-to-end here.
 */

#include <stdint.h>
#include "efi64.h"
#include "bridge.h"
#include "efi32.h"

/* 32-bit state saved by main.c pre-transition */
extern unsigned char saved_gdtr32[10];
extern unsigned char saved_idtr32[10];
uint32_t saved_cr0_32, saved_cr3_32, saved_cr4_32;
uint32_t saved_ds32, saved_es32, saved_fs32, saved_gs32;
uint32_t saved_ss32, saved_cs32;
uint32_t thunk_stack32_top;

/* runtime table of 32-bit function pointers, indexed by thunk */
uint32_t svc_table32[64];
uint32_t svc_count;

/* from thunk64.asm */
extern uint64_t do_thunk_call(uint64_t *args, uint64_t *retout,
                              uint32_t svc_index, uint32_t nargs);

static EFI_BOOT_SERVICES *bs32;

void fake64_init(void *BootServices32)
{
    bs32 = (EFI_BOOT_SERVICES *)BootServices32;
    svc_count = 0;
}

#define ADD_SVC(fn) (svc_table32[svc_count++] = (uint32_t)(uintptr_t)(fn))

/* Thunk generator: N-arg 64-bit wrapper -> bridge -> 32-bit call */
#define THUNK2(name, n, idx_expr)                                  \
    static uint64_t name(uint64_t a0, uint64_t a1)                 \
    { uint64_t v[8] = {a0,a1}; uint64_t r;                         \
      do_thunk_call(v, &r, (idx_expr), (n)); return r; }

#define THUNK4(name, n, idx_expr)                                  \
    static uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2,    \
                         uint64_t a3)                              \
    { uint64_t v[8] = {a0,a1,a2,a3}; uint64_t r;                   \
      do_thunk_call(v, &r, (idx_expr), (n)); return r; }

/* Core boot services we wrap for M2 (indices assigned in order added) */
static uint64_t th_RaiseTPL(uint64_t a)            { uint64_t v[8]={a}; uint64_t r; do_thunk_call(v,&r,SVC_RAISETPL,1); return r; }
static uint64_t th_RestoreTPL(uint64_t a)          { uint64_t v[8]={a}; uint64_t r; do_thunk_call(v,&r,SVC_RESTORETPL,1); return r; }
static uint64_t th_AllocatePages(uint64_t t, uint64_t mt, uint64_t n, uint64_t mem)
        { uint64_t v[8]={t,mt,n,mem}; uint64_t r; do_thunk_call(v,&r,SVC_ALLOCATEPAGES,4); return r; }
static uint64_t th_FreePages(uint64_t mem, uint64_t n)
        { uint64_t v[8]={mem,n}; uint64_t r; do_thunk_call(v,&r,SVC_FREEPAGES,2); return r; }
static uint64_t th_GetMemoryMap(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
        { uint64_t v[8]={a,b,c,d,e}; uint64_t r; do_thunk_call(v,&r,SVC_GETMEMORYMAP,5); return r; }
static uint64_t th_AllocatePool(uint64_t t, uint64_t sz, uint64_t buf)
        { uint64_t v[8]={t,sz,buf}; uint64_t r; do_thunk_call(v,&r,SVC_ALLOCATEPOOL,3); return r; }
static uint64_t th_FreePool(uint64_t buf)
        { uint64_t v[8]={buf}; uint64_t r; do_thunk_call(v,&r,SVC_FREEPOOL,1); return r; }
static uint64_t th_CreateEvent(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
        { uint64_t v[8]={a,b,c,d,e}; uint64_t r; do_thunk_call(v,&r,SVC_CREATEEVENT,5); return r; }
static uint64_t th_SetTimer(uint64_t ev, uint64_t t, uint64_t tt)
        { uint64_t v[8]={ev,t,tt}; uint64_t r; do_thunk_call(v,&r,SVC_SETTIMER,3); return r; }
static uint64_t th_WaitForEvent(uint64_t n, uint64_t ev, uint64_t idx)
        { uint64_t v[8]={n,ev,idx}; uint64_t r; do_thunk_call(v,&r,SVC_WAITFOREVENT,3); return r; }
static uint64_t th_SignalEvent(uint64_t ev)
        { uint64_t v[8]={ev}; uint64_t r; do_thunk_call(v,&r,SVC_SIGNALEVENT,1); return r; }
static uint64_t th_CloseEvent(uint64_t ev)
        { uint64_t v[8]={ev}; uint64_t r; do_thunk_call(v,&r,SVC_CLOSEEVENT,1); return r; }
static uint64_t th_CheckEvent(uint64_t ev)
        { uint64_t v[8]={ev}; uint64_t r; do_thunk_call(v,&r,SVC_CHECKEVENT,1); return r; }
static uint64_t th_InstallProtocolInterface(uint64_t h, uint64_t p, uint64_t it, uint64_t ifc)
        { uint64_t v[8]={h,p,it,ifc}; uint64_t r; do_thunk_call(v,&r,SVC_INSTALLPROTOCOL,4); return r; }
static uint64_t th_HandleProtocol(uint64_t h, uint64_t p, uint64_t out)
        { uint64_t v[8]={h,p,out}; uint64_t r; do_thunk_call(v,&r,SVC_HANDLEPROTOCOL,3); return r; }
static uint64_t th_RegisterProtocolNotify(uint64_t p, uint64_t ev, uint64_t reg)
        { uint64_t v[8]={p,ev,reg}; uint64_t r; do_thunk_call(v,&r,SVC_REGPROTONOTIFY,3); return r; }
static uint64_t th_LocateHandle(uint64_t search, uint64_t p, uint64_t key, uint64_t sz, uint64_t buf)
        { uint64_t v[8]={search,p,key,sz,buf}; uint64_t r; do_thunk_call(v,&r,SVC_LOCATEHANDLE,5); return r; }
static uint64_t th_LocateDevicePath(uint64_t p, uint64_t dp, uint64_t dev)
        { uint64_t v[8]={p,dp,dev}; uint64_t r; do_thunk_call(v,&r,SVC_LOCATEDEVICEPATH,3); return r; }
static uint64_t th_LoadImage(uint64_t boot, uint64_t parent, uint64_t dp, uint64_t src, uint64_t sz, uint64_t out)
        { uint64_t v[8]={boot,parent,dp,src,sz,out}; uint64_t r; do_thunk_call(v,&r,SVC_LOADIMAGE,6); return r; }
static uint64_t th_StartImage(uint64_t h, uint64_t exitdata)
        { uint64_t v[8]={h,exitdata}; uint64_t r; do_thunk_call(v,&r,SVC_STARTIMAGE,2); return r; }
static uint64_t th_Exit(uint64_t h, uint64_t st, uint64_t sz, uint64_t data)
        { uint64_t v[8]={h,st,sz,data}; uint64_t r; do_thunk_call(v,&r,SVC_EXIT,4); return r; }
static uint64_t th_ExitBootServices(uint64_t h, uint64_t key)
        { uint64_t v[8]={h,key}; uint64_t r; do_thunk_call(v,&r,SVC_EXITBOOTSERVICES,2); return r; }
static uint64_t th_Stall(uint64_t us)
        { uint64_t v[8]={us}; uint64_t r; do_thunk_call(v,&r,SVC_STALL,1); return r; }
static uint64_t th_SetWatchdogTimer(uint64_t t, uint64_t w, uint64_t dl, uint64_t d)
        { uint64_t v[8]={t,w,dl,d}; uint64_t r; do_thunk_call(v,&r,SVC_WATCHDOG,4); return r; }
static uint64_t th_LocateProtocol(uint64_t p, uint64_t reg, uint64_t out)
        { uint64_t v[8]={p,reg,out}; uint64_t r; do_thunk_call(v,&r,SVC_LOCATEPROTOCOL,3); return r; }

/* Fill svc_table32 with the 32-bit entry points in SVC_* index order. */
void fake64_bind_services(void)
{
    svc_count = 0;
    ADD_SVC(bs32->RaiseTPL);            /* SVC_RAISETPL */
    ADD_SVC(bs32->RestoreTPL);          /* SVC_RESTORETPL */
    ADD_SVC(bs32->AllocatePages);       /* SVC_ALLOCATEPAGES */
    ADD_SVC(bs32->FreePages);
    ADD_SVC(bs32->GetMemoryMap);
    ADD_SVC(bs32->AllocatePool);
    ADD_SVC(bs32->FreePool);
    ADD_SVC(bs32->CreateEvent);
    ADD_SVC(bs32->SetTimer);
    ADD_SVC(bs32->WaitForEvent);
    ADD_SVC(bs32->SignalEvent);
    ADD_SVC(bs32->CloseEvent);
    ADD_SVC(bs32->CheckEvent);
    ADD_SVC(bs32->InstallProtocolInterface);
    ADD_SVC(bs32->ReinstallProtocolInterface);
    ADD_SVC(bs32->UninstallProtocolInterface);
    ADD_SVC(bs32->HandleProtocol);
    ADD_SVC(bs32->Reserved);
    ADD_SVC(bs32->RegisterProtocolNotify);
    ADD_SVC(bs32->LocateHandle);
    ADD_SVC(bs32->LocateDevicePath);
    ADD_SVC(bs32->InstallConfigurationTable);
    ADD_SVC(bs32->LoadImage);
    ADD_SVC(bs32->StartImage);
    ADD_SVC(bs32->Exit);
    ADD_SVC(bs32->UnloadImage);
    ADD_SVC(bs32->ExitBootServices);
    ADD_SVC(bs32->GetNextMonotonicCount);
    ADD_SVC(bs32->Stall);
    ADD_SVC(bs32->SetWatchdogTimer);
    ADD_SVC(bs32->LocateProtocol);
}
