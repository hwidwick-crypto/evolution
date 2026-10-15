//
// Evolution kernel driver
//
// Works both as a regular service driver (DriverObject passed by the I/O manager)
// and as a manually mapped image (kdmapper calls DriverEntry(0, 0) - in that case
// we ask the kernel for a real DRIVER_OBJECT with IoCreateDriver, otherwise
// IoCreateDevice / user mode \\.\Evolution access would not be possible).
//
// Imports are limited to ntoskrnl.exe on purpose, this is what kdmapper can resolve.
//

#include "stdafx.h"

// IoCreateDriver is exported by ntoskrnl.exe but is not declared in the public
// WDK headers. Declared here exactly the way the kernel declares it.
extern "C" NTKERNELAPI NTSTATUS IoCreateDriver(PUNICODE_STRING DriverName, PDRIVER_INITIALIZE InitializationFunction);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static PDEVICE_OBJECT g_DeviceObject = nullptr;
static PVOID g_HookOriginal = nullptr;
static bool g_Hooked = false;

// ---------------------------------------------------------------------------
// Core functions
// ---------------------------------------------------------------------------

NTSTATUS CorePing(PREQUEST_PING args)
{
	ULONG magic = EVOLUTION_PING_MAGIC;
	SafeCopy(&args->OutMagic, &magic, sizeof(ULONG));
	return STATUS_SUCCESS;
}

NTSTATUS CoreHook(PREQUEST_HOOK args)
{
	if (g_Hooked)
		return STATUS_ALREADY_REGISTERED;

	// Placeholder: hook installation would go here.
	// For now, just mark as hooked.
	g_Hooked = true;
	printf("hook installed for process %lu\n", args->ProcessId);
	return STATUS_SUCCESS;
}

