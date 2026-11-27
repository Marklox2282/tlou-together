// Ручная проверка отправки логов: logsendtest.exe <адрес вебхука>. Шлёт короткий тестовый файл.
// Адрес - только аргументом: в репозитории его нет.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

#include "../src/log.hpp"
#include "../src/logsend.hpp"

int main(int argc, char** argv) {
	if (argc < 2) {
		puts("usage: logsendtest <webhook url>");
		return 2;
	}
	if (FILE* f = _wfopen(GameDirFile(L"tlou-together.log").c_str(), L"w")) {
		fputs("tlou-together log upload test\n", f);
		fclose(f);
	}
	OpenLog();  // старый лог станет .prev.log - его и отправим
	logsend::SetWebhook(argv[1]);
	if (!logsend::Enabled()) {
		puts("not a Discord webhook address");
		return 1;
	}
	logsend::SendPreviousSession(0);
	Sleep(8000);  // отправка идёт в своём потоке
	puts("done - see tlou-together.log next to this exe for the HTTP code");
	return 0;
}
