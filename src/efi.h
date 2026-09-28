/*
 * efi.h - the minimal slice of the UEFI 2.x ABI that QRT uses.
 *
 * Written from the UEFI specification; no EDK2/gnu-efi dependency.
 * Compiled with a *-windows target, so the default calling convention
 * already matches EFIAPI on both IA32 (cdecl) and X64 (ms_abi), and
 * UINT64 fields get the natural 8-byte alignment that firmware expects.
 */
#pragma once

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        i8;
typedef short              i16;
typedef int                i32;
typedef long long          i64;
typedef __SIZE_TYPE__      usize;
typedef __INTPTR_TYPE__    isize;
typedef u16                c16;   /* UCS-2 code unit */

typedef usize  EFI_STATUS;
typedef void  *EFI_HANDLE;
typedef void  *EFI_EVENT;
typedef u8     BOOLEAN;
typedef usize  UINTN;

#define NULL ((void *)0)
#define EFI_ERR_BIT ((usize)1 << (sizeof(usize) * 8 - 1))
#define EFI_ERROR(s) (((isize)(s)) < 0)
#define EFI_SUCCESS          0
#define EFI_LOAD_ERROR       (EFI_ERR_BIT | 1)
#define EFI_UNSUPPORTED      (EFI_ERR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL (EFI_ERR_BIT | 5)
#define EFI_NOT_READY        (EFI_ERR_BIT | 6)
#define EFI_NOT_FOUND        (EFI_ERR_BIT | 14)

typedef struct { u32 a; u16 b; u16 c; u8 d[8]; } EFI_GUID;

typedef struct {
    u64 Signature;
    u32 Revision;
    u32 HeaderSize;
    u32 CRC32;
    u32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    u16 Year; u8 Month; u8 Day; u8 Hour; u8 Minute; u8 Second; u8 Pad1;
    u32 Nanosecond; i16 TimeZone; u8 Daylight; u8 Pad2;
} EFI_TIME;

/* ---- Console ---------------------------------------------------------- */
typedef struct { u16 ScanCode; c16 UnicodeChar; } EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_TEXT_INPUT *, BOOLEAN);
    EFI_STATUS (*ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT *, EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT;

typedef struct EFI_SIMPLE_TEXT_OUTPUT {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_TEXT_OUTPUT *, BOOLEAN);
    EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT *, const c16 *);
    void *TestString, *QueryMode, *SetMode;
    EFI_STATUS (*SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT *, UINTN);
    EFI_STATUS (*ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT *);
    void *SetCursorPosition;
    EFI_STATUS (*EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT *, BOOLEAN);
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT;

#define SCAN_UP     0x01
#define SCAN_DOWN   0x02
#define SCAN_RIGHT  0x03
#define SCAN_LEFT   0x04
#define SCAN_HOME   0x05
#define SCAN_END    0x06
#define SCAN_PGUP   0x09
#define SCAN_PGDN   0x0a
#define SCAN_F1     0x0b
#define SCAN_ESC    0x17
#define SCAN_VOLUP  0x80    /* UEFI: volume up */
#define SCAN_VOLDN  0x81    /* UEFI: volume down */
#define SCAN_POWER  0x102   /* UEFI "suspend"; QRT: the power button */
#define SCAN_HOMEBTN 0x8001 /* QRT: the Windows/home button */

/* ---- Graphics Output Protocol ---------------------------------------- */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    u32 Version;
    u32 HorizontalResolution;
    u32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    u32 PixelMask[4];
    u32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    u32 MaxMode;
    u32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    u64 FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_MODE;

typedef enum {
    EfiBltVideoFill, EfiBltVideoToBltBuffer, EfiBltBufferToVideo, EfiBltVideoToVideo,
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, u32, UINTN *,
                            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (*SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, u32);
    EFI_STATUS (*Blt)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, u32 *BltBuffer,
                      EFI_GRAPHICS_OUTPUT_BLT_OPERATION, UINTN SrcX, UINTN SrcY,
                      UINTN DstX, UINTN DstY, UINTN Width, UINTN Height, UINTN Delta);
    EFI_GRAPHICS_OUTPUT_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- Pointers (mouse / touch) ---------------------------------------- */
typedef struct {
    i32 RelativeMovementX, RelativeMovementY, RelativeMovementZ;
    BOOLEAN LeftButton, RightButton;
} EFI_SIMPLE_POINTER_STATE;

typedef struct {
    u64 ResolutionX, ResolutionY, ResolutionZ;
    BOOLEAN LeftButton, RightButton;
} EFI_SIMPLE_POINTER_MODE;

typedef struct EFI_SIMPLE_POINTER_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_POINTER_PROTOCOL *, BOOLEAN);
    EFI_STATUS (*GetState)(struct EFI_SIMPLE_POINTER_PROTOCOL *, EFI_SIMPLE_POINTER_STATE *);
    EFI_EVENT WaitForInput;
    EFI_SIMPLE_POINTER_MODE *Mode;
} EFI_SIMPLE_POINTER_PROTOCOL;

