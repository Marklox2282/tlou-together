// Загружает собранную winmm.dll вне игры: прокси должен работать, мод - молчать.
#include <windows.h>
#include <cstdio>
#include <initializer_list>

int main(int argc, char** argv) {
	if (argc != 2) return 1;
	HMODULE m = LoadLibraryA(argv[1]);
	if (!m) return printf("load failed %lu\n", GetLastError()), 2;
	for (const char* n : { "timeGetTime", "timeBeginPeriod", "timeEndPeriod", "timeGetDevCaps" })
		if (!GetProcAddress(m, n)) return printf("no export %s\n", n), 3;
	auto time = reinterpret_cast<DWORD(WINAPI*)()>(GetProcAddress(m, "timeGetTime"));
	const DWORD a = time();
	Sleep(200);
	const DWORD elapsed = time() - a;
	printf("elapsed=%lu\n", elapsed);
	return elapsed >= 150 && elapsed < 1000 ? 0 : 4;
}
