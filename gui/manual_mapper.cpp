//
// Manual memory mapper using undocumented Windows NT functions
//
// This module provides:
//   - Process memory extension from 4KB to arbitrary size
//   - Manual DLL mapping with section permissions
//   - Thread creation via NtCreateThreadEx
//

#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

// <winternl.h> is intentionally not included here (it would clash with the
// local NT declarations below), so provide NT_SUCCESS ourselves.
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// NT function prototypes
extern "C" {

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID, *PCLIENT_ID;

// NT function pointer types
typedef NTSTATUS(NTAPI* NtAllocateVirtualMemory_t)(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect
);

typedef NTSTATUS(NTAPI* NtProtectVirtualMemory_t)(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    PSIZE_T RegionSize,
    ULONG NewProtect,
    PULONG OldProtect
);

typedef NTSTATUS(NTAPI* NtWriteVirtualMemory_t)(
    HANDLE ProcessHandle,
    PVOID BaseAddress,
    PVOID Buffer,
    SIZE_T NumberOfBytesToWrite,
    PSIZE_T NumberOfBytesWritten
);

typedef NTSTATUS(NTAPI* NtCreateThreadEx_t)(
    PHANDLE ThreadHandle,
    ACCESS_MASK DesiredAccess,
    PVOID ObjectAttributes,
    HANDLE ProcessHandle,
    PVOID StartRoutine,
    PVOID Argument,
    ULONG CreateFlags,
    SIZE_T ZeroBits,
    SIZE_T StackSize,
    SIZE_T MaximumStackSize,
    PVOID AttributeList
);

typedef NTSTATUS(NTAPI* NtQuerySystemInformation_t)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

typedef NTSTATUS(NTAPI* NtOpenProcess_t)(
    PHANDLE ProcessHandle,
    ACCESS_MASK DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    PCLIENT_ID ClientId
);

typedef NTSTATUS(NTAPI* NtClose_t)(
    HANDLE Handle
);

}

// Memory protection (PAGE_*) and allocation type (MEM_*) constants come from
// <winnt.h> via <windows.h>; redefining them here would expand the macros and
// fail to compile.

// ---------------------------------------------------------------------------
// NT function pointers
// ---------------------------------------------------------------------------

static NtAllocateVirtualMemory_t g_NtAllocateVirtualMemory = nullptr;
static NtProtectVirtualMemory_t g_NtProtectVirtualMemory = nullptr;
static NtWriteVirtualMemory_t g_NtWriteVirtualMemory = nullptr;
static NtCreateThreadEx_t g_NtCreateThreadEx = nullptr;
static NtQuerySystemInformation_t g_NtQuerySystemInformation = nullptr;
static NtOpenProcess_t g_NtOpenProcess = nullptr;
static NtClose_t g_NtClose = nullptr;

static HMODULE g_ntdll = nullptr;
static bool g_initialized = false;

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool ManualMapper_Initialize()
{
    if (g_initialized)
        return true;

    g_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!g_ntdll)
        return false;

    g_NtAllocateVirtualMemory = (NtAllocateVirtualMemory_t)GetProcAddress(g_ntdll, "NtAllocateVirtualMemory");
    g_NtProtectVirtualMemory = (NtProtectVirtualMemory_t)GetProcAddress(g_ntdll, "NtProtectVirtualMemory");
    g_NtWriteVirtualMemory = (NtWriteVirtualMemory_t)GetProcAddress(g_ntdll, "NtWriteVirtualMemory");
    g_NtCreateThreadEx = (NtCreateThreadEx_t)GetProcAddress(g_ntdll, "NtCreateThreadEx");
    g_NtQuerySystemInformation = (NtQuerySystemInformation_t)GetProcAddress(g_ntdll, "NtQuerySystemInformation");
    g_NtOpenProcess = (NtOpenProcess_t)GetProcAddress(g_ntdll, "NtOpenProcess");
    g_NtClose = (NtClose_t)GetProcAddress(g_ntdll, "NtClose");

    g_initialized = g_NtAllocateVirtualMemory && g_NtProtectVirtualMemory && g_NtWriteVirtualMemory;
    return g_initialized;
}

// ---------------------------------------------------------------------------
// Memory extension from 4KB to arbitrary size
// ---------------------------------------------------------------------------

