//
// Evolution loader
//
// Console loader that maps the Evolution kernel driver with kdmapper.
// kdmapper sources are compiled directly into this project (KDLIBMODE + DISABLE_OUTPUT)
// so every console line and its color is fully controlled here.
//

#include <Windows.h>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <filesystem>

#include <kdmapper.hpp>
#include <utils.hpp>
#include <intel_driver.hpp>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Driver file that must be located next to the loader (fallback: any *.sys next to it)
static const wchar_t* const kDriverFileName = L"evolution.sys";

// Device created by the driver inside DriverEntry (IoCreateDevice + IoCreateSymbolicLink)
// Used to detect that the driver is already loaded.
static const wchar_t* const kDevicePath = L"\\\\.\\Evolution";

// Service name, used as a fallback detection path if the driver is installed as a service
static const wchar_t* const kServiceName = L"Evolution";

// ---------------------------------------------------------------------------
// Console helpers
// ---------------------------------------------------------------------------

namespace ui
{
	static constexpr WORD CLR_DEFAULT = 7;
	static constexpr WORD CLR_GRAY    = 8;
	static constexpr WORD CLR_GREEN   = 10;
	static constexpr WORD CLR_CYAN    = 11;
	static constexpr WORD CLR_RED     = 12;
	static constexpr WORD CLR_YELLOW  = 14;
	static constexpr WORD CLR_WHITE   = 15;

	static HANDLE Out() { return GetStdHandle(STD_OUTPUT_HANDLE); }

	static void SetColor(WORD color)
	{
		SetConsoleTextAttribute(Out(), color);
	}

	static void Line(WORD color, const std::string& text)
	{
		SetColor(color);
		std::cout << text << std::endl;
		SetColor(CLR_DEFAULT);
	}

	static void Clear()
	{
		const HANDLE out = Out();
		CONSOLE_SCREEN_BUFFER_INFO info = {};
		if (!GetConsoleScreenBufferInfo(out, &info))
			return;

		const COORD home = { 0, 0 };
		const DWORD cells = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
		DWORD written = 0;
		FillConsoleOutputCharacterW(out, L' ', cells, home, &written);
		FillConsoleOutputAttribute(out, info.wAttributes, cells, home, &written);
		SetConsoleCursorPosition(out, home);
	}
}

// ---------------------------------------------------------------------------
// "EVOLUTION" logo, drawn with a 5x5 pixel font
// ---------------------------------------------------------------------------

namespace logo
{
	struct Glyph
	{
		char ch;
		const char* rows[5];
	};

	static const Glyph kGlyphs[] = {
		{ 'E', { "#####", "#    ", "#### ", "#    ", "#####" } },
		{ 'V', { "#   #", "#   #", "#   #", " # # ", "  #  " } },
		{ 'O', { " ### ", "#   #", "#   #", "#   #", " ### " } },
		{ 'L', { "#    ", "#    ", "#    ", "#    ", "#####" } },
		{ 'U', { "#   #", "#   #", "#   #", "#   #", " ### " } },
		{ 'T', { "#####", "  #  ", "  #  ", "  #  ", "  #  " } },
		{ 'I', { "#####", "  #  ", "  #  ", "  #  ", "#####" } },
		{ 'N', { "#   #", "##  #", "# # #", "#  ##", "#   #" } },
	};

	static const char* const kWord = "EVOLUTION";

	static const Glyph* Find(char c)
	{
		for (const Glyph& g : kGlyphs)
			if (g.ch == c)
				return &g;
		return nullptr;
	}

	static void Print()
	{
		for (int row = 0; row < 5; ++row)
		{
			std::string line;
			for (const char* p = kWord; *p; ++p)
			{
				const Glyph* g = Find(*p);
				line += (g ? g->rows[row] : "     ");
				if (*(p + 1))
					line += ' ';
			}
			ui::Line(ui::CLR_CYAN, line);
		}
	}
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

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

static std::string Trim(const std::string& text)
{
	const char* spaces = " \t\r\n";
	const size_t first = text.find_first_not_of(spaces);
	if (first == std::string::npos)
		return {};
	return text.substr(first, text.find_last_not_of(spaces) - first + 1);
}

static std::string HexStatus(NTSTATUS status)
{
	std::ostringstream oss;
	oss << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
		<< static_cast<unsigned long>(status);
	return oss.str();
}

static bool IsElevated()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;

	TOKEN_ELEVATION elevation = {};
	DWORD size = 0;
	const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
	CloseHandle(token);
	return ok && elevation.TokenIsElevated;
}

