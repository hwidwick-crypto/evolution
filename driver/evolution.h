//
// Evolution kernel driver.
//
// The driver can reach the kernel in two ways:
//
//   * as a normal WDM service driver - the I/O manager passes a real
//     DriverObject into DriverEntry;
//   * as a manually mapped image (kdmapper) - DriverEntry(NULL, NULL), so the
//     driver asks the kernel for a DriverObject through IoCreateDriver.
//
// In both cases it creates \Device\Evolution + \DosDevices\Evolution, i.e. the
// user mode device \\.\Evolution. The loader opens it to tell "the driver is
// already in the system" from "nothing is mapped yet".
//

#pragma once

#include <ntddk.h>

// Buffered ping: answer is EVOLUTION_PING_MAGIC.
#define EVOLUTION_IOCTL_PING \
	CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define EVOLUTION_PING_MAGIC 0xE70110ul

// Not declared in ntddk.h (only in ntifs.h), but exported by ntoskrnl.exe -
// exactly what the manual mapping path needs to get a DriverObject.
extern "C" NTSTATUS IoCreateDriver(
	_In_opt_ PUNICODE_STRING DriverName,
	_In_ PDRIVER_INITIALIZE InitializationFunction);
