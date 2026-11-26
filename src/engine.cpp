#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <cstdio>
#include <atomic>
#include <cstring>

#include "engine.hpp"

namespace engine {

namespace {

constexpr uintptr_t kPlayerHandleRva = 0x5ECAA80;  // из get-player-health
constexpr uintptr_t kProcessIdOffset = 0x98;
constexpr uintptr_t kLocatorOffset = 0x1B0;        // pos xyz(w), затем quat
constexpr uintptr_t kLevelMgrRva = 0x6433130;      // тот же менеджер, что обходит get-spawner
constexpr uintptr_t kMgrLevels = 0x1E0, kMgrLevelCount = 0x1D8, kLevelSpawnData = 0x100;
constexpr uintptr_t kTaskId = 0x08;                // task-get-current -> объект задания, id на +0x08

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

// Статическая запись: { объект*, хеш имени, next }, адрес функции в объект+8.
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

uintptr_t Player() {
	struct { uintptr_t entry; uint32_t id; } handle{};
	uint32_t id = 0;
	if (!Read(g_base + kPlayerHandleRva, handle) || !handle.entry || !handle.id) return 0;
	const uintptr_t object = ReadPtr(handle.entry + 8);
	return object && Read(object + kProcessIdOffset, id) && id == handle.id ? object : 0;
}

// Все спавнеры загруженных уровней; fn возвращает false, чтобы остановиться.
template <typename Fn>
void ForEachSpawner(Fn&& fn) {
	constexpr uint32_t kMaxLevels = 0x200;
	constexpr int32_t kMaxPerLevel = 20000;
	const uintptr_t mgr = ReadPtr(g_base + kLevelMgrRva), levels = mgr ? ReadPtr(mgr + kMgrLevels) : 0;
	uint32_t count = 0;
	static uintptr_t levelPtrs[kMaxLevels], spawners[kMaxPerLevel];
	if (!levels || !Read(mgr + kMgrLevelCount, count) || count > kMaxLevels || !SafeCopy(levelPtrs, levels, count * 8ull)) return;
	for (uint32_t i = 0; i < count; ++i) {
		const uintptr_t sd = ReadPtr(levelPtrs[i] + kLevelSpawnData), arr = sd ? ReadPtr(sd + 8) : 0;
		int32_t n = 0;
		if (!arr || !Read(sd + 0x10, n) || n <= 0 || n > kMaxPerLevel || !SafeCopy(spawners, arr, n * 8ull)) continue;
		// Мусорный уровень (неверный счётчик) не должен стоить тысяч перехваченных исключений.
		for (int32_t j = 0, misses = 0; j < n && misses < 8; ++j) {
			SpawnerHead h{};
			if (!spawners[j] || !Read(spawners[j], h)) { ++misses; continue; }
			misses = 0;
			if (!fn(spawners[j], h)) return;
		}
	}
}

}  // namespace

bool IsFemaleHero(const std::string& name) {
	for (const char* f : { "ellie", "sarah", "tess", "marlene", "riley", "maria" })
		if (name == f) return true;
	return false;
}

// Образ героя надевается на скелет и анимации спавнера, поэтому спавнер подбирается под героя:
// мужской образ на женском скелете (Джоэл на Марлин) бегал криво. Заражённых и собак не берём.
// Сюжетные арты: "t1x-<имя>[-costume-NN]"; обычные: "survivor-m-02", "military-f-01"...
// 0 - не годится; больше - лучше. Спавнер самого героя не берём: копия персонажа, которым
// играет игрок, роняла игру (HALT движка при spawn-entity-at-with-look).
// Арт "t1x-..." бывает и у предметов ("t1x-tool-clump-cleaning-rod" у treasure-tool-3: попытка
// сделать из него NPC роняла игру), поэтому персонаж - только "t1x-<имя>-costume-...".
int SpawnerRank(const std::string& art, const std::string& hero) {
	if (art.rfind("t1x-", 0) == 0) {
		if (art.find("-costume") == std::string::npos) return 0;
		const std::string name = HeroName(art);
		if (!hero.empty() && name == hero) return 0;
		// Сюжетные персонажи (Марлин, Тесс) надёжно слушаются команд движения - всегда впереди.
		return SameSex(art, hero) ? 4 : 3;
	}
	// Обычные NPC (контрабандист в Бостоне) появляются, но не реагируют ни на телепорт, ни на
	// npc-move-to - только если сюжетных нет.
	if (art.rfind("survivor-", 0) && art.rfind("smuggler-", 0) && art.rfind("military-", 0)) return 0;
	return SameSex(art, hero) ? 2 : 1;
}

bool SameSex(const std::string& art, const std::string& hero) {
	if (hero.empty()) return true;
	const bool heroFemale = IsFemaleHero(hero);
	if (const std::string name = HeroName(art); !name.empty()) return IsFemaleHero(name) == heroFemale;
	return art.find(heroFemale ? "-f-" : "-m-") != std::string::npos;
}

namespace {

}  // namespace

uintptr_t g_base = 0;
Natives g_dc{};
// Чтение без исключений. Перехват ошибки доступа (__try) в потоках-файберах движка ломал саму
// обработку исключения (вторая ошибка в ntdll, игра вставала), а ReadProcessMemory на каждое
// поле при обходе тысяч спавнеров вешал игру на секунды. Поэтому адрес проверяется заранее:
// VirtualQuery по области памяти, области кэшируются (их немного и они большие) на 200 мс,
// чтобы освобождённая игрой память не считалась доступной надолго.
namespace {

struct Region { uintptr_t begin, end; ULONGLONG at; };
thread_local Region t_regions[32];
thread_local unsigned t_nextRegion = 0;

bool IsReadableProtect(DWORD p) {
	if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
	return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
		PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool Readable(uintptr_t begin, uintptr_t end) {
	const ULONGLONG now = GetTickCount64();
	for (uintptr_t p = begin; p < end;) {
		bool cached = false;
		for (const Region& r : t_regions)
			if (p >= r.begin && p < r.end && now - r.at < 200) {
				p = r.end;
				cached = true;
				break;
			}
		if (cached) continue;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
			!IsReadableProtect(mbi.Protect))
			return false;
		const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress), re = rb + mbi.RegionSize;
		t_regions[t_nextRegion++ % 32] = { rb, re, now };
		p = re;
	}
	return true;
}

}  // namespace

