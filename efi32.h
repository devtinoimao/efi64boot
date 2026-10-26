/* Minimal IA32 UEFI definitions for efi64boot (only what M1 uses). */
#ifndef EFI32_H
#define EFI32_H

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef intptr_t  INTN;
typedef void *EFI_HANDLE;
typedef UINT64 EFI_STATUS;
typedef UINT64 EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS     ((EFI_STATUS)0)
#define EFI_UNSUPPORTED ((EFI_STATUS)0x8000000000000003ULL)

typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;
typedef enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData,
               EfiBootServicesCode, EfiBootServicesData,
               EfiRuntimeServicesCode, EfiRuntimeServicesData } EFI_MEMORY_TYPE;

typedef EFI_STATUS (*EFI_ALLOCATE_PAGES)(EFI_ALLOCATE_TYPE Type,
                                         EFI_MEMORY_TYPE MemoryType,
                                         UINTN Pages,
                                         EFI_PHYSICAL_ADDRESS *Memory);

/* Field order per UEFI spec; only the head is declared (we stop after
 * the members we call — the rest of the table exists, we just don't
 * name it). */
typedef struct {
    UINT64 Hdr_dummy[3];        /* EFI_TABLE_HEADER: signature + rev + CRC */
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages;
    void *FreePages;
    void *GetMemoryMap;
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    void *HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;
    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    void *LocateProtocol;
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
    /* spec tail continues but we stop here */
} EFI_BOOT_SERVICES;

typedef struct {
    UINT64 Signature_dummy[3];  /* signature + table header (24 bytes) */
    UINTN  FirmwareVendor;      /* CHAR16* */
    UINT32 FirmwareRevision;
    UINTN  pad_console[4];      /* ConsoleInHandle, ConIn, ConsoleOutHandle, ConOut */
    UINTN  pad_stderr[2];       /* StandardErrorHandle, StdErr */
    UINTN  RuntimeServices;     /* EFI_RUNTIME_SERVICES* */
    EFI_BOOT_SERVICES *BootServices;
    UINTN  NumberOfTableEntries;
    UINTN  ConfigurationTable;
} EFI_SYSTEM_TABLE;

#define EFI_ERROR(s) (((INTN)(s)) < 0)

#endif