// ---------------------------------------------------------------------------
// Driver detection
// ---------------------------------------------------------------------------

static bool IsDriverLoaded()
{
	// 1) main path - communication device created by the driver itself
	const HANDLE device = CreateFileW(kDevicePath, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

	if (device && device != INVALID_HANDLE_VALUE)
	{
		CloseHandle(device);
		return true;
	}

	// any error other than "not found" means the device object exists (access denied, etc.)
	if (GetLastError() != ERROR_FILE_NOT_FOUND)
		return true;

	// 2) fallback - driver installed as a regular service
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
// Mapping
// ---------------------------------------------------------------------------

struct MapOutcome
{
	bool ok = false;
	std::string reason;
	std::vector<std::string> hints;
};

static std::string DescribeNtStatus(NTSTATUS status)
{
	switch (status)
	{
	case STATUS_ALREADY_REGISTERED:            return "intel driver is already loaded - close other mapper instance or reboot";
	case STATUS_OBJECT_NAME_COLLISION:         return "the driver is already registered in the kernel (\\Driver\\Evolution) - it is loaded";
	case STATUS_NOT_ALL_ASSIGNED:              return "required privileges are missing - run the loader as administrator";
	case STATUS_PRIVILEGE_NOT_HELD:            return "SeLoadDriverPrivilege is missing - run the loader as administrator";
	case STATUS_REGISTRY_IO_FAILED:            return "failed to write the intel driver service key - run as administrator";
	case STATUS_DISK_OPERATION_FAILED:         return "failed to create the intel driver file in %temp%";
	case STATUS_IMAGE_CERT_REVOKED:            return "vulnerable driver blocklist is enabled - disable it";
	case STATUS_INVALID_IMAGE_HASH:            return "driver signature rejected by memory integrity";
	case STATUS_SYSTEM_INTEGRITY_POLICY_VIOLATION: return "system integrity policy blocks the driver";
	case STATUS_ACCESS_DENIED:                 return "access denied - antivirus or anticheat is blocking the load";
	case STATUS_INSUFFICIENT_RESOURCES:        return "kernel refused to load the driver - antivirus or anticheat may be blocking";
	case STATUS_UNSUCCESSFUL:                  return "failed to prepare the vulnerable intel driver";
	default:                                   return "ntstatus " + HexStatus(status);
	}
}

static fs::path FindDriverFile()
{
	wchar_t buffer[MAX_PATH] = {};
	if (!GetModuleFileNameW(nullptr, buffer, MAX_PATH))
		return {};
	const fs::path dir = fs::path(buffer).parent_path();

	std::error_code ec;
	const fs::path preferred = dir / kDriverFileName;
	if (fs::exists(preferred, ec))
		return preferred;

	// fallback: first *.sys file located next to the loader
	fs::directory_iterator it(dir, ec);
	if (ec)
		return {};
	const fs::directory_iterator end;
	for (; it != end; it.increment(ec))
	{
		if (ec)
			break;
		std::error_code typeEc;
		if (!it->is_regular_file(typeEc))
			continue;
		if (it->path().extension() == L".sys")
			return it->path();
	}
	return {};
}

static MapOutcome MapKernelDriver()
{
	static const std::vector<std::string> kStdHints = {
		"make sure core isolation is disabled & game is closed"
	};

	MapOutcome out;
	const auto fail = [&out](const std::string& reason, const std::vector<std::string>& hints) {
		out.ok = false;
		out.reason = reason;
		out.hints = hints;
		return out;
	};

	const fs::path driverPath = FindDriverFile();
	if (driverPath.empty())
		return fail(std::string("driver file '") + ToUtf8(kDriverFileName) + "' not found next to the loader",
			{ "put the driver file next to Evolution.exe" });

	if (!IsElevated())
		return fail("administrator privileges are required",
			{ "restart the loader as administrator" });

	// vulnerable intel driver
	ui::Line(ui::CLR_GRAY, "[~] loading vulnerable intel driver...");
	const NTSTATUS intelStatus = intel_driver::Load();
	if (!NT_SUCCESS(intelStatus))
	{
		// Load() may have unloaded the driver internally and left a stale handle behind
		intel_driver::hDevice = 0;
		return fail("cannot load intel driver: " + DescribeNtStatus(intelStatus),
			{ "make sure core isolation is disabled & game is closed",
			  "close antivirus / anticheat and retry" });
	}

	// read the image
	std::vector<BYTE> rawImage;
	if (!kdmUtils::ReadFileToMemory(driverPath.wstring(), &rawImage))
	{
		intel_driver::Unload();
		intel_driver::hDevice = 0;
		return fail("failed to read the driver image",
			{ "check that the driver file is not corrupted" });
	}

	// map it
	ui::Line(ui::CLR_GRAY, std::string("[~] mapping ") + ToUtf8(driverPath.filename().wstring()) + "...");
	NTSTATUS entryStatus = STATUS_SUCCESS;
	const bool mapped = kdmapper::MapDriver(rawImage.data(), 0, 0,
		false /* keep memory after DriverEntry - driver stays in kernel */,
		true /* destroy image header */,
		kdmapper::AllocationMode::AllocatePool,
		false /* do not pass allocation ptr */,
		nullptr /* no callback */,
		&entryStatus);

	const NTSTATUS unloadStatus = intel_driver::Unload();
	intel_driver::hDevice = 0;
	if (!NT_SUCCESS(unloadStatus))
		ui::Line(ui::CLR_YELLOW, "[!] failed to fully unload the vulnerable intel driver");

	if (!mapped)
		return fail("kdmapper could not map the image", kStdHints);

	if (!NT_SUCCESS(entryStatus))
		return fail("driver entry returned " + DescribeNtStatus(entryStatus), kStdHints);

	out.ok = true;
	return out;
}

// ---------------------------------------------------------------------------
// Menu actions
// ---------------------------------------------------------------------------

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
	std::string detail;
	if (info && info->ExceptionRecord)
	{
		std::ostringstream oss;
		oss << "addr 0x" << std::hex << reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)
			<< " code 0x" << info->ExceptionRecord->ExceptionCode;
		detail = oss.str();
	}

	ui::Line(ui::CLR_RED, std::string("[!] unexpected crash (") + detail + ")");
	ui::Line(ui::CLR_YELLOW, "make sure core isolation is disabled & game is closed");

	if (intel_driver::hDevice)
		intel_driver::Unload();

	std::cout << std::endl;
	ui::Line(ui::CLR_GRAY, "press enter to close...");
	std::string dummy;
	std::getline(std::cin, dummy);
	return EXCEPTION_EXECUTE_HANDLER;
}

