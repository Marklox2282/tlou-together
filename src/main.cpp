// TLOU Part I co-op: загрузка (прокси winmm), хуки, клавиши, сеть, журнал падений, обновление.
// Основан на TLOU2-Coop (GPL-3.0).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <safetyhook.hpp>

#include "engine.hpp"
#include "log.hpp"
#include "net.hpp"
#include "puppet.hpp"
#include "update.hpp"
#include "winmm-proxy.hpp"

namespace {

// Номер версии мода: увеличивать при каждом выпуске (tools/release.py берёт его отсюда).
// Игроки с разными версиями не соединяются.
constexpr uint32_t kModVersion = 9;

using engine::Locator;

safetyhook::MidHook g_frameHook, g_scriptHook;

bool KeyPressed(int vk, bool& wasDown) {
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000);
	const bool pressed = down && !wasDown;
	wasDown = down;
	return pressed;
}

// ---- сеть ----------------------------------------------------------------------------------
// tlou1-coop.ini рядом с игрой; без peer_ip связь идёт через Steam.
net::Config LoadConfig(std::string& name) {
	net::Config cfg;
	const std::wstring path = GameDirFile(L"tlou1-coop.ini");
	if (path.empty()) return cfg;
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"r") || !f) {
		if (!_wfopen_s(&f, path.c_str(), L"w") && f) {
			fputs("; TLOU Part I co-op. Lines starting with ; are comments.\n"
				"; name      = shown to your partner; empty = your Steam name.\n"
				"; peer_ip   = empty: F8 connects to a Steam friend who is in this game and also pressed F8.\n"
				";             An IP (LAN): connect directly over UDP instead of Steam.\n"
				"; port      = your UDP port for peer_ip mode; peer_port = your friend's.\n"
				"name=\npeer_ip=\nport=7777\npeer_port=7777\n", f);
			fclose(f);
		}
		return cfg;
	}
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char* eq = strchr(line, '=');
		if (line[0] == ';' || !eq) continue;
		*eq = 0;
		std::string key = line, value = eq + 1;
		while (!value.empty() && strchr(" \t\r\n", value.back())) value.pop_back();
		while (!value.empty() && strchr(" \t", value.front())) value.erase(0, 1);
		while (!key.empty() && strchr(" \t", key.back())) key.pop_back();
		if (key == "peer_ip") cfg.peerIp = value;
		else if (key == "name") name = value;
		else if (key == "port") cfg.port = static_cast<uint16_t>(atoi(value.c_str()));
		else if (key == "peer_port") cfg.peerPort = static_cast<uint16_t>(atoi(value.c_str()));
	}
	fclose(f);
	return cfg;
}

net::Link g_link(Log, kModVersion);

// Все сетевые вызовы - из игрового кадра (основной поток): Link не потокобезопасен.
void NetTick(ULONGLONG now, bool havePlayer, const Locator& me) {
	static bool f8Down = false, f6Down = false;
	static ULONGLONG lastSend = 0;
	if (KeyPressed(VK_F8, f8Down) && g_link.GetState() == net::State::Off) {
		std::string name;
		const net::Config cfg = LoadConfig(name);
		g_link.SetName(name);
		puppet::SetNetActive(g_link.Start(cfg, now));
	}
	if (KeyPressed(VK_F6, f6Down) && g_link.GetState() != net::State::Off) {
		g_link.Stop();
		puppet::SetNetActive(false);
		puppet::SetPartnerChapter(0);
		puppet::UseNetwork(false);
	}
	if (g_link.GetState() == net::State::Off) return;

	g_link.SetChapter(puppet::MyChapter());
	net::Packet pose;
	if (g_link.Tick(now, pose)) {
		if (!puppet::UsingNetwork()) {  // первая поза друга: марионетка теперь его
			puppet::UseNetwork(true);
			puppet::SetWanted(true);
			puppet::SetFollow(true);
			Log("net: driving the puppet from %s", g_link.PeerName().c_str());
		}
		Locator l{};
		memcpy(l.pos, pose.pos, sizeof(pose.pos));
		memcpy(l.rot, pose.rot, sizeof(pose.rot));
		puppet::Record(now, l);
	}
	puppet::SetPartnerChapter(g_link.GetState() == net::State::Connected ? g_link.PeerChapter() : 0);
	if (havePlayer && now - lastSend >= 50) {  // 20 раз в секунду
		lastSend = now;
		g_link.SendPose(me.pos, me.rot);
	}
}

