//
// Evolution GUI - DLL entry point and exported functions
//
// Two contexts:
//
// 1. Loader process ("GUI Mode" in Evolution.exe) - the loader calls:
//      EvolutionGui_Init        - console output + waiter thread
//      EvolutionGui_WaitForGTA  - blocks until GTA5.exe / FiveM appears
//      EvolutionGui_MapDriver   - maps evolution.sys via kdmapper (console
//                                 confirmation is done by the loader with Enter)
//      EvolutionGui_Inject      - injects this DLL into the game process
//      EvolutionGui_Shutdown    - cleanup
//
// 2. Game process (after injection) - DllMain starts the present hook:
//      INSERT toggles the menu, ESC closes it, END unhooks, cleans traces and
//      unloads the DLL.
//

#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdio>

#include <kdmapper.hpp>
#include <utils.hpp>
#include <intel_driver.hpp>

#include "framework/core/present_hook.h"
#include "utils/process.hpp"
#include "driver/driver.hpp"

// NT types and constants
typedef long NTSTATUS;
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#endif
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)

// Link required libraries
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static const wchar_t* const kDriverFileName = L"evolution.sys";
static const wchar_t* const kDevicePath = L"\\\\.\\Evolution";
static const wchar_t* const kServiceName = L"Evolution";

static const wchar_t* const kGameProcessNamesW[] = {
    L"GTA5.exe", L"GTA5_Enhanced.exe", L"FiveM.exe",
};

// ---------------------------------------------------------------------------
// Global state (loader context)
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{ false };
static std::atomic<bool> g_gtaDetected{ false };
static std::thread g_waiterThread;

// ---------------------------------------------------------------------------
// Console output helper
// ---------------------------------------------------------------------------

static HANDLE g_console = INVALID_HANDLE_VALUE;

static void InitConsole()
{
    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
}

static void PrintColor(const char* prefix, WORD color, const std::string& msg)
{
    if (g_console == INVALID_HANDLE_VALUE)
        return;
    SetConsoleTextAttribute(g_console, color);
    printf("%s %s\n", prefix, msg.c_str());
    SetConsoleTextAttribute(g_console, 7);
}

#define PRINT_GRAY(msg)  PrintColor("[~]", 8, (msg))
#define PRINT_GREEN(msg) PrintColor("[+]", 10, (msg))
#define PRINT_RED(msg)   PrintColor("[!]", 12, (msg))
#define PRINT_YELLOW(msg)PrintColor("[!]", 14, (msg))
#define PRINT_CYAN(msg)  PrintColor("[*]", 11, (msg))

// ---------------------------------------------------------------------------
// Game process detection
// ---------------------------------------------------------------------------

static DWORD FindGamePid()
{
    for (const wchar_t* name : kGameProcessNamesW)
    {
        const DWORD pid = Utils::Process::GetProcessId(name);
        if (pid)
            return pid;
    }
    return 0;
}

static bool IsGameProcess()
{
    wchar_t host[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, host, MAX_PATH))
        return false;

    const wchar_t* base = wcsrchr(host, L'\\');
    base = base ? base + 1 : host;

    for (const wchar_t* name : kGameProcessNamesW)
    {
        if (_wcsicmp(base, name) == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Driver detection (from loader)
// ---------------------------------------------------------------------------

static bool IsDriverLoaded()
{
    const HANDLE device = CreateFileW(kDevicePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (device && device != INVALID_HANDLE_VALUE)
    {
        CloseHandle(device);
        return true;
    }

    if (GetLastError() != ERROR_FILE_NOT_FOUND)
        return true;

    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;

    bool running = false;
    const SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    if (service)
    {
        SERVICE_STATUS_PROCESS status = {};
        DWORD needed = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed))
        {
            running = status.dwCurrentState == SERVICE_RUNNING ||
                status.dwCurrentState == SERVICE_START_PENDING;
        }
        CloseServiceHandle(service);
    }

    CloseServiceHandle(scm);
    return running;
}

// ---------------------------------------------------------------------------
// Driver mapping (from loader)
// ---------------------------------------------------------------------------

static bool IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    const BOOL OK = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return OK && elevation.TokenIsElevated;
}

static std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

