//
// Evolution kernel driver
//
// The loader (Evolution.exe) maps this image with kdmapper and then checks the
// \\.\Evolution device to find out whether the driver is already loaded.
//

#pragma once

// Device + dos symlink, the loader opens \\.\Evolution to detect the driver.
#define EVOLUTION_DEVICE_NAME      L"\\Device\\Evolution"
#define EVOLUTION_SYMLINK_NAME     L"\\DosDevices\\Evolution"

// Driver object used when the image is mapped manually (kdmapper calls DriverEntry
// without arguments, so the driver has to ask the kernel for a DRIVER_OBJECT).
#define EVOLUTION_DRIVER_NAME      L"\\Driver\\Evolution"

// Magic value for request validation.
#define DATA_UNIQUE (0x1234)

// IOCTL codes.
#define EVOLUTION_IOCTL_PING       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EVOLUTION_IOCTL_HOOK       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EVOLUTION_IOCTL_UNHOOK     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EVOLUTION_PING_MAGIC       0xE70110u

// Memory access IOCTLs used by the injected GUI (cheat features) to read/write
// the game process through the driver.
//   READ    - input: REQUEST_COPY,              output: Size bytes of target memory
//   WRITE   - input: REQUEST_COPY + payload,    output: none
//   GETBASE - input: REQUEST_COPY (Address/Size ignored), output: image base (8 bytes)
#define EVOLUTION_IOCTL_READ        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EVOLUTION_IOCTL_WRITE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define EVOLUTION_IOCTL_GETBASE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)

// DLL injection performed entirely in kernel mode (ZwAllocateVirtualMemory +
// ZwCreateThreadEx on a kernel handle). Used as a fallback when the game
// blocks the classic user mode CreateRemoteThread injection.
//   input: REQUEST_INJECT + null-terminated UTF-16 path bytes
#define EVOLUTION_IOCTL_INJECT      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x813, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Bumped whenever the usermode protocol changes; the client refuses to talk
// IOCTLs to an older resident build (see Driver::GetVersion in the GUI DLL).
#define EVOLUTION_VERSION           2

// Request types.
typedef enum _REQUEST_TYPE {
	RequestTypePing,
	RequestTypeHook,
	RequestTypeUnhook,
} REQUEST_TYPE;

typedef struct _REQUEST_DATA {
	DWORD Unique;
	REQUEST_TYPE Type;
	PVOID Arguments;
} REQUEST_DATA, *PREQUEST_DATA;

typedef struct _REQUEST_PING {
	ULONG OutMagic;
	ULONG Version;
} REQUEST_PING, *PREQUEST_PING;

typedef struct _REQUEST_HOOK {
	ULONG ProcessId;
} REQUEST_HOOK, *PREQUEST_HOOK;

typedef struct _REQUEST_UNHOOK {
	ULONG Reserved;
} REQUEST_UNHOOK, *PREQUEST_UNHOOK;

// Memory copy descriptor for the READ / WRITE / GETBASE IOCTLs.
// Layout must stay in sync with Driver::COPY_REQUEST in gui\cheat\driver\driver.hpp.
typedef struct _REQUEST_COPY {
	ULONG ProcessId;
	ULONG_PTR Address;
	SIZE_T Size;
} REQUEST_COPY, *PREQUEST_COPY;

// Injection support: the driver only ALLOCATES the path buffer inside the
// target (ZwAllocateVirtualMemory + MmCopyVirtualMemory) - thread creation is
// done from user mode via ntdll!NtCreateThreadEx, because ZwCreateThreadEx is
// NOT exported by ntoskrnl and cannot be resolved from the kernel.
//   input:  REQUEST_INJECT + null-terminated UTF-16 path bytes
//   output: remote path address (8 bytes)
// Layout must stay in sync with Driver::INJECT_REQUEST.
typedef struct _REQUEST_INJECT {
	ULONG_PTR ProcessHandle;  // target handle held by the CALLING process
	ULONG ProcessId;
	ULONG PathSize;           // UTF-16 path bytes incl. the terminator
} REQUEST_INJECT, *PREQUEST_INJECT;

// Core functions.
NTSTATUS CorePing(PREQUEST_PING args);
NTSTATUS CoreHook(PREQUEST_HOOK args);
NTSTATUS CoreUnhook(PREQUEST_UNHOOK args);