void FrameTick(safetyhook::Context&) {
	static ULONGLONG lastLog = 0;
	static unsigned frames = 0;
	static bool f10Down = false, f9Down = false, f7Down = false;
	static uint64_t frameUs = 0;
	LARGE_INTEGER start;
	QueryPerformanceCounter(&start);
	++frames;
	puppet::OnFrame();
	const ULONGLONG now = GetTickCount64();

	Locator me{};
	const bool havePlayer = engine::PlayerLocator(me);
	NetTick(now, havePlayer, me);
	if (havePlayer && !puppet::UsingNetwork()) puppet::Record(now, me);  // локальная проверка
	// F10: включить/выключить тестовую марионетку. Выключение пока только перестаёт её двигать.
	if (KeyPressed(VK_F10, f10Down)) {
		puppet::SetWanted(!puppet::Wanted());
		Log("F10: puppet %s", puppet::Wanted() ? "on" : "off");
	}
	// F9: марионетка идёт по следу (иначе стоит, где появилась).
	if (KeyPressed(VK_F9, f9Down)) {
		puppet::SetFollow(!puppet::Follow());
		Log("F9: follow %s", puppet::Follow() ? "on" : "off");
	}
	if (KeyPressed(VK_F7, f7Down)) puppet::RequestTeleport();

	if (now - lastLog >= 2000) {
		// Сколько мод тратит на кадр: в основном потоке и в скриптовом (среднее за 2 с).
		const uint64_t scriptUs = puppet::TakeWorkMicroseconds();
		if (havePlayer && frames)
			Log("player pos=(%.2f %.2f %.2f) fps=%.0f mod=%llu+%llu us/frame", me.pos[0], me.pos[1], me.pos[2],
				frames * 1000.0 / (now - lastLog), frameUs / frames, scriptUs / frames);
		lastLog = now;
		frames = 0;
		frameUs = 0;
	}
	LARGE_INTEGER end, freq;
	QueryPerformanceCounter(&end);
	QueryPerformanceFrequency(&freq);
	frameUs += static_cast<uint64_t>((end.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart);
}

void ScriptInvoke(safetyhook::Context&) { puppet::ScriptTick(); }

// ---- журнал падений ------------------------------------------------------------------------
// Windows пишет дамп не на каждое падение, поэтому при ошибке доступа мод сам пишет в
// tlou1-coop-crash.log адрес, регистры и адреса возврата со стека. Обработку ошибки не меняет.
// Только WriteFile и snprintf: блокировки CRT в момент падения могут быть заняты.
HANDLE g_crashFile = INVALID_HANDLE_VALUE;
std::atomic<int> g_crashCount{ 0 };

void CrashWrite(const char* fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	DWORD written = 0;
	if (n > 0) WriteFile(g_crashFile, buf, static_cast<DWORD>((std::min)(n, static_cast<int>(sizeof(buf)) - 1)), &written, nullptr);
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
	if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_crashCount++ >= 3)
		return EXCEPTION_CONTINUE_SEARCH;
	const uintptr_t base = engine::g_base;
	const CONTEXT& c = *e->ContextRecord;
	auto rva = [base](uint64_t a) { return a >= base && a < base + 0x9000000 ? a - base : 0; };
	CrashWrite("\r\naccess violation v%u tid=%lu rip=tlou+%llX (%llX) %s %llX\r\n", kModVersion, GetCurrentThreadId(), rva(c.Rip),
		c.Rip, e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", e->ExceptionRecord->ExceptionInformation[1]);
	CrashWrite("rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX r8=%llX r9=%llX rsp=%llX\r\n",
		c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9, c.Rsp);
	CrashWrite("puppet=%016llX wanted=%d follow=%d stack:", puppet::Current(), puppet::Wanted() ? 1 : 0, puppet::Follow() ? 1 : 0);
	const HMODULE self = GetModuleHandleW(L"winmm.dll");
	for (uint64_t p = c.Rsp, found = 0; p < c.Rsp + 0x3000 && found < 40; p += 8) {
		uint64_t v = 0;
		if (!engine::Read(p, v)) break;
		if (rva(v)) { CrashWrite(" tlou+%llX", rva(v)); ++found; }
		else if (self && v >= reinterpret_cast<uint64_t>(self) && v < reinterpret_cast<uint64_t>(self) + 0x100000) {
			CrashWrite(" MOD+%llX", v - reinterpret_cast<uint64_t>(self));
			++found;
		}
	}
	CrashWrite("\r\n");
	return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLog() {
	const std::wstring path = GameDirFile(L"tlou1-coop-crash.log");
	if (path.empty()) return;
	g_crashFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (g_crashFile != INVALID_HANDLE_VALUE && AddVectoredExceptionHandler(1, OnException)) Log("crash log: on");
}

// ---- автообновление ------------------------------------------------------------------------
std::wstring SelfPath() {
	HMODULE self = nullptr;
	wchar_t path[MAX_PATH]{};
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&SelfPath), &self);
	const DWORD n = self ? GetModuleFileNameW(self, path, MAX_PATH) : 0;
	return n && n < MAX_PATH ? path : L"";
}

void StartUpdateCheck() {
	static std::wstring self = SelfPath();
	if (self.empty()) return;
	update::CleanupOld(self);
	// Фоновый поток: сеть не трогаем в DllMain и не задерживаем запуск игры.
	if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
			Sleep(5000);
			update::Run(kModVersion, self, update::kReleaseUrl, Log);
			return 0;
		}, nullptr, 0, nullptr))
		CloseHandle(t);
}

void Init() {
	engine::g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	const wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash || _wcsicmp(slash + 1, L"tlou-i.exe")) return;
	OpenLog();
	const uint32_t stamp = engine::BuildTimestamp();
	Log("tlou1-coop v%u, base=%p build=%08X", kModVersion, reinterpret_cast<void*>(engine::g_base), stamp);
	StartUpdateCheck();  // и на неподдерживаемой сборке игры: обновление может её поддержать
	if (stamp != engine::kBuildTimestamp) {
		Log("unsupported game build, mod is off");
		return;
	}
	if (!engine::InitNatives()) {
		Log("natives: MISSING, mod is off");
		return;
	}
	Log("natives: ok");
	auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(engine::g_base + engine::kFrameTickRva), FrameTick);
	if (hook) g_frameHook = std::move(*hook);
	Log("frame hook: %s", g_frameHook ? "ok" : "FAILED");
	auto script = safetyhook::MidHook::create(reinterpret_cast<void*>(engine::g_base + engine::kScriptInvokeRva), ScriptInvoke);
	if (script) g_scriptHook = std::move(*script);
	Log("script hook: %s", g_scriptHook ? "ok" : "FAILED");
	InstallCrashLog();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(module);
		winmm_proxy::ResolveReal();
		Init();
	}
	return TRUE;
}