static std::vector<uint8_t> ReadFileToMemory(const std::wstring& path)
{
    std::vector<uint8_t> data;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return data;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize))
    {
        CloseHandle(hFile);
        return data;
    }

    data.resize(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, data.data(), static_cast<DWORD>(data.size()), &bytesRead, nullptr))
    {
        data.clear();
    }

    CloseHandle(hFile);
    return data;
}

static std::wstring FindDriverFile()
{
    wchar_t buffer[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, buffer, MAX_PATH))
        return {};

    std::wstring dir = buffer;
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        dir = dir.substr(0, pos);

    // Check next to the DLL
    std::wstring preferred = dir + L"\\" + kDriverFileName;
    if (GetFileAttributesW(preferred.c_str()) != INVALID_FILE_ATTRIBUTES)
        return preferred;

    // Check next to the loader exe
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(GetModuleHandle(nullptr), exePath, MAX_PATH))
    {
        std::wstring exeDir = exePath;
        pos = exeDir.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
            exeDir = exeDir.substr(0, pos);
        preferred = exeDir + L"\\" + kDriverFileName;
        if (GetFileAttributesW(preferred.c_str()) != INVALID_FILE_ATTRIBUTES)
            return preferred;
    }

    // Fallback: search for any .sys in the same directory
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW((dir + L"\\*.sys").c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        std::wstring result = dir + L"\\" + findData.cFileName;
        FindClose(hFind);
        return result;
    }

    return {};
}

// ---------------------------------------------------------------------------
// GTA process waiting (console confirmation happens in the loader)
// ---------------------------------------------------------------------------

static void WaitForGtaThread()
{
    PRINT_GRAY("waiting for GTA5.exe / FiveM...");

    while (g_running && !g_gtaDetected)
    {
        const DWORD pid = FindGamePid();
        if (pid)
        {
            g_gtaDetected = true;
            PRINT_GREEN("game process detected (pid " + std::to_string(pid) + ")");
            break;
        }
        Sleep(1000);
    }
}

// ---------------------------------------------------------------------------
// Driver mapping (synchronous, called by the loader)
// ---------------------------------------------------------------------------

static bool MapDriverNow()
{
    if (IsDriverLoaded())
    {
        PRINT_GREEN("driver already loaded!");
        return true;
    }

    const std::wstring driverPath = FindDriverFile();
    if (driverPath.empty())
    {
        PRINT_RED("driver file not found!");
        return false;
    }

    if (!IsElevated())
    {
        PRINT_RED("administrator privileges required!");
        return false;
    }

    PRINT_GRAY("loading vulnerable intel driver...");
    const NTSTATUS intelStatus = intel_driver::Load();
    if (!NT_SUCCESS(intelStatus))
    {
        PRINT_RED("failed to load intel driver: 0x" + ToUtf8(std::to_wstring(intelStatus)));
        intel_driver::hDevice = 0;
        return false;
    }

    std::vector<BYTE> rawImage = ReadFileToMemory(driverPath);
    if (rawImage.empty())
    {
        PRINT_RED("failed to read driver image");
        intel_driver::Unload();
        intel_driver::hDevice = 0;
        return false;
    }

    PRINT_GRAY("mapping " + ToUtf8(driverPath) + "...");

    NTSTATUS entryStatus = STATUS_SUCCESS;
    const bool mapped = kdmapper::MapDriver(rawImage.data(), 0, 0,
        true, true,
        kdmapper::AllocationMode::AllocatePool,
        false, nullptr, &entryStatus);

    const NTSTATUS unloadStatus = intel_driver::Unload();
    intel_driver::hDevice = 0;

    if (!NT_SUCCESS(unloadStatus))
        PRINT_YELLOW("failed to fully unload intel driver");

    if (mapped && NT_SUCCESS(entryStatus) && IsDriverLoaded())
    {
        PRINT_GREEN("driver mapped successfully! device \\\\.\\Evolution is online");
        return true;
    }

    PRINT_RED("driver mapping failed!");
    return false;
}

// ---------------------------------------------------------------------------
// DLL injection into the game process
// ---------------------------------------------------------------------------

// True when the module is present in the target process (best-effort - the
// snapshot may fail on protected processes, in which case we just don't know).
static bool IsModuleLoadedInProcess(DWORD pid, const wchar_t* moduleName, bool* snapshotOk)
{
    if (snapshotOk)
        *snapshotOk = false;

    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    if (snapshotOk)
        *snapshotOk = true;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    bool found = false;
    if (Module32FirstW(snap, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szModule, moduleName) == 0)
            {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &entry));
    }

    CloseHandle(snap);
    return found;
}