bool SafeCopy(void* dst, uintptr_t src, size_t n) {
	if (src < 0x10000 || src + n < src || src + n > 0x7FFFFFFFFFFFull || !Readable(src, src + n)) return false;
	memcpy(dst, reinterpret_cast<const void*>(src), n);
	return true;
}

uintptr_t ReadPtr(uintptr_t a) {
	uintptr_t v = 0;
	return Read(a, v) && v >= 0x10000 && v <= 0x7FFFFFFFFFFFull ? v : 0;
}

// По байту до нуля: строка может кончаться у границы страницы.
std::string ReadName(uintptr_t p) {
	char buf[96]{};
	size_t n = 0;
	while (n + 1 < sizeof(buf) && SafeCopy(&buf[n], p + n, 1) && buf[n]) ++n;
	buf[n] = 0;
	return buf;
}

uint32_t BuildTimestamp() { return Nt()->FileHeader.TimeDateStamp; }

void NameNative(NativeFn fn, const char* name);

bool InitNatives() {
	g_dc = { FindNative("alloc-vector"), FindNative("alloc-quat"), FindNative("alloc-bound-frame"),
		FindNative("spawn-entity-at-with-look"), FindNative("entity-exists?"), FindNative("npc-teleport-to"),
		FindNative("npc-set-script-mode"), FindNative("npc-set-ignore"), FindNative("npc-set-blind"),
		FindNative("npc-set-deaf"), FindNative("npc-enable-hit-reactions"), FindNative("npc-enable-stun"),
		FindNative("npc-enable-dodges"), FindNative("get-object-locator"), FindNative("npc-move-to"),
		FindNative("new-boxed-value"), FindNative("send-event"), FindNative("task-get-current") };
	const std::pair<NativeFn, const char*> names[] = { { g_dc.allocVector, "alloc-vector" }, { g_dc.allocQuat, "alloc-quat" },
		{ g_dc.allocBoundFrame, "alloc-bound-frame" }, { g_dc.spawnWithLook, "spawn" }, { g_dc.entityExists, "entity-exists?" },
		{ g_dc.teleport, "npc-teleport-to" }, { g_dc.scriptMode, "script-mode" }, { g_dc.objectLocator, "get-object-locator" },
		{ g_dc.moveTo, "npc-move-to" }, { g_dc.newBoxedValue, "new-boxed-value" }, { g_dc.sendEvent, "send-event" },
		{ g_dc.taskGetCurrent, "task-get-current" } };
	for (const auto& [fn, name] : names) NameNative(fn, name);
	for (NativeFn f : { g_dc.allocVector, g_dc.allocQuat, g_dc.allocBoundFrame, g_dc.spawnWithLook, g_dc.entityExists,
			g_dc.teleport, g_dc.scriptMode, g_dc.setIgnore, g_dc.setBlind, g_dc.setDeaf, g_dc.hitReactions, g_dc.stun,
			g_dc.dodges, g_dc.objectLocator, g_dc.moveTo, g_dc.newBoxedValue, g_dc.sendEvent, g_dc.taskGetCurrent })
		if (!f) return false;
	return true;
}

uint64_t F(float v) {
	uint32_t bits;
	memcpy(&bits, &v, 4);
	return bits;
}

// Замер: сколько вызовов и времени ушло на каждую функцию движка (для лога, раз в 2 с).
namespace {
struct ProfileSlot { NativeFn fn; const char* name; std::atomic<uint64_t> calls, us; };
ProfileSlot g_profile[32];
int g_profileCount = 0;
}  // namespace

void NameNative(NativeFn fn, const char* name) {
	if (fn && g_profileCount < 32) {
		g_profile[g_profileCount].fn = fn;
		g_profile[g_profileCount++].name = name;
	}
}