static void Start()
{
	ui::Line(ui::CLR_GRAY, "[~] checking driver...");

	if (IsDriverLoaded())
	{
		ui::Line(ui::CLR_GREEN, "[+] driver is already loaded - mapping skipped");
		return;
	}

	ui::Line(ui::CLR_GRAY, "[~] driver not loaded, mapping...");

	const MapOutcome outcome = MapKernelDriver();
	if (outcome.ok)
	{
		ui::Line(ui::CLR_GREEN, "[+] driver mapped successfully");

		// make sure the marker device really showed up before calling it ready
		if (IsDriverLoaded())
			ui::Line(ui::CLR_GREEN, "[+] driver ready! device \\\\.\\Evolution is online");
		else
			ui::Line(ui::CLR_YELLOW, "[!] mapped, but \\\\.\\Evolution did not appear");

		return;
	}

	ui::Line(ui::CLR_RED, "[!] driver mapping failed");
	ui::Line(ui::CLR_YELLOW, outcome.reason);
	for (const std::string& hint : outcome.hints)
		ui::Line(ui::CLR_YELLOW, hint);
}

static void WaitForEnter()
{
	std::cout << std::endl;
	ui::Line(ui::CLR_GRAY, "press enter to return to the menu...");
	std::string dummy;
	std::getline(std::cin, dummy);
}

int main()
{
	SetUnhandledExceptionFilter(CrashHandler);
	SetConsoleTitleW(L"Evolution Loader");

	bool running = true;
	bool firstFrame = true;

	while (running)
	{
		if (!firstFrame)
			ui::Clear();
		firstFrame = false;

		logo::Print();
		std::cout << std::endl;
		ui::Line(ui::CLR_GRAY, "        evolution | kernel loader");
		std::cout << std::endl;
		ui::Line(ui::CLR_WHITE, "1 - Start");
		ui::Line(ui::CLR_WHITE, "2 - Exit");
		std::cout << std::endl << "> " << std::flush;

		std::string choice;
		if (!std::getline(std::cin, choice))
			break;
		choice = Trim(choice);

		if (choice == "1")
		{
			std::cout << std::endl;
			Start();
			WaitForEnter();
		}
		else if (choice == "2")
		{
			ui::Line(ui::CLR_GRAY, "[~] bye");
			running = false;
		}
		else
		{
			ui::Line(ui::CLR_YELLOW, "[!] unknown option, use 1 or 2");
			WaitForEnter();
		}
	}

	return 0;
}
