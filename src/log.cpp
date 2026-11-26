#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>

#include "log.hpp"

namespace {
FILE* g_log = nullptr;
}

std::wstring GameDirFile(const wchar_t* name) {
	wchar_t path[MAX_PATH];
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	wchar_t* slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
	if (!slash) return {};
	slash[1] = 0;
	return wcscat_s(path, name) ? std::wstring() : std::wstring(path);
}

void OpenLog() {
	const std::wstring path = GameDirFile(L"tlou1-coop.log");
	if (!path.empty()) g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...) {
	if (!g_log) return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	// Одним вызовом: пишут несколько потоков (кадр, скрипты, обновление).
	fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
	fflush(g_log);
}
