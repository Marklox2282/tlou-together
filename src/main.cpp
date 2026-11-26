// TLOU Part I co-op, этап 2: игровой тик и тестовый второй персонаж (F10).
// Основан на TLOU2-Coop (GPL-3.0). Адреса - только для Steam build 18995818.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <share.h>
#include <string>
#include <utility>

#include <safetyhook.hpp>

#include "net.hpp"
#include "update.hpp"
#include "winmm-proxy.hpp"

namespace {

constexpr uint32_t kBuildTimestamp = 0x685BEBE1;
constexpr uintptr_t kPlayerHandleRva = 0x5ECAA80;  // из get-player-health
constexpr uintptr_t kProcessIdOffset = 0x98;
constexpr uintptr_t kLocatorOffset = 0x1B0;        // pos xyz(w), затем quat
constexpr uintptr_t kFrameTickRva = 0x11D45C7;     // конец кадра: ++frameCounter (основной поток)
// Обёртка, через которую интерпретатор скриптов вызывает любую функцию движка (0x208BBF8):
// 0x208BB1C открывает скриптовую область, потом call rax. Точка между ними - в скриптовом
// контексте (без него spawn падал) и вне чужих функций движка (вложенный вызов вешал игру).
// Найдено замером: через неё идут ~100 разных функций, через другие места - единицы.
constexpr uintptr_t kScriptInvokeRva = 0x208BC36;
constexpr uintptr_t kFrameCounterRva = 0x6433108;  // [ptr]+0x40 = номер кадра (get-current-frame-count)
constexpr uintptr_t kLevelMgrRva = 0x6433130;      // тот же менеджер, что обходит get-spawner
constexpr uintptr_t kMgrLevels = 0x1E0, kMgrLevelCount = 0x1D8, kLevelSpawnData = 0x100;
// Раскладка спавнера совпадает с Part II.
constexpr uintptr_t kSpSid = 0x38, kSpName = 0x50, kSpType = 0x70, kSpArt = 0x78, kSpFlags0 = 0x80, kSpFlags1 = 0x81;
constexpr uint8_t kSpDisabled = 0x08, kSpMultiSpawn = 0x10;

uintptr_t g_base = 0;
FILE* g_log = nullptr;
safetyhook::MidHook g_frameHook, g_scriptHook;

void Log(const char* fmt, ...) {
	if (!g_log) return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(g_log, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

// Чтение через ядро: неверный адрес даёт false, а не вылет игры.
template <typename T>
bool Read(uintptr_t address, T& out) {
	SIZE_T n = 0;
	return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &out, sizeof(T), &n) &&
		n == sizeof(T);
}

uintptr_t ReadPtr(uintptr_t a) {
	uintptr_t v = 0;
	return Read(a, v) && v >= 0x10000 && v <= 0x7FFFFFFFFFFFull ? v : 0;
}

std::string ReadName(uintptr_t p) {
	char buf[96]{};
	SIZE_T n = 0;
	if (!p || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf, sizeof(buf) - 1, &n)) return {};
	buf[n] = 0;
	return buf;
}

constexpr uint64_t Sid(const char* s) {
	uint64_t h = 0xCBF29CE484222325;
	while (*s) h = (h ^ static_cast<uint8_t>(*s++)) * 0x100000001B3;
	return h;
}

const IMAGE_NT_HEADERS64* Nt() {
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
	return reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
}

struct Span { uintptr_t begin, end; };

Span Section(const char* name) {
	const auto* nt = Nt();
	const auto* s = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		if (!strncmp(reinterpret_cast<const char*>(s[i].Name), name, 8))
			return { g_base + s[i].VirtualAddress, g_base + s[i].VirtualAddress + s[i].SizeOfRawData };
	return {};
}

// ---- скриптовые функции движка -------------------------------------------------------------
// Статическая запись: { объект*, хеш имени, next }, адрес функции в объект+8.
// Вызов: fn(ret*, argc, argv*, 0); аргументы - 8-байтовые слоты, float в младших 32 битах.
using NativeFn = void (*)(uint64_t* ret, int argc, const uint64_t* argv, uint64_t unused);

NativeFn FindNative(const char* name) {
	const uint64_t sid = Sid(name);
	const Span data = Section(".data"), text = Section(".text");
	for (uintptr_t p = data.begin + 8; p + 8 <= data.end; p += 8) {
		if (*reinterpret_cast<const uint64_t*>(p) != sid) continue;
		const uintptr_t object = ReadPtr(p - 8), fn = object ? ReadPtr(object + 8) : 0;
		if (fn >= text.begin && fn < text.end) return reinterpret_cast<NativeFn>(fn);
	}
	return nullptr;
}

struct Natives {
	NativeFn allocVector, allocQuat, allocBoundFrame, spawnWithLook, entityExists, teleport, scriptMode,
		setIgnore, setBlind, setDeaf, hitReactions, stun, dodges, objectLocator, moveTo;
	bool Ok() const {
		for (NativeFn f : { allocVector, allocQuat, allocBoundFrame, spawnWithLook, entityExists, teleport, scriptMode,
				setIgnore, setBlind, setDeaf, hitReactions, stun, dodges, objectLocator, moveTo })
			if (!f) return false;
		return true;
	}
} g_dc{};

uint64_t F(float v) {
	uint32_t bits;
	memcpy(&bits, &v, 4);
	return bits;
}

uint64_t Call(NativeFn fn, std::initializer_list<uint64_t> args) {
	uint64_t argv[16]{}, ret[8]{};
	int argc = 0;
	for (uint64_t a : args) argv[argc++] = a;
	fn(ret, argc, argv, 0);
	return ret[0];
}

// ---- игрок -------------------------------------------------------------------------------
struct Locator { float pos[4], rot[4]; };

uintptr_t Player() {
	struct { uintptr_t entry; uint32_t id; } handle{};
	uint32_t id = 0;
	if (!Read(g_base + kPlayerHandleRva, handle) || !handle.entry || !handle.id) return 0;
	const uintptr_t object = ReadPtr(handle.entry + 8);
	return object && Read(object + kProcessIdOffset, id) && id == handle.id ? object : 0;
}

bool PlayerLocator(Locator& l) {
	const uintptr_t p = Player();
	return p && Read(p + kLocatorOffset, l) && std::isfinite(l.pos[0]) && std::isfinite(l.pos[2]);
}

// ---- тестовый партнёр: копия человека-NPC с загруженного уровня ----------------------------
// Лучше всего сюжетный персонаж (арт "t1x-<имя>-costume-NN"), иначе обычный гражданский.
// Заражённых и собак не берём: у них другой скелет.
int SpawnerRank(const std::string& art) {
	if (art.rfind("t1x-", 0) == 0 && art.find("-costume") != std::string::npos) return 2;
	if (art.rfind("survivor-", 0) == 0 || art.rfind("smuggler-", 0) == 0) return 1;
	return 0;
}

uintptr_t FindBuddySpawner(std::string& nameOut) {
	const uintptr_t mgr = ReadPtr(g_base + kLevelMgrRva), levels = mgr ? ReadPtr(mgr + kMgrLevels) : 0;
	uint32_t count = 0;
	if (!levels || !Read(mgr + kMgrLevelCount, count) || count > 0x200) return 0;
	uintptr_t best = 0;
	int bestRank = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const uintptr_t sd = ReadPtr(ReadPtr(levels + i * 8) + kLevelSpawnData), arr = sd ? ReadPtr(sd + 8) : 0;
		int32_t n = 0;
		if (!arr || !Read(sd + 0x10, n) || n <= 0 || n > 100000) continue;
		for (int32_t j = 0; j < n; ++j) {
			const uintptr_t sp = ReadPtr(arr + j * 8);
			uint64_t type = 0;  // 0 = точка появления игрока, не NPC
			uint8_t f0 = 0;
			if (!sp || !Read(sp + kSpType, type) || !type || !Read(sp + kSpFlags0, f0) || (f0 & kSpDisabled)) continue;
			const std::string art = ReadName(ReadPtr(sp + kSpArt));
			const int rank = SpawnerRank(art);
			if (rank > bestRank) {
				best = sp;
				bestRank = rank;
				nameOut = ReadName(ReadPtr(sp + kSpName)) + " (" + art + ")";
				if (rank == 2) return best;
			}
		}
	}
	return best;
}

