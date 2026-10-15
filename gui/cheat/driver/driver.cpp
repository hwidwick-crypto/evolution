#include "driver.hpp"

#include <cstring>
#include <vector>

namespace Driver {
    HANDLE hDriver = INVALID_HANDLE_VALUE;
    DWORD  targetPid = 0;
    uintptr_t baseAddress = 0;

    bool InitDriver() {
        if (hDriver != INVALID_HANDLE_VALUE) return true;

        hDriver = CreateFileW(L"\\\\.\\Evolution", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return hDriver != INVALID_HANDLE_VALUE;
    }

    void CloseDriver() {
        if (hDriver != INVALID_HANDLE_VALUE) {
            CloseHandle(hDriver);
            hDriver = INVALID_HANDLE_VALUE;
        }
    }

    uintptr_t GetModuleBaseAddress(DWORD pid) {
        if (hDriver == INVALID_HANDLE_VALUE) return 0;

        COPY_REQUEST request{};
        request.ProcessId = pid;

        uintptr_t base = 0;
        DWORD returned = 0;
        if (!DeviceIoControl(hDriver, IOCTL_GET_BASE, &request, sizeof(request),
                             &base, sizeof(base), &returned, NULL))
            return 0;

        return base;
    }

    int GetVersion() {
        if (!InitDriver()) return -2;

        struct { unsigned long magic; unsigned long version; } ping{};
        DWORD returned = 0;
        if (!DeviceIoControl(hDriver, IOCTL_PING, nullptr, 0, &ping, sizeof(ping), &returned, NULL))
            return -2;

        // Old builds reply with the magic only (4 bytes).
        return returned == sizeof(ping) ? static_cast<int>(ping.version) : -1;
    }

    bool AllocateRemotePath(DWORD pid, uintptr_t targetProcessHandle,
                            const wchar_t* path, uintptr_t& remotePathOut) {
        remotePathOut = 0;
        if (hDriver == INVALID_HANDLE_VALUE || !pid || !targetProcessHandle || !path || !*path)
            return false;

        const size_t pathBytes = (wcslen(path) + 1) * sizeof(wchar_t);
        if (pathBytes > 1024) return false;

        std::vector<char> buffer(sizeof(INJECT_REQUEST) + pathBytes);

        INJECT_REQUEST request{};
        request.ProcessHandle = targetProcessHandle;
        request.ProcessId = pid;
        request.PathSize = static_cast<unsigned long>(pathBytes);

        std::memcpy(buffer.data(), &request, sizeof(request));
        std::memcpy(buffer.data() + sizeof(request), path, pathBytes);

        unsigned long long remote = 0;
        DWORD returned = 0;
        if (!DeviceIoControl(hDriver, IOCTL_INJECT, buffer.data(),
                             static_cast<DWORD>(buffer.size()),
                             &remote, sizeof(remote), &returned, NULL) ||
            returned != sizeof(remote))
            return false;

        remotePathOut = static_cast<uintptr_t>(remote);
        return remotePathOut != 0;
    }
}