typedef struct {
    u64 AbsoluteMinX, AbsoluteMinY, AbsoluteMinZ;
    u64 AbsoluteMaxX, AbsoluteMaxY, AbsoluteMaxZ;
    u32 Attributes;
} EFI_ABSOLUTE_POINTER_MODE;

typedef struct {
    u64 CurrentX, CurrentY, CurrentZ;
    u32 ActiveButtons;
} EFI_ABSOLUTE_POINTER_STATE;

typedef struct EFI_ABSOLUTE_POINTER_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *, BOOLEAN);
    EFI_STATUS (*GetState)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *, EFI_ABSOLUTE_POINTER_STATE *);
    EFI_EVENT WaitForInput;
    EFI_ABSOLUTE_POINTER_MODE *Mode;
} EFI_ABSOLUTE_POINTER_PROTOCOL;

/* ---- Storage ----------------------------------------------------------- */
typedef struct {
    u32 MediaId;
    BOOLEAN RemovableMedia, MediaPresent, LogicalPartition, ReadOnly, WriteCaching;
    u32 BlockSize;
    u32 IoAlign;
    u64 LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL {
    u64 Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    void *Reset, *ReadBlocks, *WriteBlocks, *FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL {
    u64 Revision;
    EFI_STATUS (*Open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **,
                       const c16 *, u64 OpenMode, u64 Attributes);
    EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL *);
    EFI_STATUS (*Delete)(struct EFI_FILE_PROTOCOL *);
    EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (*Write)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    void *GetPosition;
    EFI_STATUS (*SetPosition)(struct EFI_FILE_PROTOCOL *, u64);
    EFI_STATUS (*GetInfo)(struct EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, void *);
    void *SetInfo;
    EFI_STATUS (*Flush)(struct EFI_FILE_PROTOCOL *);
} EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ   0x1ULL
#define EFI_FILE_MODE_WRITE  0x2ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_DIRECTORY   0x10ULL

typedef struct {
    u64 Size, FileSize, PhysicalSize;
    EFI_TIME CreateTime, LastAccessTime, ModificationTime;
    u64 Attribute;
    c16 FileName[];
} EFI_FILE_INFO;

typedef struct {
    u64 Size;
    BOOLEAN ReadOnly;
    u64 VolumeSize, FreeSpace;
    u32 BlockSize;
    c16 VolumeLabel[];
} EFI_FILE_SYSTEM_INFO;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    u64 Revision;
    EFI_STATUS (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    u32 Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    u32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    u64 ImageSize;
    u32 ImageCodeType, ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- PCI ---------------------------------------------------------------- */
typedef struct { void *Read, *Write; } EFI_PCI_IO_ACCESS;
typedef struct { EFI_STATUS (*Read)(void *, u32 Width, u32 Offset, UINTN Count, void *Buf); void *Write; } EFI_PCI_IO_CONFIG_ACCESS;
typedef struct EFI_PCI_IO_PROTOCOL {
    void *PollMem, *PollIo;
    EFI_PCI_IO_ACCESS Mem, Io;
    EFI_PCI_IO_CONFIG_ACCESS Pci;
    void *CopyMem, *Map, *Unmap, *AllocateBuffer, *FreeBuffer, *Flush;
    EFI_STATUS (*GetLocation)(struct EFI_PCI_IO_PROTOCOL *, UINTN *Seg, UINTN *Bus, UINTN *Dev, UINTN *Fn);
    void *Attributes, *GetBarAttributes, *SetBarAttributes;
    u64 RomSize;
    void *RomImage;
} EFI_PCI_IO_PROTOCOL;

/* ---- MP services (run code on the other cores) ------------------------- */
typedef void (*EFI_AP_PROCEDURE)(void *Buffer);
typedef struct EFI_MP_SERVICES_PROTOCOL {
    EFI_STATUS (*GetNumberOfProcessors)(struct EFI_MP_SERVICES_PROTOCOL *, UINTN *Count, UINTN *Enabled);
    void *GetProcessorInfo;
    EFI_STATUS (*StartupAllAPs)(struct EFI_MP_SERVICES_PROTOCOL *, EFI_AP_PROCEDURE, BOOLEAN SingleThread,
                                EFI_EVENT WaitEvent, UINTN TimeoutUs, void *Arg, UINTN **FailedCpuList);
    void *StartupThisAP, *SwitchBSP, *EnableDisableAP;
    EFI_STATUS (*WhoAmI)(struct EFI_MP_SERVICES_PROTOCOL *, UINTN *ProcessorNumber);
} EFI_MP_SERVICES_PROTOCOL;

/* ---- Boot / runtime services ------------------------------------------ */
typedef struct {
    u32 Type;
    u64 PhysicalStart;
    u64 VirtualStart;
    u64 NumberOfPages;
    u64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData,
       EfiRuntimeServicesCode, EfiRuntimeServicesData, EfiConventionalMemory, EfiUnusableMemory,
       EfiACPIReclaimMemory, EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
       EfiPalCode, EfiPersistentMemory };
enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress };

#define TPL_HIGH_LEVEL 31
#define EVT_TIMER 0x80000000u
#define TimerCancel   0
#define TimerPeriodic 1
#define TimerRelative 2

typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;
typedef enum { EfiResetCold, EfiResetWarm, EfiResetShutdown } EFI_RESET_TYPE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    UINTN (*RaiseTPL)(UINTN NewTpl);
    void (*RestoreTPL)(UINTN OldTpl);
    EFI_STATUS (*AllocatePages)(u32 Type, u32 MemoryType, UINTN Pages, u64 *Memory);
    EFI_STATUS (*FreePages)(u64 Memory, UINTN Pages);
    EFI_STATUS (*GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, u32 *);
    EFI_STATUS (*AllocatePool)(u32 PoolType, UINTN, void **);
    EFI_STATUS (*FreePool)(void *);
    EFI_STATUS (*CreateEvent)(u32 Type, UINTN NotifyTpl, void *NotifyFn, void *Ctx, EFI_EVENT *);
    EFI_STATUS (*SetTimer)(EFI_EVENT, u32 Type, u64 TriggerTime100ns);
    EFI_STATUS (*WaitForEvent)(UINTN, EFI_EVENT *, UINTN *Index);
    void *SignalEvent;
    EFI_STATUS (*CloseEvent)(EFI_EVENT);
    EFI_STATUS (*CheckEvent)(EFI_EVENT);
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE, UINTN MapKey);
    void *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN Microseconds);
    EFI_STATUS (*SetWatchdogTimer)(UINTN Timeout, u64 Code, UINTN DataSize, c16 *Data);
    EFI_STATUS (*ConnectController)(EFI_HANDLE, EFI_HANDLE *, void *, BOOLEAN Recursive);
    EFI_STATUS (*DisconnectController)(EFI_HANDLE, EFI_HANDLE Driver, EFI_HANDLE Child);
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *, void *,
                                     UINTN *NoHandles, EFI_HANDLE **Buffer);
    EFI_STATUS (*LocateProtocol)(EFI_GUID *, void *, void **);
    void *InstallMultipleProtocolInterfaces, *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32, *CopyMem, *SetMem, *CreateEventEx;
} EFI_BOOT_SERVICES;

