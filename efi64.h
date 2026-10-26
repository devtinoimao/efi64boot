/* Minimal 64-bit (X64) UEFI definitions for the fake system table. */
#ifndef EFI64_H
#define EFI64_H

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef intptr_t  INTN;
typedef void *EFI_HANDLE;
typedef UINT64 EFI_STATUS;

typedef EFI_STATUS (*EFI_THUNK_FN)(void);

/* X64 calling convention function pointers in the fake tables */
typedef EFI_STATUS (*EFI64_FN)();

#pragma pack(1)
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HDR64;

typedef struct {
    EFI_TABLE_HDR64 Hdr;
    UINT64 RaiseTPL;
    UINT64 RestoreTPL;
    UINT64 AllocatePages;
    UINT64 FreePages;
    UINT64 GetMemoryMap;
    UINT64 AllocatePool;
    UINT64 FreePool;
    UINT64 CreateEvent;
    UINT64 SetTimer;
    UINT64 WaitForEvent;
    UINT64 SignalEvent;
    UINT64 CloseEvent;
    UINT64 CheckEvent;
    UINT64 InstallProtocolInterface;
    UINT64 ReinstallProtocolInterface;
    UINT64 UninstallProtocolInterface;
    UINT64 HandleProtocol;
    UINT64 Reserved;
    UINT64 RegisterProtocolNotify;
    UINT64 LocateHandle;
    UINT64 LocateDevicePath;
    UINT64 InstallConfigurationTable;
    UINT64 LoadImage;
    UINT64 StartImage;
    UINT64 Exit;
    UINT64 UnloadImage;
    UINT64 ExitBootServices;
    UINT64 GetNextMonotonicCount;
    UINT64 Stall;
    UINT64 SetWatchdogTimer;
    UINT64 ConnectController;
    UINT64 DisconnectController;
    UINT64 OpenProtocol;
    UINT64 CloseProtocol;
    UINT64 OpenProtocolInformation;
    UINT64 ProtocolsPerHandle;
    UINT64 LocateHandleBuffer;
    UINT64 LocateProtocol;
    UINT64 InstallMultipleProtocolInterfaces;
    UINT64 UninstallMultipleProtocolInterfaces;
    UINT64 CalculateCrc32;
    UINT64 CopyMem;
    UINT64 SetMem;
    UINT64 CreateEventEx;
} EFI_BOOT_SERVICES_64;

typedef struct {
    EFI_TABLE_HDR64 Hdr;
    UINT64 GetTime;
    UINT64 SetTime;
    UINT64 GetWakeupTime;
    UINT64 SetWakeupTime;
    UINT64 SetVirtualAddressMap;
    UINT64 ConvertPointer;
    UINT64 GetVariable;
    UINT64 GetNextVariableName;
    UINT64 SetVariable;
    UINT64 GetNextHighMonotonicCount;
    UINT64 ResetSystem;
    UINT64 UpdateCapsule;
    UINT64 QueryCapsuleCapabilities;
    UINT64 QueryVariableInfo;
} EFI_RUNTIME_SERVICES_64;

typedef struct {
    UINT64 fwd_link;
    EFI_TABLE_HDR64 Hdr;
    UINT64 FirmwareVendor;
    UINT32 FirmwareRevision;
    UINT32 _pad;
    UINT64 ConsoleInHandle;
    UINT64 ConIn;
    UINT64 ConsoleOutHandle;
    UINT64 ConOut;
    UINT64 StandardErrorHandle;
    UINT64 StdErr;
    UINT64 RuntimeServices;
    UINT64 BootServices;
    UINT64 NumberOfTableEntries;
    UINT64 ConfigurationTable;
} EFI_SYSTEM_TABLE_64;
#pragma pack()

#define EFI64_SIGNATURE 0x5453595320494249ULL  /* "IBI SYST" */

#endif