uint64_t BoundFrame(const float pos[3], const float rot[4]) {
	const uint64_t v = Call(g_dc.allocVector, { F(pos[0]), F(pos[1]), F(pos[2]), F(1.0f), 0 });
	const uint64_t q = Call(g_dc.allocQuat, { F(rot[0]), F(rot[1]), F(rot[2]), F(rot[3]), 0 });
	return v && q ? Call(g_dc.allocBoundFrame, { v, q, 0, 0, 0 }) : 0;
}

// Копия NPC без собственной воли: тот же набор, что у марионетки в TLOU2-Coop.
uint64_t SpawnPuppet(const Locator& at) {
	std::string name;
	const uintptr_t sp = FindBuddySpawner(name);
	uint64_t sid = 0;
	uint8_t f1 = 0;
	if (!sp || !Read(sp + kSpSid, sid) || !Read(sp + kSpFlags1, f1)) {
		Log("puppet: no buddy spawner in loaded levels");
		return 0;
	}
	// Бит "multi-spawn": иначе движок переиспользует и телепортирует сюжетного NPC уровня.
	auto* flags1 = reinterpret_cast<uint8_t*>(sp + kSpFlags1);
	*flags1 = static_cast<uint8_t>(f1 | kSpMultiSpawn);
	const uint64_t frame = BoundFrame(at.pos, at.rot);
	const uint64_t npc = frame ? Call(g_dc.spawnWithLook, { sid, frame, 0 }) : 0;
	*flags1 = f1;
	if (!npc || npc == sid) {
		Log("puppet: spawn from %s failed (npc=%016llX)", name.c_str(), static_cast<unsigned long long>(npc));
		return 0;
	}
	Call(g_dc.scriptMode, { npc, 2, 1 });  // выключить ИИ и сюжетные команды
	for (NativeFn on : { g_dc.setIgnore, g_dc.setBlind, g_dc.setDeaf }) Call(on, { npc, 1 });
	for (NativeFn off : { g_dc.hitReactions, g_dc.stun, g_dc.dodges }) Call(off, { npc, 0 });
	Log("puppet: %s -> %016llX", name.c_str(), static_cast<unsigned long long>(npc));
	return npc;
}