#define EFI_VARIABLE_NON_VOLATILE       0x1
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x2
#define EFI_VARIABLE_RUNTIME_ACCESS     0x4

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (*GetTime)(EFI_TIME *, void *Capabilities);
    void *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    EFI_STATUS (*GetVariable)(const c16 *, EFI_GUID *, u32 *Attr, UINTN *, void *);
    void *GetNextVariableName;
    EFI_STATUS (*SetVariable)(const c16 *, EFI_GUID *, u32 Attr, UINTN, void *);
    void *GetNextHighMonotonicCount;
    void (*ResetSystem)(EFI_RESET_TYPE, EFI_STATUS, UINTN, void *);
    void *UpdateCapsule, *QueryCapsuleCapabilities, *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    c16 *FirmwareVendor;
    u32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- GUIDs ------------------------------------------------------------- */
#define GOP_GUID            {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}}
#define SIMPLE_POINTER_GUID {0x31878c87,0x0b75,0x11d5,{0x9a,0x4f,0x00,0x90,0x27,0x3f,0xc1,0x4d}}
#define ABS_POINTER_GUID    {0x8d59d32b,0xc655,0x4ae9,{0x9b,0x15,0xf2,0x59,0x04,0x99,0x2a,0x43}}
#define SIMPLE_FS_GUID      {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define BLOCK_IO_GUID       {0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define FILE_INFO_GUID      {0x09576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define FS_INFO_GUID        {0x09576e93,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define LOADED_IMAGE_GUID   {0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
#define MP_SERVICES_GUID    {0x3fdda605,0xa76e,0x4f46,{0xad,0x29,0x12,0xf4,0x53,0x1b,0x3d,0x08}}
#define PCI_IO_GUID         {0x4cf5b200,0x68b8,0x4ca5,{0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}}
#define ACPI10_TABLE_GUID   {0xeb9d2d30,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}}
#define ACPI20_TABLE_GUID   {0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}}
#define SMBIOS_TABLE_GUID   {0xeb9d2d31,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}}
#define SMBIOS3_TABLE_GUID  {0xf2fd1544,0x9794,0x4a2c,{0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94}}
#define GLOBAL_VARIABLE_GUID {0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c}}
