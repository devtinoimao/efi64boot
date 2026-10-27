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

/*
 * The 32-bit firmware's EFI_BOOT_SERVICES table has 4-byte pointers
 * (this is IA32 UEFI).  efi32.h's struct is compiled into BOTH the 32-bit
 * main.c and the 64-bit fake64.c, so its member offsets are 8 bytes in
 * fake64.c — wrong for indexing the real 32-bit table.  Use explicit
 * byte offsets into the table instead of the struct members.
 */
#define BS32_RAISETPL          24
#define BS32_RESTORETPL        28
#define BS32_ALLOCATEPAGES     32
#define BS32_FREEPAGES         36
#define BS32_GETMEMORYMAP      40
#define BS32_ALLOCATEPOOL      44
#define BS32_FREEPOOL          48
#define BS32_CREATEEVENT       52
#define BS32_SETTIMER          56
#define BS32_WAITFOREVENT      60
#define BS32_SIGNALEVENT       64
#define BS32_CLOSEEVENT        68
#define BS32_CHECKEVENT        72
#define BS32_INSTALLPROTO      76
#define BS32_REINSTALLPROTO    80
#define BS32_UNINSTALLPROTO    84
#define BS32_HANDLEPROTOCOL    88
#define BS32_RESERVED          92
#define BS32_REGPROTONOTIFY    96
#define BS32_LOCATEHANDLE      100
#define BS32_LOCATEDEVICEPATH  104
#define BS32_INSTALLCONFIGTBL  108
#define BS32_LOADIMAGE         112
#define BS32_STARTIMAGE        116
#define BS32_EXIT              120
#define BS32_UNLOADIMAGE       124
#define BS32_EXITBOOTSERVICES  128
#define BS32_GETNEXTMONOTONIC  132
#define BS32_STALL             136
#define BS32_WATCHDOG          140
#define BS32_LOCATEPROTOCOL    144

static inline uint32_t bs32_get(uint32_t off)
{
    return *(const uint32_t *)((const uint8_t *)bs32 + off);
}

uint32_t fake64_bs32_get(uint32_t off)
{
    return bs32_get(off);
}

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
    ADD_SVC(bs32_get(BS32_RAISETPL));            /* SVC_RAISETPL */
    ADD_SVC(bs32_get(BS32_RESTORETPL));          /* SVC_RESTORETPL */
    ADD_SVC(bs32_get(BS32_ALLOCATEPAGES));       /* SVC_ALLOCATEPAGES */
    ADD_SVC(bs32_get(BS32_FREEPAGES));
    ADD_SVC(bs32_get(BS32_GETMEMORYMAP));
    ADD_SVC(bs32_get(BS32_ALLOCATEPOOL));
    ADD_SVC(bs32_get(BS32_FREEPOOL));
    ADD_SVC(bs32_get(BS32_CREATEEVENT));
    ADD_SVC(bs32_get(BS32_SETTIMER));
    ADD_SVC(bs32_get(BS32_WAITFOREVENT));
    ADD_SVC(bs32_get(BS32_SIGNALEVENT));
    ADD_SVC(bs32_get(BS32_CLOSEEVENT));
    ADD_SVC(bs32_get(BS32_CHECKEVENT));
    ADD_SVC(bs32_get(BS32_INSTALLPROTO));
    ADD_SVC(bs32_get(BS32_REINSTALLPROTO));
    ADD_SVC(bs32_get(BS32_UNINSTALLPROTO));
    ADD_SVC(bs32_get(BS32_HANDLEPROTOCOL));
    ADD_SVC(bs32_get(BS32_RESERVED));
    ADD_SVC(bs32_get(BS32_REGPROTONOTIFY));
    ADD_SVC(bs32_get(BS32_LOCATEHANDLE));
    ADD_SVC(bs32_get(BS32_LOCATEDEVICEPATH));
    ADD_SVC(bs32_get(BS32_INSTALLCONFIGTBL));
    ADD_SVC(bs32_get(BS32_LOADIMAGE));
    ADD_SVC(bs32_get(BS32_STARTIMAGE));
    ADD_SVC(bs32_get(BS32_EXIT));
    ADD_SVC(bs32_get(BS32_UNLOADIMAGE));
    ADD_SVC(bs32_get(BS32_EXITBOOTSERVICES));
    ADD_SVC(bs32_get(BS32_GETNEXTMONOTONIC));
    ADD_SVC(bs32_get(BS32_STALL));
    ADD_SVC(bs32_get(BS32_WATCHDOG));
    ADD_SVC(bs32_get(BS32_LOCATEPROTOCOL));
}
