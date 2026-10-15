#include "cleaner.hpp"
#include <windows.h>

namespace Utils {
    void Cleaner::SanitizeRegistryTraces() {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\iqvw64e");
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\intel_driver");
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\monitorinspect");
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\bam\\UserSettings");
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\dam\\UserSettings");
    }
}