static bool WaitForModuleInProcess(DWORD pid, const wchar_t* moduleName, DWORD timeoutMs, bool* snapshotOk)
{
    const DWORD start = GetTickCount();
    do
    {
        if (IsModuleLoadedInProcess(pid, moduleName, snapshotOk))
            return true;
        Sleep(250);
    } while (GetTickCount() - start < timeoutMs);

    return false;
}

// 1) classic CreateRemoteThread + LoadLibraryW
static bool InjectUsermode(DWORD pid, LPTHREAD_START_ROUTINE loadLibraryW, const wchar_t* dllPath, bool* blocked)
{
    if (blocked)
        *blocked = false;

    const HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!process)
    {
        PRINT_RED("failed to open the game process (run the loader as admin)");
        return false;
    }

    bool ok = false;
    const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);

    if (void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
    {
        if (WriteProcessMemory(process, remote, dllPath, bytes, nullptr))
        {
            if (HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibraryW, remote, 0, nullptr))
            {
                WaitForSingleObject(thread, 20000);

                DWORD exitCode = 0;
                GetExitCodeThread(thread, &exitCode);
                CloseHandle(thread);

                if (exitCode)
                {
                    ok = true;
                }
                else
                {
                    PRINT_RED("LoadLibraryW failed inside the game (dependencies?)");
                }
            }
            else
            {
                PRINT_YELLOW("CreateRemoteThread failed: " + std::to_string(GetLastError()));
                if (blocked) *blocked = true;
            }
        }
        else
        {
            PRINT_YELLOW("WriteProcessMemory failed: " + std::to_string(GetLastError()));
            if (blocked) *blocked = true;
        }

        if (!ok)
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    else
    {
        PRINT_YELLOW("VirtualAllocEx failed: " + std::to_string(GetLastError()));
        if (blocked) *blocked = true;
    }

    CloseHandle(process);
    return ok;
}

// ntdll!NtCreateThreadEx - the same syscall CreateRemoteThread uses
// internally, called directly so the thread can start at LoadLibraryW (an
// image address) without allocating executable memory.
typedef NTSTATUS(NTAPI* NtCreateThreadEx_t)(
    PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, PVOID ObjectAttributes,
    HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument,
    ULONG CreateFlags, SIZE_T ZeroBits, SIZE_T StackSize,
    SIZE_T MaximumStackSize, PVOID AttributeList);

// 2) kernel fallback - the driver ALLOCATES the path inside the game (this is
//    what bypasses the blocked VirtualAllocEx), then the thread is started
//    from user mode via ntdll!NtCreateThreadEx at LoadLibraryW. Kernel-side
//    thread creation is impossible: ZwCreateThreadEx is NOT exported by
//    ntoskrnl (going through a null pointer there was the old BSOD).
static bool InjectKernel(DWORD pid, LPTHREAD_START_ROUTINE loadLibraryW, const wchar_t* dllPath)
{
    if (!Driver::InitDriver())
    {
        PRINT_RED("driver is not available for kernel injection");
        return false;
    }

    // A build with an older IOCTL protocol may still be resident in the kernel
    // (kdmapper drivers live until reboot) - never talk to it, it may crash.
    const int version = Driver::GetVersion();
    if (version != Driver::EVOLUTION_VERSION)
    {
        PRINT_RED("old evolution.sys is still resident in the kernel (v" +
            std::to_string(version) + ", need v" + std::to_string(Driver::EVOLUTION_VERSION) +
            ") - REBOOT, then run the loader again");
        return false;
    }

    const HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
        PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!process)
    {
        PRINT_RED("failed to open the game process for kernel injection");
        return false;
    }

    uintptr_t remotePath = 0;
    if (!Driver::AllocateRemotePath(pid, reinterpret_cast<uintptr_t>(process), dllPath, remotePath))
    {
        PRINT_RED("kernel path allocation failed");
        CloseHandle(process);
        return false;
    }

    const auto NtCreateThreadEx = reinterpret_cast<NtCreateThreadEx_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateThreadEx"));
    if (!NtCreateThreadEx)
    {
        PRINT_RED("ntdll!NtCreateThreadEx not found");
        CloseHandle(process);
        return false;
    }

    HANDLE thread = nullptr;
    const NTSTATUS status = NtCreateThreadEx(&thread, THREAD_ALL_ACCESS, nullptr,
        process, reinterpret_cast<PVOID>(loadLibraryW),
        reinterpret_cast<PVOID>(remotePath),
        0, 0, 0, 0, nullptr);

    if (status < 0 || !thread)
    {
        char hex[16] = {};
        snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned int>(status));
        PRINT_RED(std::string("NtCreateThreadEx failed: ") + hex);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 20000);

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    CloseHandle(process);

    if (!exitCode)
    {
        PRINT_RED("LoadLibraryW returned NULL inside the game");
        return false;
    }

    return true;
}

