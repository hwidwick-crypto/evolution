#pragma once
#include <windows.h>

namespace Utils {
    class Process {
    public:
        static DWORD GetProcessId(const wchar_t* processName);
    };
}