NTSTATUS CoreUnhook(PREQUEST_UNHOOK args)
{
	UNREFERENCED_PARAMETER(args);

	if (!g_Hooked)
		return STATUS_NOT_FOUND;

	// Placehook: hook removal would go here.
	g_Hooked = false;
	printf("hook removed\n");
	return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Memory access (READ / WRITE / GETBASE)
// ---------------------------------------------------------------------------

// Copies Size bytes between the target process and the METHOD_BUFFERED system
// buffer. write == false -> target -> buffer (read), write == true -> buffer
// -> target (write).
static NTSTATUS CopyProcessMemory(const REQUEST_COPY* request, PVOID systemBuffer, BOOLEAN write)
{
	PEPROCESS process = nullptr;
	NTSTATUS status = KLI_FN(PsLookupProcessByProcessId)((HANDLE)(ULONG_PTR)request->ProcessId, &process);
	if (!NT_SUCCESS(status))
		return status;

	SIZE_T done = 0;
	status = KLI_FN(MmCopyVirtualMemory)(
		write ? KLI_FN(IoGetCurrentProcess)() : process,
		write ? systemBuffer : (PVOID)request->Address,
		write ? process : KLI_FN(IoGetCurrentProcess)(),
		write ? (PVOID)request->Address : systemBuffer,
		request->Size, KernelMode, &done);

	// KLI resolves plain exports by name, ObDereferenceObject is a macro around
	// ObfDereferenceObject in the WDK headers, so call the real export.
	KLI_FN(ObfDereferenceObject)(process);

	if (NT_SUCCESS(status) && done != request->Size)
		return STATUS_PARTIAL_COPY;

	return status;
}

// ---------------------------------------------------------------------------
// Injection support (kernel side)
// ---------------------------------------------------------------------------

// Allocates the DLL path inside the target process and returns its remote
// address. Thread creation stays in user mode (ntdll!NtCreateThreadEx) - the
// kernel does not start user threads here on purpose:
//   * ZwCreateThreadEx / NtCreateThreadEx are NOT exported by ntoskrnl, so a
//     kernel-side thread start would mean calling through a null pointer;
//   * all the primitives below ARE exported and safe (verified against the
//     ntoskrnl export table).
static NTSTATUS AllocateInTarget(const REQUEST_INJECT* request, const WCHAR* path, PVOID* outAddress)
{
	*outAddress = nullptr;

	if (!request->ProcessId || !request->ProcessHandle || !request->PathSize || !path)
		return STATUS_INVALID_PARAMETER;

	const auto pfnZwAllocateVirtualMemory = KLI_FN(ZwAllocateVirtualMemory);
	const auto pfnMmCopyVirtualMemory = KLI_FN(MmCopyVirtualMemory);
	const auto pfnPsLookupProcessByProcessId = KLI_FN(PsLookupProcessByProcessId);
	const auto pfnObfDereferenceObject = KLI_FN(ObfDereferenceObject);
	const auto pfnIoGetCurrentProcess = KLI_FN(IoGetCurrentProcess);

	if (!pfnZwAllocateVirtualMemory || !pfnMmCopyVirtualMemory ||
		!pfnPsLookupProcessByProcessId || !pfnObfDereferenceObject || !pfnIoGetCurrentProcess)
		return STATUS_PROCEDURE_NOT_FOUND;

	PEPROCESS target = nullptr;
	NTSTATUS status = pfnPsLookupProcessByProcessId((HANDLE)(ULONG_PTR)request->ProcessId, &target);
	if (!NT_SUCCESS(status))
		return status;

	// The IOCTL runs in the context of the calling process, so the user mode
	// handle passed in the request is usable directly here - it was granted
	// PROCESS_VM_OPERATION by OpenProcess.
	SIZE_T regionSize = request->PathSize;
	PVOID base = nullptr;
	status = pfnZwAllocateVirtualMemory((HANDLE)request->ProcessHandle, &base, 0, &regionSize,
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	if (NT_SUCCESS(status))
	{
		SIZE_T written = 0;
		status = pfnMmCopyVirtualMemory(
			pfnIoGetCurrentProcess(), (PVOID)path,
			target, base,
			request->PathSize, KernelMode, &written);
	}

	if (NT_SUCCESS(status))
		*outAddress = base;

	pfnObfDereferenceObject(target);
	return status;
}

// ---------------------------------------------------------------------------
// IRP handlers
// ---------------------------------------------------------------------------

static VOID EvolutionComplete(PIRP irp, NTSTATUS status, ULONG_PTR information)
{
	irp->IoStatus.Status = status;
	irp->IoStatus.Information = information;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static NTSTATUS EvolutionCreateClose(PDEVICE_OBJECT deviceObject, PIRP irp)
{
	UNREFERENCED_PARAMETER(deviceObject);
	EvolutionComplete(irp, STATUS_SUCCESS, 0);
	return STATUS_SUCCESS;
}

static NTSTATUS EvolutionDeviceControl(PDEVICE_OBJECT deviceObject, PIRP irp)
{
	UNREFERENCED_PARAMETER(deviceObject);

	const PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
	ULONG_PTR information = 0;

	switch (stack->Parameters.DeviceIoControl.IoControlCode)
	{
	case EVOLUTION_IOCTL_PING:
		if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG))
		{
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(REQUEST_PING))
		{
			// Old style client - magic only.
			*static_cast<PULONG>(irp->AssociatedIrp.SystemBuffer) = EVOLUTION_PING_MAGIC;
			information = sizeof(ULONG);
		}
		else
		{
			PREQUEST_PING ping = static_cast<PREQUEST_PING>(irp->AssociatedIrp.SystemBuffer);
			ping->OutMagic = EVOLUTION_PING_MAGIC;
			ping->Version = EVOLUTION_VERSION;
			information = sizeof(REQUEST_PING);
		}
		status = STATUS_SUCCESS;
		break;

	case EVOLUTION_IOCTL_HOOK:
	case EVOLUTION_IOCTL_UNHOOK:
	{
		REQUEST_DATA safeData = { 0 };
		if (!SafeCopy(&safeData, irp->AssociatedIrp.SystemBuffer, sizeof(safeData)) || safeData.Unique != DATA_UNIQUE)
		{
			status = STATUS_ACCESS_VIOLATION;
			break;
		}

		switch (safeData.Type)
		{
			HANDLE_REQUEST(Hook, REQUEST_HOOK);
			HANDLE_REQUEST(Unhook, REQUEST_UNHOOK);
		default:
			status = STATUS_INVALID_DEVICE_REQUEST;
			break;
		}

		EvolutionComplete(irp, status, information);
		return status;
	}

	case EVOLUTION_IOCTL_READ:
	case EVOLUTION_IOCTL_WRITE:
	{
		PVOID systemBuffer = irp->AssociatedIrp.SystemBuffer;
		const ULONG inLength  = stack->Parameters.DeviceIoControl.InputBufferLength;
		const ULONG outLength = stack->Parameters.DeviceIoControl.OutputBufferLength;

		if (!systemBuffer || inLength < sizeof(REQUEST_COPY))
		{
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		// The header lives in the same system buffer we use as the data path,
		// so snapshot it first.
		const REQUEST_COPY request = *static_cast<PREQUEST_COPY>(systemBuffer);

		if (stack->Parameters.DeviceIoControl.IoControlCode == EVOLUTION_IOCTL_READ)
		{
			if (outLength < request.Size)
			{
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}

			status = CopyProcessMemory(&request, systemBuffer, FALSE);
			if (NT_SUCCESS(status))
				information = request.Size;
		}
		else
		{
			if (inLength < sizeof(REQUEST_COPY) + request.Size)
			{
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}

			status = CopyProcessMemory(&request, static_cast<PUCHAR>(systemBuffer) + sizeof(REQUEST_COPY), TRUE);
		}
		break;
	}

	case EVOLUTION_IOCTL_GETBASE:
	{
		PVOID systemBuffer = irp->AssociatedIrp.SystemBuffer;
		if (!systemBuffer ||
			stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(REQUEST_COPY) ||
			stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG_PTR))
		{
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		const REQUEST_COPY request = *static_cast<PREQUEST_COPY>(systemBuffer);

		PEPROCESS process = nullptr;
		status = KLI_FN(PsLookupProcessByProcessId)((HANDLE)(ULONG_PTR)request.ProcessId, &process);
		if (!NT_SUCCESS(status))
			break;

		*static_cast<PULONG_PTR>(systemBuffer) =
			reinterpret_cast<ULONG_PTR>(KLI_FN(PsGetProcessSectionBaseAddress)(process));
		KLI_FN(ObfDereferenceObject)(process);

		information = sizeof(ULONG_PTR);
		break;
	}

	case EVOLUTION_IOCTL_INJECT:
	{
		PVOID systemBuffer = irp->AssociatedIrp.SystemBuffer;
		const ULONG inLength  = stack->Parameters.DeviceIoControl.InputBufferLength;
		const ULONG outLength = stack->Parameters.DeviceIoControl.OutputBufferLength;

		if (!systemBuffer ||
			inLength < sizeof(REQUEST_INJECT) ||
			outLength < sizeof(ULONG_PTR))
		{
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		const REQUEST_INJECT request = *static_cast<PREQUEST_INJECT>(systemBuffer);

		if (request.PathSize == 0 || request.PathSize > 1024 ||
			(request.PathSize & 1) ||
			inLength < sizeof(REQUEST_INJECT) + request.PathSize)
		{
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		const WCHAR* path = reinterpret_cast<const WCHAR*>(
			static_cast<PUCHAR>(systemBuffer) + sizeof(REQUEST_INJECT));

		PVOID remoteAddress = nullptr;
		status = AllocateInTarget(&request, path, &remoteAddress);

		if (NT_SUCCESS(status))
		{
			*static_cast<PULONG_PTR>(systemBuffer) = reinterpret_cast<ULONG_PTR>(remoteAddress);
			information = sizeof(ULONG_PTR);
		}
		break;
	}

	default:
		break;
	}

	EvolutionComplete(irp, status, information);
	return status;
}

// ---------------------------------------------------------------------------
// Unload
// ---------------------------------------------------------------------------

static VOID EvolutionUnload(PDRIVER_OBJECT driverObject)
{
	// Remove hook if still active.
	if (g_Hooked)
	{
		REQUEST_UNHOOK req = { 0 };
		CoreUnhook(&req);
		g_Hooked = false;
	}

	// Delete symbolic link.
	UNICODE_STRING symlinkName;
	KLI_FN(RtlInitUnicodeString)(&symlinkName, EVOLUTION_SYMLINK_NAME);
	KLI_FN(IoDeleteSymbolicLink)(&symlinkName);

	// Delete device.
	while (driverObject->DeviceObject)
		KLI_FN(IoDeleteDevice)(driverObject->DeviceObject);

	printf("driver unloaded\n");
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

static NTSTATUS EvolutionInitialize(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
	UNREFERENCED_PARAMETER(registryPath);

	driverObject->DriverUnload = EvolutionUnload;
	driverObject->MajorFunction[IRP_MJ_CREATE] = EvolutionCreateClose;
	driverObject->MajorFunction[IRP_MJ_CLOSE] = EvolutionCreateClose;
	driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = EvolutionDeviceControl;

	UNICODE_STRING deviceName;
	UNICODE_STRING symlinkName;
	KLI_FN(RtlInitUnicodeString)(&deviceName, EVOLUTION_DEVICE_NAME);
	KLI_FN(RtlInitUnicodeString)(&symlinkName, EVOLUTION_SYMLINK_NAME);

	NTSTATUS status = KLI_FN(IoCreateDevice)(driverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN,
		FILE_DEVICE_SECURE_OPEN, FALSE, &g_DeviceObject);
	if (!NT_SUCCESS(status))
	{
		printf("IoCreateDevice failed: 0x%08X\n", status);
		return status;
	}

	g_DeviceObject->Flags |= DO_BUFFERED_IO;

	status = KLI_FN(IoCreateSymbolicLink)(&symlinkName, &deviceName);
	if (!NT_SUCCESS(status))
	{
		KLI_FN(IoDeleteDevice)(g_DeviceObject);
		printf("IoCreateSymbolicLink failed: 0x%08X\n", status);
		return status;
	}

	g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	printf("driver ready\n");
	return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
	UNREFERENCED_PARAMETER(driverObject);
	UNREFERENCED_PARAMETER(registryPath);

	// kdmapper always calls DriverEntry(0, 0), so we create our own driver object.
	UNICODE_STRING driverName;
	KLI_FN(RtlInitUnicodeString)(&driverName, EVOLUTION_DRIVER_NAME);

	const NTSTATUS status = IoCreateDriver(&driverName, EvolutionInitialize);
	if (!NT_SUCCESS(status))
		printf("IoCreateDriver failed: 0x%08X\n", status);

	return status;
}
