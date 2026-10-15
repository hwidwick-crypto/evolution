#pragma once

#include <ntifs.h>
#include <ntddk.h>
#include <ntimage.h>
#include <minwindef.h>

#include <intrin.h>
#include <cstdint>

#define printf(fmt, ...) DbgPrintEx(0, 0, "[evolution] " fmt, ##__VA_ARGS__)

// Expects plain `status` (NTSTATUS) and `safeData` in scope; `break` leaves the
// surrounding switch so the caller can complete the IRP with the final status.
#define HANDLE_REQUEST(name, args) \
	case RequestType##name: {                                     \
		args safe = { 0 };                                        \
		if (!SafeCopy(&safe, safeData.Arguments, sizeof(args))) { \
			status = STATUS_ACCESS_VIOLATION;                     \
			break;                                                \
		}                                                         \
		status = Core##name(&safe);                               \
		break;                                                    \
	}

#include "kli.hpp"
#include "evolution.h"
#include "util.h"