static bool InjectNow()
{
    const DWORD pid = FindGamePid();
    if (!pid)
    {
        PRINT_RED("game process not found!");
        return false;
    }

    HMODULE self = nullptr;
    wchar_t dllPath[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&MapDriverNow), &self) ||
        !GetModuleFileNameW(self, dllPath, MAX_PATH))
    {
        PRINT_RED("failed to resolve own module path");
        return false;
    }

    const auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLibraryW)
    {
        PRINT_RED("failed to resolve LoadLibraryW");
        return false;
    }

    // Already inside the game? (e.g. manual start for testing)
    bool snapshotOk = false;
    if (IsModuleLoadedInProcess(pid, L"evolution_gui.dll", &snapshotOk))
    {
        PRINT_GREEN("evolution_gui.dll is already loaded in the game!");
        return true;
    }

    // 1) classic user mode injection
    PRINT_GRAY("trying classic injection...");
    bool blocked = false;
    if (InjectUsermode(pid, loadLibraryW, dllPath, &blocked))
    {
        PRINT_GREEN("injected into pid " + std::to_string(pid) + "!");
        return true;
    }

    // 2) kernel mode fallback (game blocks user mode injection)
    PRINT_YELLOW("classic injection blocked (" + std::to_string(GetLastError()) +
        "), trying kernel injection...");
    const bool kernelOk = InjectKernel(pid, loadLibraryW, dllPath);

    // Verify: wait for the module to appear in the game process.
    const bool loaded = WaitForModuleInProcess(pid, L"evolution_gui.dll", 15000, &snapshotOk);

    if (loaded)
    {
        PRINT_GREEN("injected into pid " + std::to_string(pid) + " (" +
            std::string(kernelOk ? "kernel" : "usermode") + ")!");
        return true;
    }

    if (!snapshotOk)
    {
        PRINT_YELLOW("could not verify the injection (module snapshot denied) - check the game");
        return true;
    }

    PRINT_RED("injection failed - evolution_gui.dll never appeared in the game");
    return false;
}

// ---------------------------------------------------------------------------
// Exported functions
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) bool EvolutionGui_Init()
{
    if (g_running)
        return true;

    InitConsole();
    PRINT_CYAN("Evolution GUI DLL initialized");

    g_running = true;

    // Start the GTA waiter thread
    g_waiterThread = std::thread(WaitForGtaThread);

    return true;
}

extern "C" __declspec(dllexport) bool EvolutionGui_WaitForGTA()
{
    // Block until GTA is detected or timeout
    int timeout = 300; // 5 minutes
    while (g_running && !g_gtaDetected && timeout > 0)
    {
        Sleep(1000);
        timeout--;
    }

    return g_gtaDetected;
}

extern "C" __declspec(dllexport) bool EvolutionGui_MapDriver()
{
    return MapDriverNow();
}

extern "C" __declspec(dllexport) bool EvolutionGui_Inject()
{
    return InjectNow();
}

extern "C" __declspec(dllexport) void EvolutionGui_Shutdown()
{
    g_running = false;
    g_gtaDetected = false;

    if (g_waiterThread.joinable())
        g_waiterThread.join();

    PRINT_CYAN("Evolution GUI DLL shutdown");
}

// ---------------------------------------------------------------------------
// DLL entry point
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // Inside the game process -> bring up the in-game menu / ESP right away.
        if (IsGameProcess())
            present_hook::start(hModule);
        break;
    case DLL_PROCESS_DETACH:
        // Real FreeLibrary only (lpReserved == NULL); process termination needs
        // no cleanup - the present hook already unloads itself beforehand.
        if (lpReserved == nullptr && g_running)
            EvolutionGui_Shutdown();
        break;
    default:
        break;
    }

    return TRUE;
}
