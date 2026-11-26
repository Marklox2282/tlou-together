// Проверка обновления без игры: updatetest.exe <base url> <папка> <ожидаемый результат 0|1|2>
// В папке лежит "winmm.dll" (будто бы загруженный мод). 0 = UpToDate, 1 = Installed, 2 = Failed.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/update.hpp"

static void Log(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

int wmain(int argc, wchar_t** argv) {
	if (argc != 5) return 3;
	const uint32_t current = static_cast<uint32_t>(_wtoi(argv[3]));
	// Как в игре: файл мода спроецирован в процесс и заблокирован от перезаписи.
	LoadLibraryExW((std::wstring(argv[2]) + L"\\winmm.dll").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
	const auto r = update::Run(current, std::wstring(argv[2]) + L"\\winmm.dll", argv[1], Log);
	const int want = _wtoi(argv[4]);
	printf("result %d, expected %d\n", static_cast<int>(r), want);
	return static_cast<int>(r) == want ? 0 : 1;
}