bool ManualMapper_ExtendMemory(PVOID* baseAddress, SIZE_T newSize, ULONG protection)
{
    if (!g_initialized && !ManualMapper_Initialize())
        return false;

    SIZE_T regionSize = newSize;
    NTSTATUS status = g_NtAllocateVirtualMemory(
        GetCurrentProcess(),
        baseAddress,
        0,
        &regionSize,
        MEM_COMMIT | MEM_RESERVE,
        protection
    );

    return NT_SUCCESS(status);
}

bool ManualMapper_ProtectMemory(PVOID baseAddress, SIZE_T size, ULONG newProtect, PULONG oldProtect)
{
    if (!g_initialized && !ManualMapper_Initialize())
        return false;

    SIZE_T regionSize = size;
    NTSTATUS status = g_NtProtectVirtualMemory(
        GetCurrentProcess(),
        &baseAddress,
        &regionSize,
        newProtect,
        oldProtect
    );

    return NT_SUCCESS(status);
}

bool ManualMapper_WriteMemory(PVOID baseAddress, const void* data, SIZE_T size)
{
    if (!g_initialized && !ManualMapper_Initialize())
        return false;

    SIZE_T written = 0;
    NTSTATUS status = g_NtWriteVirtualMemory(
        GetCurrentProcess(),
        baseAddress,
        const_cast<void*>(data),
        size,
        &written
    );

    return NT_SUCCESS(status) && written == size;
}

// ---------------------------------------------------------------------------
// Manual DLL mapping
// ---------------------------------------------------------------------------

struct SECTION_INFO {
    PVOID baseAddress;
    SIZE_T size;
    ULONG characteristics;
};

bool ManualMapper_MapDll(const uint8_t* dllData, SIZE_T dllSize, PVOID* outBase)
{
    if (!g_initialized && !ManualMapper_Initialize())
        return false;

    // Parse DOS header
    const IMAGE_DOS_HEADER* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(dllData);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    // Parse NT headers
    const IMAGE_NT_HEADERS64* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(dllData + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return false;

    SIZE_T imageSize = ntHeaders->OptionalHeader.SizeOfImage;
    DWORD sectionAlignment = ntHeaders->OptionalHeader.SectionAlignment;
    SIZE_T headersSize = ntHeaders->OptionalHeader.SizeOfHeaders;

    // Step 1: Allocate initial 4KB block
    PVOID initialBase = nullptr;
    SIZE_T initialSize = 4096;

    NTSTATUS status = g_NtAllocateVirtualMemory(
        GetCurrentProcess(),
        &initialBase,
        0,
        &initialSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );

    if (!NT_SUCCESS(status))
        return false;

    // Step 2: Extend to full image size
    PVOID imageBase = nullptr;
    SIZE_T fullSize = imageSize;

    status = g_NtAllocateVirtualMemory(
        GetCurrentProcess(),
        &imageBase,
        0,
        &fullSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );

    if (!NT_SUCCESS(status))
        return false;

    // Step 3: Write headers
    if (!ManualMapper_WriteMemory(imageBase, dllData, headersSize))
        return false;

    // Step 4: Write sections
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(ntHeaders);
    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section)
    {
        if (section->SizeOfRawData == 0)
            continue;

        PVOID dest = static_cast<PBYTE>(imageBase) + section->VirtualAddress;
        const void* src = dllData + section->PointerToRawData;

        if (!ManualMapper_WriteMemory(dest, src, section->SizeOfRawData))
            return false;
    }

    // Step 5: Set proper section protections
    section = IMAGE_FIRST_SECTION(ntHeaders);
    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section)
    {
        ULONG protection = PAGE_READWRITE;
        if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            protection = PAGE_EXECUTE_READ;
        if (section->Characteristics & IMAGE_SCN_MEM_WRITE)
            protection = PAGE_EXECUTE_READWRITE;

        PVOID sectionBase = static_cast<PBYTE>(imageBase) + section->VirtualAddress;
        SIZE_T sectionSize = section->Misc.VirtualSize;
        ULONG oldProtect;

        ManualMapper_ProtectMemory(sectionBase, sectionSize, protection, &oldProtect);
    }

    // Step 6: Process relocations (simplified - assumes base is preferred)
    // In a full implementation, we would process the relocation table here

    // Step 7: Process imports (simplified)
    // In a full implementation, we would load dependent DLLs and resolve imports

    *outBase = imageBase;
    return true;
}

