#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "version.hpp"
#include "log.hpp"
#include "logsend.hpp"

namespace logsend {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::string g_url;  // под g_lock
std::atomic<bool> g_busy{ false };
constexpr size_t kMaxFile = 7u << 20;  // Discord принимает до ~8 МБ на файл - берём хвост

struct Attachment { std::string name; std::string data; };

// Весь файл, даже если его держит открытым наш же лог (только чтение, с общим доступом на запись).
bool ReadShared(const std::wstring& path, std::string& out) {
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0;
	if (ok) {
		const uint64_t total = static_cast<uint64_t>(size.QuadPart);
		const uint64_t take = total > kMaxFile ? kMaxFile : total;
		LARGE_INTEGER from{};
		from.QuadPart = static_cast<LONGLONG>(total - take);
		out.resize(static_cast<size_t>(take));
		DWORD read = 0;
		ok = SetFilePointerEx(h, from, nullptr, FILE_BEGIN) && ReadFile(h, out.data(), static_cast<DWORD>(take), &read, nullptr);
		out.resize(read);
	}
	CloseHandle(h);
	return ok && !out.empty();
}

std::wstring Widen(const std::string& s) {
	std::wstring w(s.size(), L'\0');
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
	w.resize(n > 0 ? n : 0);
	return w;
}

// POST multipart/form-data: текст сообщения и файлы. Возвращает HTTP-код (0 - не дошло).
DWORD Post(const std::string& url, const std::string& message, const std::vector<Attachment>& files) {
	const std::wstring wurl = Widen(url);
	URL_COMPONENTS parts{};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256]{}, path[1024]{};
	parts.lpszHostName = host;
	parts.dwHostNameLength = 256;
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = 1024;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return 0;

	const std::string boundary = "----tlou-together-logs-7f3a9c";
	std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"content\"\r\n\r\n" + message + "\r\n";
	for (size_t i = 0; i < files.size(); ++i) {
		body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"files[" + std::to_string(i) + "]\"; filename=\"" +
			files[i].name + "\"\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n";
		body += files[i].data;
		body += "\r\n";
	}
	body += "--" + boundary + "--\r\n";
	const std::wstring header = L"Content-Type: multipart/form-data; boundary=" + Widen(boundary);

	DWORD status = 0;
	HINTERNET session = WinHttpOpen(L"tlou-together-logs", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
	HINTERNET conn = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
	HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
	if (req) {
		WinHttpSetTimeouts(req, 10000, 10000, 30000, 30000);
		DWORD size = sizeof(status);
		if (!WinHttpSendRequest(req, header.c_str(), static_cast<DWORD>(-1), body.data(), static_cast<DWORD>(body.size()),
				static_cast<DWORD>(body.size()), 0) ||
			!WinHttpReceiveResponse(req, nullptr) ||
			!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr))
			status = 0;
	}
	for (HINTERNET h : { req, conn, session })
		if (h) WinHttpCloseHandle(h);
	return status;
}

std::string Url() {
	AcquireSRWLockShared(&g_lock);
	std::string u = g_url;
	ReleaseSRWLockShared(&g_lock);
	return u;
}

// Отправка в своём потоке: сеть не должна держать игру.
void SendAsync(std::string message, std::vector<std::pair<std::wstring, std::string>> paths, std::wstring stampAfter) {
	const std::string url = Url();
	if (url.empty() || g_busy.exchange(true)) return;
	std::thread([url, message = std::move(message), paths = std::move(paths), stampAfter = std::move(stampAfter)] {
		std::vector<Attachment> files;
		for (const auto& [path, name] : paths) {
			Attachment a{ name, {} };
			if (ReadShared(path, a.data)) files.push_back(std::move(a));
		}
		if (!files.empty()) {
			const DWORD status = Post(url, message, files);
			Log("logs: sent %zu file(s) to the webhook - HTTP %lu", files.size(), status);
			if (status >= 200 && status < 300 && !stampAfter.empty())
				if (FILE* f = nullptr; _wfopen_s(&f, stampAfter.c_str(), L"w") == 0 && f) fclose(f);  // время отправки файла падений
		}
		g_busy = false;
	}).detach();
}

// Файл падений новее прошлой отправки?
bool Newer(const std::wstring& file, const std::wstring& stamp) {
	WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
	if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a) || (a.nFileSizeHigh == 0 && a.nFileSizeLow == 0)) return false;
	if (!GetFileAttributesExW(stamp.c_str(), GetFileExInfoStandard, &b)) return true;
	return CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) > 0;
}

}  // namespace

void SetWebhook(const std::string& url) {
	const bool ok = url.rfind("https://discord.com/api/webhooks/", 0) == 0 || url.rfind("https://discordapp.com/api/webhooks/", 0) == 0;
	AcquireSRWLockExclusive(&g_lock);
	g_url = ok ? url : std::string();
	ReleaseSRWLockExclusive(&g_lock);
	if (!url.empty() && url != "off" && !ok) Log("logs: log_webhook is not a Discord webhook address - not sending");
}

bool Enabled() { return !Url().empty(); }
std::string Webhook() { return Url(); }

void SendPreviousSession(unsigned modVersion) {
	if (!Enabled()) return;
	const std::wstring prev = GameDirFile(L"tlou-together.prev.log"), crash = GameDirFile(L"tlou-together-crash.log"),
					   stamp = GameDirFile(L"tlou-together.logs-sent");
	std::vector<std::pair<std::wstring, std::string>> paths{ { prev, "tlou-together.prev.log" } };
	const bool crashed = Newer(crash, stamp);
	if (crashed) paths.push_back({ crash, "tlou-together-crash.log" });
	char msg[160];
	snprintf(msg, sizeof(msg), "tlou-together %s - previous session%s", version::Name(modVersion).c_str(), crashed ? " (CRASH)" : "");
	SendAsync(msg, std::move(paths), stamp);
}

void SendNow(unsigned modVersion, const std::string& note) {
	if (!Enabled()) return;
	char msg[200];
	snprintf(msg, sizeof(msg), "tlou-together %s - %s", version::Name(modVersion).c_str(), note.c_str());
	SendAsync(msg, { { GameDirFile(L"tlou-together.log"), "tlou-together.log" }, { GameDirFile(L"tlou-together-crash.log"), "tlou-together-crash.log" } },
		std::wstring());
}

}  // namespace logsend