// След, по которому идёт марионетка: позы друга из сети (задержка 120 мс на сглаживание
// неровной доставки) или, для локальной проверки без сети, свои позы с задержкой 1 с.
constexpr int kTrail = 256;
constexpr ULONGLONG kLocalDelayMs = 1000, kNetDelayMs = 120;
std::atomic<ULONGLONG> g_trailDelay{ kLocalDelayMs };
struct Sample { ULONGLONG t; Locator l; };
Sample g_trail[kTrail];
int g_trailHead = 0;
SRWLOCK g_trailLock = SRWLOCK_INIT;

void ClearTrail(ULONGLONG delay) {
	AcquireSRWLockExclusive(&g_trailLock);
	memset(g_trail, 0, sizeof(g_trail));
	g_trailHead = 0;
	g_trailDelay = delay;
	ReleaseSRWLockExclusive(&g_trailLock);
}

void RecordTrail(ULONGLONG now, const Locator& l) {
	AcquireSRWLockExclusive(&g_trailLock);
	const Sample& last = g_trail[(g_trailHead + kTrail - 1) % kTrail];
	if (now - last.t >= 15) g_trail[g_trailHead++ % kTrail] = { now, l };
	ReleaseSRWLockExclusive(&g_trailLock);
}

bool DelayedTarget(ULONGLONG now, Locator& out, ULONGLONG extra = 0) {
	const ULONGLONG delay = g_trailDelay.load() + extra;
	AcquireSRWLockShared(&g_trailLock);
	bool found = false;
	for (int i = 1; i <= kTrail; ++i) {
		const Sample& s = g_trail[(g_trailHead - i + kTrail * 2) % kTrail];
		if (!s.t) break;
		out = s.l;
		found = true;
		if (now - s.t >= delay) break;
	}
	ReleaseSRWLockShared(&g_trailLock);
	return found;
}