std::string TakeProfile() {
	std::string out;
	int order[32];
	for (int i = 0; i < g_profileCount; ++i) order[i] = i;
	std::sort(order, order + g_profileCount, [](int a, int b) { return g_profile[a].us > g_profile[b].us; });
	for (int k = 0; k < g_profileCount; ++k) {
		ProfileSlot& s = g_profile[order[k]];
		const uint64_t calls = s.calls.exchange(0), us = s.us.exchange(0);
		if (!calls) continue;
		char buf[96];
		snprintf(buf, sizeof(buf), "%s%s %llux %.1fms", out.empty() ? "" : ", ", s.name,
			static_cast<unsigned long long>(calls), us / 1000.0);
		out += buf;
	}
	return out;
}

uint64_t Call(NativeFn fn, std::initializer_list<uint64_t> args) {
	uint64_t argv[16]{}, ret[8]{};
	int argc = 0;
	for (uint64_t a : args) argv[argc++] = a;
	LARGE_INTEGER a, b, f;
	QueryPerformanceCounter(&a);
	fn(ret, argc, argv, 0);
	QueryPerformanceCounter(&b);
	QueryPerformanceFrequency(&f);
	for (int i = 0; i < g_profileCount; ++i)
		if (g_profile[i].fn == fn) {
			g_profile[i].calls.fetch_add(1, std::memory_order_relaxed);
			g_profile[i].us.fetch_add(static_cast<uint64_t>((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart), std::memory_order_relaxed);
			break;
		}
	return ret[0];
}

bool PlayerLocator(Locator& l) {
	const uintptr_t p = Player();
	return p && Read(p + kLocatorOffset, l) && std::isfinite(l.pos[0]) && std::isfinite(l.pos[2]);
}

// get-object-locator отдаёт копию bound-frame объекта (объект+0x180, 0x50 байт) во временной
// памяти кадра; сам локатор {pos, quat} в ней на +0x30 (у объекта это +0x1B0).
bool NpcLocator(uint64_t npc, Locator& out) {
	const uint64_t p = Call(g_dc.objectLocator, { npc });
	return p && Read(p + 0x30, out) && std::isfinite(out.pos[0]) && std::isfinite(out.pos[2]);
}

uint64_t BoundFrame(const float pos[3], const float rot[4]) {
	const uint64_t v = Call(g_dc.allocVector, { F(pos[0]), F(pos[1]), F(pos[2]), F(1.0f), 0 });
	const uint64_t q = Call(g_dc.allocQuat, { F(rot[0]), F(rot[1]), F(rot[2]), F(rot[3]), 0 });
	return v && q ? Call(g_dc.allocBoundFrame, { v, q, 0, 0, 0 }) : 0;
}

// task-get-current возвращает объект задания; id задания на +0x08, узел (чекпоинт) на +0x120.
// Как в TLOU2-Coop; в Part I это подтверждено разбором task-get-current-node-id.
uint64_t CurrentChapter() {
	const uintptr_t task = static_cast<uintptr_t>(Call(g_dc.taskGetCurrent, {}));
	uint64_t id = 0;
	return task && Read(task + kTaskId, id) ? id : 0;
}

std::string HeroName(const std::string& art) {
	if (art.rfind("t1x-", 0) != 0) return {};
	const size_t end = art.find('-', 4);
	return art.substr(4, end == std::string::npos ? std::string::npos : end - 4);
}

uintptr_t FindBuddySpawner(const std::string& hero, const std::vector<uintptr_t>& exclude, std::string& nameOut) {
	uintptr_t best = 0;
	int bestRank = 0;
	ForEachSpawner([&](uintptr_t sp, const SpawnerHead& h) {
		if (!h.type || (h.flags0 & kSpDisabled)) return true;
		for (uintptr_t bad : exclude)
			if (bad == sp) return true;
		const std::string art = ReadName(h.art);
		int rank = SpawnerRank(art, hero) * 2;
		if (!rank) return true;
		// "snpc-..." - упрощённая массовка; полноценный NPC ("npc-...", сюжетный) надёжнее
		// принимает команды движения, поэтому при равном ранге он впереди.
		const std::string name = ReadName(h.name);
		if (name.rfind("snpc-", 0) != 0) ++rank;
		if (rank > bestRank) {
			best = sp;
			bestRank = rank;
			nameOut = name + " (" + art + ")";
		}
		return bestRank < 9;
	});
	return best;
}

std::string HeroLook(const float around[3]) {
	std::string best;
	float bestDist = 1e30f;
	ForEachSpawner([&](uintptr_t, const SpawnerHead& h) {
		if (h.type) return true;
		const float dx = h.pos[0] - around[0], dy = h.pos[1] - around[1], dz = h.pos[2] - around[2];
		const float d = dx * dx + dy * dy + dz * dz;
		if (d >= bestDist || ReadName(h.name).find("-player-") == std::string::npos) return true;
		if (std::string art = ReadName(h.art); art.rfind("t1x-", 0) == 0) {
			bestDist = d;
			best = std::move(art);
		}
		return true;
	});
	return best;
}

}  // namespace engine
