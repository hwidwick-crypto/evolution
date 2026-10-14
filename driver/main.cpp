//
// Evolution kernel driver - HelloWorld style + marker device.
//
// Prints a debug line and creates \Device\Evolution + \DosDevices\Evolution,
// so the loader can see that the driver is really present (\\.\Evolution).
//

#include <ntddk.h>
#include "evolution.h"

static NTSTATUS CompleteIrp(PIRP irp, NTSTATUS status, ULONG_PTR information = 0)
{
	irp->IoStatus.Status = status;
	irp->IoStatus.Information = information;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return status;
}

static NTSTATUS NTAPI DispatchCreateClose(PDEVICE_OBJECT deviceObject, PIRP irp)
{
	UNREFERENCED_PARAMETER(deviceObject);
	return CompleteIrp(irp, STATUS_SUCCESS);
}

static NTSTATUS NTAPI DispatchDeviceControl(PDEVICE_OBJECT deviceObject, PIRP irp)
{
	UNREFERENCED_PARAMETER(deviceObject);

	const PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);

	if (stack->Parameters.DeviceIoControl.IoControlCode == EVOLUTION_IOCTL_PING &&
		irp->AssociatedIrp.SystemBuffer != nullptr &&
		stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG))
	{
		const ULONG magic = EVOLUTION_PING_MAGIC;
		RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &magic, sizeof(magic));
		return CompleteIrp(irp, STATUS_SUCCESS, sizeof(magic));
	}

	return CompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST);
}

// Runs with a real DriverObject, no matter how the driver got into the kernel.
// Signature matches PDRIVER_INITIALIZE: (DriverObject, RegistryPath).
static NTSTATUS NTAPI EvolutionInitialize(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
	UNREFERENCED_PARAMETER(registryPath);

	UNICODE_STRING deviceName;
	UNICODE_STRING dosDeviceName;
	RtlInitUnicodeString(&deviceName, L"\\Device\\Evolution");
	RtlInitUnicodeString(&dosDeviceName, L"\\DosDevices\\Evolution");

	driverObject->MajorFunction[IRP_MJ_CREATE]         = DispatchCreateClose;
	driverObject->MajorFunction[IRP_MJ_CLOSE]          = DispatchCreateClose;
	driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;

	PDEVICE_OBJECT deviceObject = nullptr;
	NTSTATUS status = IoCreateDevice(driverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN,
		FILE_DEVICE_SECURE_OPEN, FALSE /* exclusive */, &deviceObject);
	if (!NT_SUCCESS(status))
		return status;

	status = IoCreateSymbolicLink(&dosDeviceName, &deviceName);
	if (!NT_SUCCESS(status))
	{
		IoDeleteDevice(deviceObject);
		return status;
	}

	deviceObject->Flags |= DO_BUFFERED_IO;
	deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	DbgPrintEx(0, 0, "Evolution: \\Device\\Evolution is ready\n");
	return STATUS_SUCCESS;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
	UNREFERENCED_PARAMETER(registryPath);

	DbgPrintEx(0, 0, "Hello world!\n");

	// normal service load - the I/O manager gave us a DriverObject
	if (driverObject != nullptr)
		return EvolutionInitialize(driverObject, registryPath);

	// manually mapped (kdmapper calls DriverEntry(NULL, NULL)) - ask the kernel
	// for a DriverObject, otherwise IoCreateDevice is impossible
	UNICODE_STRING driverName;
	RtlInitUnicodeString(&driverName, L"\\Driver\\Evolution");
	return IoCreateDriver(&driverName, EvolutionInitialize);
}