float Dist(const float a[3], const float b[3]) {
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// get-object-locator отдаёт копию bound-frame объекта (объект+0x180, 0x50 байт) во временной
// памяти кадра; сам локатор {pos, quat} в ней на +0x30 (у объекта это +0x1B0).
bool NpcLocator(uint64_t npc, Locator& out) {
	const uint64_t p = Call(g_dc.objectLocator, { npc });
	return p && Read(p + 0x30, out) && std::isfinite(out.pos[0]) && std::isfinite(out.pos[2]);
}

std::atomic<bool> g_puppetWanted{ false }, g_follow{ false };
uint64_t g_puppet = 0;
ULONGLONG g_lastSpawnTry = 0;

void PuppetUpdate() {
	const ULONGLONG now = GetTickCount64();
	if (g_puppet && !Call(g_dc.entityExists, { g_puppet })) {
		Log("puppet %016llX is gone (death or checkpoint reload)", static_cast<unsigned long long>(g_puppet));
		g_puppet = 0;
	}
	Locator target{};
	if (!DelayedTarget(now, target)) return;
	if (!g_puppet) {
		if (now - g_lastSpawnTry < 3000) return;
		g_lastSpawnTry = now;
		g_puppet = SpawnPuppet(target);
		return;
	}
	// Марионетка идёт сама своей анимацией: npc-move-to(npc, frame, режим, 0...), где режим
	// 0 шаг, 1 бег, 2 спринт (замерено в TLOU2-Coop). Телепорт - только при сильном отставании:
	// телепорт на каждом кадре ронял игру в Fsm::StateMachineBase::SwitchToNewState.
	static ULONGLONG lastCmdAt = 0, lastSnapAt = 0;
	static float lastGoal[3]{};
	static int lastMode = -1;
	if (!g_follow || now - lastCmdAt < 100) return;
	Locator cur{}, before{};
	if (!NpcLocator(g_puppet, cur) || !DelayedTarget(now, before, 250)) {
		static ULONGLONG lastWarn = 0;
		if (now - lastWarn > 5000) Log("puppet: can't read its position");
		lastWarn = now;
		return;
	}
	const float err = Dist(cur.pos, target.pos);
	const float targetSpeed = Dist(before.pos, target.pos) / 0.25f;
	if (err > 4.0f) {
		if (now - lastSnapAt < 1000) return;
		lastSnapAt = lastCmdAt = now;
		lastMode = -1;
		if (const uint64_t frame = BoundFrame(target.pos, target.rot)) Call(g_dc.teleport, { g_puppet, frame });
		Log("puppet: snapped (%.1f m behind)", err);
		return;
	}
	if (targetSpeed < 0.4f && err < 1.0f) return;  // стоим рядом - ничего не делаем
	const float speed = (std::max)(targetSpeed, err * 0.8f);
	const int mode = speed < 1.8f ? 0 : (speed < 4.2f ? 1 : 2);
	if (mode == lastMode && Dist(target.pos, lastGoal) < 0.75f && now - lastCmdAt < 1500) return;
	if (const uint64_t frame = BoundFrame(target.pos, target.rot))
		Call(g_dc.moveTo, { g_puppet, frame, static_cast<uint64_t>(mode), 0, 0, 0, 0, 0 });
	memcpy(lastGoal, target.pos, sizeof(lastGoal));
	if (mode != lastMode) Log("puppet: move mode %d (target %.1f m/s, %.1f m away)", mode, targetSpeed, err);
	lastMode = mode;
	lastCmdAt = now;
}

uint64_t FrameCounter() {
	const uintptr_t globals = ReadPtr(g_base + kFrameCounterRva);
	uint64_t frame = 0;
	return globals && Read(globals + 0x40, frame) ? frame : 0;
}

// Скрипты выполняются на нескольких потоках; обновление - один раз за кадр, первым пришедшим.
std::atomic<uint64_t> g_puppetFrame{ 0 };

void ScriptInvoke(safetyhook::Context&) {
	if (!g_puppetWanted.load(std::memory_order_relaxed)) return;
	const uint64_t frame = FrameCounter();
	uint64_t done = g_puppetFrame.load(std::memory_order_relaxed);
	if (!frame || done == frame || !g_puppetFrame.compare_exchange_strong(done, frame)) return;
	PuppetUpdate();
}

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
net::Config LoadConfig() {
	net::Config cfg;
	wchar_t path[MAX_PATH];
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	wchar_t* slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
	if (!slash) return cfg;
	slash[1] = 0;
	if (wcscat_s(path, L"tlou1-coop.ini")) return cfg;
	FILE* f = nullptr;
	if (_wfopen_s(&f, path, L"r") || !f) {
		if (!_wfopen_s(&f, path, L"w") && f) {
			fputs("; TLOU Part I co-op. Lines starting with ; are comments.\n"
				"; peer_ip   = empty: F8 connects to a Steam friend who is in this game and also pressed F8.\n"
				";             An IP (LAN): connect directly over UDP instead of Steam.\n"
				"; port      = your UDP port for peer_ip mode; peer_port = your friend's.\n"
				"peer_ip=\nport=7777\npeer_port=7777\n", f);
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
		else if (key == "port") cfg.port = static_cast<uint16_t>(atoi(value.c_str()));
		else if (key == "peer_port") cfg.peerPort = static_cast<uint16_t>(atoi(value.c_str()));
	}
	fclose(f);
	return cfg;
}

net::Link g_link(Log);

// Все сетевые вызовы - из игрового кадра (основной поток): Link не потокобезопасен.
void NetTick(ULONGLONG now, bool havePlayer, const Locator& me) {
	static bool f8Down = false, f6Down = false, remote = false;
	static ULONGLONG lastSend = 0;
	if (KeyPressed(VK_F8, f8Down) && g_link.GetState() == net::State::Off) g_link.Start(LoadConfig(), now);
	if (KeyPressed(VK_F6, f6Down) && g_link.GetState() != net::State::Off) {
		g_link.Stop();
		remote = false;
		ClearTrail(kLocalDelayMs);
	}
	if (g_link.GetState() == net::State::Off) return;

	net::Packet pose;
	if (g_link.Tick(now, pose)) {
		if (!remote) {  // первая поза друга: марионетка теперь его
			remote = true;
			ClearTrail(kNetDelayMs);
			g_puppetWanted = g_follow = true;
			Log("net: driving the puppet from %s", g_link.PeerName().c_str());
		}
		Locator l{};
		memcpy(l.pos, pose.pos, sizeof(pose.pos));
		memcpy(l.rot, pose.rot, sizeof(pose.rot));
		RecordTrail(now, l);
	}
	if (havePlayer && now - lastSend >= 50) {  // 20 раз в секунду
		lastSend = now;
		g_link.SendPose(me.pos, me.rot);
	}
}

void FrameTick(safetyhook::Context&) {
	static ULONGLONG lastLog = 0;
	static unsigned frames = 0;
	static bool f10Down = false, f9Down = false;
	++frames;
	const ULONGLONG now = GetTickCount64();

	Locator me{};
	const bool havePlayer = PlayerLocator(me);
	NetTick(now, havePlayer, me);
	if (havePlayer && g_trailDelay.load() == kLocalDelayMs) RecordTrail(now, me);  // локальная проверка
	// F10: включить/выключить тестовую марионетку. Выключение пока только перестаёт её двигать.
	if (KeyPressed(VK_F10, f10Down)) {
		g_puppetWanted = !g_puppetWanted;
		Log("F10: puppet %s", g_puppetWanted ? "on" : "off");
	}
	// F9: марионетка идёт по следу (иначе стоит, где появилась).
	if (KeyPressed(VK_F9, f9Down)) {
		g_follow = !g_follow;
		Log("F9: follow %s", g_follow ? "on" : "off");
	}

	if (now - lastLog >= 2000) {
		Locator l{};
		if (PlayerLocator(l))
			Log("player pos=(%.2f %.2f %.2f) fps=%.0f tid=%lu", l.pos[0], l.pos[1], l.pos[2],
				frames * 1000.0 / (now - lastLog), GetCurrentThreadId());
		lastLog = now;
		frames = 0;
	}
}

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
	if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_crashCount++ >= 3) return EXCEPTION_CONTINUE_SEARCH;
	const CONTEXT& c = *e->ContextRecord;
	auto rva = [](uint64_t a) { return a >= g_base && a < g_base + 0x9000000 ? a - g_base : 0; };
	CrashWrite("\r\naccess violation tid=%lu rip=tlou+%llX (%llX) %s %llX\r\n", GetCurrentThreadId(), rva(c.Rip), c.Rip,
		e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", e->ExceptionRecord->ExceptionInformation[1]);
	CrashWrite("rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX r8=%llX r9=%llX rsp=%llX\r\n",
		c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9, c.Rsp);
	CrashWrite("puppet=%016llX wanted=%d follow=%d stack:", g_puppet, g_puppetWanted.load() ? 1 : 0, g_follow.load() ? 1 : 0);
	const HMODULE self = GetModuleHandleW(L"winmm.dll");
	for (uint64_t p = c.Rsp, found = 0; p < c.Rsp + 0x3000 && found < 40; p += 8) {
		uint64_t v = 0;
		if (!Read(p, v)) break;
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
	wchar_t path[MAX_PATH];
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	wchar_t* slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
	if (!slash) return;
	slash[1] = 0;
	if (wcscat_s(path, L"tlou1-coop-crash.log")) return;
	g_crashFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (g_crashFile != INVALID_HANDLE_VALUE && AddVectoredExceptionHandler(1, OnException)) Log("crash log: on");
}

void OpenLog() {
	wchar_t path[MAX_PATH];
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (!n || n >= MAX_PATH) return;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash) return;
	slash[1] = 0;
	if (wcscat_s(path, L"tlou1-coop.log") == 0) g_log = _wfsopen(path, L"w", _SH_DENYWR);
}

// ---- автообновление ------------------------------------------------------------------------
// Номер версии мода: увеличивать при каждом выпуске (tools/release.ps1 берёт его отсюда).
constexpr uint32_t kModVersion = 7;

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
	g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	const wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash || _wcsicmp(slash + 1, L"tlou-i.exe")) return;
	OpenLog();
	const uint32_t stamp = Nt()->FileHeader.TimeDateStamp;
	Log("tlou1-coop v%u, base=%p build=%08X", kModVersion, reinterpret_cast<void*>(g_base), stamp);
	StartUpdateCheck();  // и на неподдерживаемой сборке игры: обновление может её поддержать
	if (stamp != kBuildTimestamp) {
		Log("unsupported game build, mod is off");
		return;
	}
	g_dc = { FindNative("alloc-vector"), FindNative("alloc-quat"), FindNative("alloc-bound-frame"),
		FindNative("spawn-entity-at-with-look"), FindNative("entity-exists?"), FindNative("npc-teleport-to"),
		FindNative("npc-set-script-mode"), FindNative("npc-set-ignore"), FindNative("npc-set-blind"),
		FindNative("npc-set-deaf"), FindNative("npc-enable-hit-reactions"), FindNative("npc-enable-stun"),
		FindNative("npc-enable-dodges"), FindNative("get-object-locator"), FindNative("npc-move-to") };
	Log("natives: %s", g_dc.Ok() ? "ok" : "MISSING");
	auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(g_base + kFrameTickRva), FrameTick);
	if (hook) g_frameHook = std::move(*hook);
	Log("frame hook: %s", g_frameHook ? "ok" : "FAILED");
	auto script = safetyhook::MidHook::create(reinterpret_cast<void*>(g_base + kScriptInvokeRva), ScriptInvoke);
	if (script) g_scriptHook = std::move(*script);
	Log("script hook: %s", g_scriptHook ? "ok" : "FAILED");
	InstallCrashLog();
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(module);
		winmm_proxy::ResolveReal();
		Init();
	}
	return TRUE;
}