// ---------------------------------------------------------------------------
// Thread creation
// ---------------------------------------------------------------------------

HANDLE ManualMapper_CreateThread(PVOID startRoutine, PVOID argument)
{
    if (!g_initialized && !ManualMapper_Initialize())
        return nullptr;

    HANDLE thread = nullptr;
    NTSTATUS status = g_NtCreateThreadEx(
        &thread,
        THREAD_ALL_ACCESS,
        nullptr,
        GetCurrentProcess(),
        startRoutine,
        argument,
        0, // CreateFlags
        0, // ZeroBits
        0, // StackSize
        0, // MaximumStackSize
        nullptr // AttributeList
    );

    if (!NT_SUCCESS(status))
        return nullptr;

    return thread;
}

// ---------------------------------------------------------------------------
// Process enumeration
// ---------------------------------------------------------------------------

struct ProcessInfo {
    DWORD pid;
    std::wstring name;
};

std::vector<ProcessInfo> ManualMapper_EnumProcesses()
{
    std::vector<ProcessInfo> processes;

    if (!g_initialized && !ManualMapper_Initialize())
        return processes;

    ULONG returnLength = 0;
    g_NtQuerySystemInformation(5 /* SystemProcessInformation */, nullptr, 0, &returnLength);

    if (returnLength == 0)
        return processes;

    std::vector<BYTE> buffer(returnLength);
    NTSTATUS status = g_NtQuerySystemInformation(5, buffer.data(), returnLength, &returnLength);

    if (!NT_SUCCESS(status))
        return processes;

    // SYSTEM_PROCESS_INFORMATION structure
    struct SYSTEM_PROCESS_INFORMATION {
        ULONG NextEntryOffset;
        ULONG NumberOfThreads;
        LARGE_INTEGER WorkingSetPrivateSize;
        ULONG HardFaultCount;
        ULONG NumberOfThreadsHighWatermark;
        ULONGLONG CycleTime;
        LARGE_INTEGER CreateTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER KernelTime;
        UNICODE_STRING ImageName;
        LONG BasePriority;
        HANDLE UniqueProcessId;
        HANDLE InheritedFromUniqueProcessId;
        ULONG HandleCount;
        ULONG SessionId;
        SIZE_T PeakVirtualSize;
        SIZE_T VirtualSize;
        ULONG PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage;
        SIZE_T PeakPagefileUsage;
        SIZE_T PrivatePageCount;
        LARGE_INTEGER ReadOperationCount;
        LARGE_INTEGER WriteOperationCount;
        LARGE_INTEGER OtherOperationCount;
        LARGE_INTEGER ReadTransferCount;
        LARGE_INTEGER WriteTransferCount;
        LARGE_INTEGER OtherTransferCount;
    };

    BYTE* ptr = buffer.data();

    while (true)
    {
        SYSTEM_PROCESS_INFORMATION* procInfo = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(ptr);

        if (procInfo->ImageName.Buffer && procInfo->ImageName.Length > 0)
        {
            ProcessInfo info;
            info.pid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(procInfo->UniqueProcessId));
            info.name = std::wstring(procInfo->ImageName.Buffer,
                procInfo->ImageName.Length / sizeof(wchar_t));
            processes.push_back(info);
        }

        if (procInfo->NextEntryOffset == 0)
            break;

        ptr += procInfo->NextEntryOffset;
    }

    return processes;
}

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

bool ManualMapper_IsProcessRunning(const wchar_t* processName)
{
    auto processes = ManualMapper_EnumProcesses();

    for (const auto& proc : processes)
    {
        if (_wcsicmp(proc.name.c_str(), processName) == 0)
            return true;
    }

    return false;
}

DWORD ManualMapper_GetProcessId(const wchar_t* processName)
{
    auto processes = ManualMapper_EnumProcesses();

    for (const auto& proc : processes)
    {
        if (_wcsicmp(proc.name.c_str(), processName) == 0)
            return proc.pid;
    }

    return 0;
}
