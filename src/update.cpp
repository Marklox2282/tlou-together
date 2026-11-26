#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <cstdlib>
#include <fstream>

#include "signing.hpp"
#include "update.hpp"
#include "update_key.hpp"

namespace update {

namespace {

constexpr size_t kMaxDownload = 16u << 20;

// GET целиком в память; редиректы GitHub (на objects.githubusercontent.com) WinHTTP проходит сам.
bool Get(const std::wstring& url, signing::Bytes& out, size_t limit) {
	URL_COMPONENTS parts{};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256]{}, path[2048]{};
	parts.lpszHostName = host;
	parts.dwHostNameLength = ARRAYSIZE(host);
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = ARRAYSIZE(path);
	if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;
	const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;
	HINTERNET session = WinHttpOpen(L"tlou-together-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
	HINTERNET conn = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
	HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, nullptr, nullptr, https ? WINHTTP_FLAG_SECURE : 0) : nullptr;
	bool ok = false;
	if (req) {
		WinHttpSetTimeouts(req, 10000, 10000, 15000, 30000);
		DWORD status = 0, size = sizeof(status);
		if (WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr) &&
			WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr) &&
			status == 200) {
			out.clear();
			ok = true;
			for (DWORD avail = 0; ok && WinHttpQueryDataAvailable(req, &avail) && avail;) {
				const size_t at = out.size();
				if (at + avail > limit) { ok = false; break; }
				out.resize(at + avail);
				DWORD read = 0;
				ok = WinHttpReadData(req, out.data() + at, avail, &read) != FALSE;
				out.resize(at + read);
			}
		}
	}
	for (HINTERNET h : { req, conn, session })
		if (h) WinHttpCloseHandle(h);
	return ok;
}

}  // namespace

void CleanupOld(const std::wstring& dllPath) {
	DeleteFileW((dllPath + L".old").c_str());
	DeleteFileW((dllPath + L".new").c_str());
}

Result Run(uint32_t currentVersion, const std::wstring& dllPath, const std::wstring& baseUrl, LogFn log) {
	signing::Bytes text, dll, sig;
	if (!Get(baseUrl + L"version.txt", text, 64)) {
		log("update: can't check for updates");
		return Result::Failed;
	}
	text.push_back(0);
	const uint32_t latest = static_cast<uint32_t>(strtoul(reinterpret_cast<const char*>(text.data()), nullptr, 10));
	if (latest <= currentVersion) {
		log("update: v%u is the latest", currentVersion);
		return Result::UpToDate;
	}
	log("update: v%u available (have v%u), downloading", latest, currentVersion);
	if (!Get(baseUrl + L"winmm.dll", dll, kMaxDownload) || !Get(baseUrl + L"winmm.dll.sig", sig, 256)) {
		log("update: download failed");
		return Result::Failed;
	}
	const signing::Bytes key(std::begin(kUpdatePublicKey), std::end(kUpdatePublicKey));
	if (dll.size() < 2 || dll[0] != 'M' || dll[1] != 'Z' || !signing::Verify(key, dll, latest, sig)) {
		log("update: v%u REJECTED - signature does not match, not installed", latest);
		return Result::Failed;
	}
	const std::wstring fresh = dllPath + L".new", old = dllPath + L".old";
	{
		std::ofstream f(fresh, std::ios::binary);
		f.write(reinterpret_cast<const char*>(dll.data()), static_cast<std::streamsize>(dll.size()));
		if (!f) {
			log("update: can't write %ls", fresh.c_str());
			return Result::Failed;
		}
	}
	if (!MoveFileExW(dllPath.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		log("update: can't move the running mod aside (error %lu)", GetLastError());
		DeleteFileW(fresh.c_str());
		return Result::Failed;
	}
	if (!MoveFileExW(fresh.c_str(), dllPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		log("update: can't put v%u in place (error %lu), keeping the current version", latest, GetLastError());
		MoveFileExW(old.c_str(), dllPath.c_str(), MOVEFILE_REPLACE_EXISTING);
		return Result::Failed;
	}
	log("update: v%u installed, it starts with the next game launch", latest);
	return Result::Installed;
}

}  // namespace update
