#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cstdint>

// User-mode client for the Evolution kernel driver.
// Keep the IOCTL codes in sync with driver\evolution.h (METHOD_BUFFERED:
//   READ    -> input: COPY_REQUEST,           output: Size bytes of memory
//   WRITE   -> input: COPY_REQUEST + payload, output: none
//   GETBASE -> input: COPY_REQUEST,           output: image base, 8 bytes
//   INJECT  -> input: INJECT_REQUEST + path,  output: none
// )
namespace Driver {
    constexpr DWORD IOCTL_PING     = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD IOCTL_READ_MEM  = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD IOCTL_WRITE_MEM = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD IOCTL_GET_BASE  = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD IOCTL_INJECT    = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x813, METHOD_BUFFERED, FILE_ANY_ACCESS);

    // Bumped when the IOCTL protocol changes; keep in sync with EVOLUTION_VERSION
    // in driver\evolution.h. The client refuses kernel IOCTLs on older builds.
    constexpr unsigned long EVOLUTION_VERSION = 2;

    // Must match REQUEST_COPY in driver\evolution.h.
    struct COPY_REQUEST {
        unsigned long long ProcessId;
        unsigned long long Address;
        unsigned long long Size;
    };
    static_assert(sizeof(COPY_REQUEST) == 24, "COPY_REQUEST layout must match kernel REQUEST_COPY");

    // Must match REQUEST_INJECT in driver\evolution.h.
    struct INJECT_REQUEST {
        unsigned long long ProcessHandle; // target handle held by this process
        unsigned long ProcessId;
        unsigned long PathSize;           // UTF-16 path bytes incl. the terminator
    };
    static_assert(sizeof(INJECT_REQUEST) == 16, "INJECT_REQUEST layout must match kernel REQUEST_INJECT");

    extern HANDLE hDriver;
    extern DWORD  targetPid;
    extern uintptr_t baseAddress;

    bool InitDriver();
    void CloseDriver();
    uintptr_t GetModuleBaseAddress(DWORD pid);

    // Pings the driver, returns EVOLUTION_VERSION on success, -1 for an old /
    // unknown build, -2 when the driver is not reachable at all.
    int GetVersion();

    // Kernel side of the injection fallback: allocates the path inside the
    // target process and returns its remote address. Thread creation stays in
    // user mode (ntdll!NtCreateThreadEx) - ZwCreateThreadEx is not exported by
    // ntoskrnl, kernel-side thread start is impossible.
    bool AllocateRemotePath(DWORD pid, uintptr_t targetProcessHandle,
                            const wchar_t* path, uintptr_t& remotePathOut);

    template <typename T>
    T ReadMemory(uintptr_t address) {
        T buffer{};
        if (hDriver == INVALID_HANDLE_VALUE || !address) return buffer;

        COPY_REQUEST request{};
        request.ProcessId = targetPid;
        request.Address = address;
        request.Size = sizeof(T);

        DWORD returned = 0;
        if (!DeviceIoControl(hDriver, IOCTL_READ_MEM, &request, sizeof(request),
                             &buffer, sizeof(T), &returned, NULL) || returned != sizeof(T))
            return T{};

        return buffer;
    }

    template <typename T>
    bool WriteMemory(uintptr_t address, const T& value) {
        if (hDriver == INVALID_HANDLE_VALUE || !address) return false;

        struct {
            COPY_REQUEST header;
            T payload;
        } request{};

        request.header.ProcessId = targetPid;
        request.header.Address = address;
        request.header.Size = sizeof(T);
        request.payload = value;

        DWORD returned = 0;
        return DeviceIoControl(hDriver, IOCTL_WRITE_MEM, &request, sizeof(request),
                               nullptr, 0, &returned, NULL) != FALSE;
    }
}
