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
#include "log.hpp"

namespace engine {

namespace {

constexpr uintptr_t kPlayerHandleRva = 0x5ECAA80;  // из get-player-health
constexpr uintptr_t kProcessIdOffset = 0x98;
constexpr uintptr_t kLookupProcessFnRva = 0x365F740;  // табличный поиск ProcessHandle по StringId64
constexpr uintptr_t kLocatorOffset = 0x1B0;        // pos xyz(w), затем quat
constexpr uintptr_t kLevelMgrRva = 0x6433130;      // тот же менеджер, что обходит get-spawner
constexpr uintptr_t kMgrLevels = 0x1E0, kMgrLevelCount = 0x1D8, kLevelSpawnData = 0x100;

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
void ProfileAdd(const char* name, uint64_t us);
uint64_t MicrosecondsSince(int64_t qpcStart);
int64_t QpcNow();

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
		const int64_t q0 = QpcNow();
		const SIZE_T got = VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi));
		ProfileAdd("VirtualQuery", MicrosecondsSince(q0));
		if (!got || mbi.State != MEM_COMMIT ||
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
	const std::pair<NativeFn*, const char*> table[] = { { &g_dc.allocVector, "alloc-vector" },
		{ &g_dc.allocQuat, "alloc-quat" }, { &g_dc.allocBoundFrame, "alloc-bound-frame" },
		{ &g_dc.spawnWithLook, "spawn-entity-at-with-look" }, { &g_dc.entityExists, "entity-exists?" },
		{ &g_dc.teleport, "npc-teleport-to" }, { &g_dc.scriptMode, "npc-set-script-mode" },
		{ &g_dc.setIgnore, "npc-set-ignore" }, { &g_dc.setBlind, "npc-set-blind" }, { &g_dc.setDeaf, "npc-set-deaf" },
		{ &g_dc.hitReactions, "npc-enable-hit-reactions" }, { &g_dc.stun, "npc-enable-stun" },
		{ &g_dc.dodges, "npc-enable-dodges" }, { &g_dc.objectLocator, "get-object-locator" },
		{ &g_dc.moveTo, "npc-move-to" }, { &g_dc.newBoxedValue, "new-boxed-value" }, { &g_dc.sendEvent, "send-event" },
		{ &g_dc.setVisible, "set-visible" },
		{ &g_dc.setUnshootable, "set-is-unshootable" }, { &g_dc.darrayCount, "darray-count" },
		{ &g_dc.darrayAt, "darray-at" }, { &g_dc.getHealth, "get-object-health" },
		{ &g_dc.setHealth, "set-object-health" }, { &g_dc.isDead, "character-is-dead?" }, { &g_dc.npcDie, "npc-die" },
		{ &g_dc.taskNode, "task-get-current-node-id" }, { &g_dc.taskSubnode, "task-get-current-subnode-id" },
		{ &g_dc.taskPlay, "task-play-debug!" }, { &g_dc.getScriptMode, "npc-get-script-mode" },
		{ &g_dc.swapLook, "player-swap-look-id" }, { &g_dc.isFiring, "player-is-firing?" },
		{ &g_dc.fireWeapon, "npc-fire-weapon" }, { &g_dc.cameraLocator, "camera-locator" },
		{ &g_dc.artGroup, "get-art-group" },
		{ &g_dc.playerHealth, "get-player-health" },
		{ &g_dc.isCrouched, "player-is-crouched?" }, { &g_dc.isAiming, "player-is-aiming?" },
		{ &g_dc.setDemeanor, "npc-set-demeanor" }, { &g_dc.weaponUp, "npc-weapon-up" }, { &g_dc.weaponDown, "npc-weapon-down" },
		{ &g_dc.getFaction, "npc-get-faction-id" }, { &g_dc.setFaction, "set-object-faction" },
		{ &g_dc.highlight, "character-highlight" }, { &g_dc.allocColor, "alloc-color" },
		{ &g_dc.getDifficulty, "get-difficulty" }, { &g_dc.setDifficulty, "set-difficulty" },
		{ &g_dc.hudMessage, "hud-show-cipher-message" },
		{ &g_dc.playerWeapon, "get-player-weapon-id" }, { &g_dc.npcWeapon, "npc-get-weapon" },
		{ &g_dc.giveWeapon, "npc-give-weapon" }, { &g_dc.destroyWeapon, "npc-destroy-weapon" } };
	for (const auto& [slot, name] : table) {
		*slot = FindNative(name);
		if (!*slot) {
			Log("natives: %s not found", name);
			return false;
		}
		NameNative(*slot, name);
	}
	// Необязательные: без них мод работает, только хуже.
	if ((g_dc.totalAmmo = FindNative("get-total-ammo"))) NameNative(g_dc.totalAmmo, "get-total-ammo");
	else Log("natives: get-total-ammo not found (optional)");
	if ((g_dc.setObjectLocator = FindNative("set-object-locator"))) NameNative(g_dc.setObjectLocator, "set-object-locator");
	else Log("natives: set-object-locator not found (optional)");
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
constexpr int kMaxProfile = 64;
ProfileSlot g_profile[kMaxProfile];
int g_profileCount = 0;
}  // namespace

void ProfileAdd(const char* name, uint64_t us) {
	for (int i = 0; i < g_profileCount; ++i)
		if (!g_profile[i].fn && g_profile[i].name == name) {
			g_profile[i].calls.fetch_add(1, std::memory_order_relaxed);
			g_profile[i].us.fetch_add(us, std::memory_order_relaxed);
			return;
		}
	static SRWLOCK lock = SRWLOCK_INIT;
	AcquireSRWLockExclusive(&lock);
	if (g_profileCount < kMaxProfile) {
		g_profile[g_profileCount].name = name;
		g_profile[g_profileCount].calls = 1;
		g_profile[g_profileCount].us = us;
		++g_profileCount;  // после заполнения: читатели видят слот целиком
	}
	ReleaseSRWLockExclusive(&lock);
}

uint64_t MicrosecondsSince(int64_t qpcStart) {
	LARGE_INTEGER now, f;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&f);
	return static_cast<uint64_t>((now.QuadPart - qpcStart) * 1000000 / f.QuadPart);
}

int64_t QpcNow() {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

void NameNative(NativeFn fn, const char* name) {
	if (fn && g_profileCount < kMaxProfile) {
		g_profile[g_profileCount].fn = fn;
		g_profile[g_profileCount++].name = name;
	}
}

std::string TakeProfile() {
	std::string out;
	int order[kMaxProfile];
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

// camera-locator(0) отдаёт локатор {pos, quat}, как его читает TLOU2-Coop для ников.
bool CameraLocator(Locator& l) {
	const uint64_t p = Call(g_dc.cameraLocator, { 0 });
	return p && Read(p, l) && std::isfinite(l.pos[0]) && std::isfinite(l.rot[3]);
}

CameraView CurrentView() {
	CameraView v{};
	v.ok = CameraLocator(v.cam);
	const float* q = v.cam.rot;  // (0,0,1), повёрнутый кватернионом камеры
	v.fwd[0] = 2.0f * (q[0] * q[2] + q[3] * q[1]);
	v.fwd[1] = 2.0f * (q[1] * q[2] - q[3] * q[0]);
	v.fwd[2] = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
	return v;
}

bool Seen(const CameraView& v, const float p[3], float maxDist) {
	if (!v.ok) return true;
	const float d[3] = { p[0] - v.cam.pos[0], p[1] - v.cam.pos[1], p[2] - v.cam.pos[2] };
	const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len > maxDist) return false;
	if (len < 2.0f) return true;
	return (d[0] * v.fwd[0] + d[1] * v.fwd[1] + d[2] * v.fwd[2]) > len * 0.34f;  // cos 70°
}

namespace {
bool LookupHandleRaw(uintptr_t fnAddr, uint64_t sid, uintptr_t& outEntry, uint32_t& outId) {
	struct Handle { uintptr_t entry; uint32_t id; };
	using Fn = Handle*(__fastcall*)(Handle* out, uint64_t sid);
	Handle buf{};
	__try {
		Handle* res = reinterpret_cast<Fn>(fnAddr)(&buf, sid);
		if (!res || !res->entry || !res->id) return false;
		outEntry = res->entry;
		outId = res->id;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}
}  // namespace

uintptr_t PlayerEntity() { return Player(); }

uintptr_t ResolveEntity(uint64_t sid) {
	if (!g_base || !sid) return 0;
	const uintptr_t fnAddr = ReadPtr(g_base + kLookupProcessFnRva);
	uintptr_t entry = 0;
	uint32_t id = 0;
	if (!fnAddr || !LookupHandleRaw(fnAddr, sid, entry, id)) return 0;
	const uintptr_t obj = ReadPtr(entry + 8);
	uint32_t objId = 0;
	return obj && Read(obj + kProcessIdOffset, objId) && objId == id ? obj : 0;
}

bool PlayerLocator(Locator& l) {
	const uintptr_t p = Player();
	return p && Read(p + kLocatorOffset, l) && std::isfinite(l.pos[0]) && std::isfinite(l.pos[2]);
}

// get-object-locator отдаёт копию bound-frame объекта (объект+0x180, 0x50 байт) во временной
// памяти кадра; сам локатор {pos, quat} в ней на +0x30 (у объекта это +0x1B0).
// Указатель вернула сама игра и он живёт до конца кадра: читаем без VirtualQuery (проверка
// адреса здесь только тратила время скриптового потока).
bool NpcLocator(uint64_t npc, Locator& out) {
	const uint64_t p = Call(g_dc.objectLocator, { npc });
	if (p < 0x10000) return false;
	memcpy(&out, reinterpret_cast<const void*>(p + 0x30), sizeof(out));
	return std::isfinite(out.pos[0]) && std::isfinite(out.pos[2]);
}

uint64_t BoundFrame(const float pos[3], const float rot[4]) {
	const uint64_t v = Call(g_dc.allocVector, { F(pos[0]), F(pos[1]), F(pos[2]), F(1.0f), 0 });
	const uint64_t q = Call(g_dc.allocQuat, { F(rot[0]), F(rot[1]), F(rot[2]), F(rot[3]), 0 });
	return v && q ? Call(g_dc.allocBoundFrame, { v, q, 0, 0, 0 }) : 0;
}

Chapter CurrentChapter() { return { Call(g_dc.taskNode, {}), Call(g_dc.taskSubnode, {}) }; }

void PlayChapter(const Chapter& c) { Call(g_dc.taskPlay, { c.node, c.task, 0 }); }

std::string HeroName(const std::string& art) {
	if (art.rfind("t1x-", 0) != 0) return {};
	const size_t end = art.find('-', 4);
	return art.substr(4, end == std::string::npos ? std::string::npos : end - 4);
}

std::vector<uint64_t> CharacterSids(const std::string& character) {
	std::vector<uint64_t> out;
	if (character.empty()) return out;
	ForEachSpawner([&](uintptr_t, const SpawnerHead& h) {
		if (h.type && HeroName(ReadName(h.art)) == character) out.push_back(h.sid);
		return out.size() < 64;
	});
	return out;
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

uint64_t CurrentBuddy(uint64_t exclude) {
	const uint64_t list = Sid("all-buddy-npcs");
	const uint64_t n = (std::min)(Call(g_dc.darrayCount, { list }), uint64_t{ 16 });
	for (uint64_t i = 0; i < n; ++i) {
		uint64_t npc = 0;
		const uint64_t at = Call(g_dc.darrayAt, { list, i });
		if (!at || !Read(at, npc) || !npc || npc == exclude) continue;
		if (Call(g_dc.entityExists, { npc }) && !(Call(g_dc.isDead, { npc }) & 1)) return npc;
	}
	return 0;
}

std::string ArtOfCharacter(const std::string& character) {
	std::string art;
	ForEachSpawner([&](uintptr_t, const SpawnerHead& h) {
		std::string a = ReadName(h.art);
		if (HeroName(a) != character || a.find("-costume") == std::string::npos) return true;
		art = std::move(a);
		return false;
	});
	return art;
}

// Имя NPC, созданного спавнером, совпадает с sid спавнера.
std::string ArtOfNpc(uint64_t npc) {
	std::string art;
	ForEachSpawner([&](uintptr_t, const SpawnerHead& h) {
		if (h.sid != npc) return true;
		art = ReadName(h.art);
		return false;
	});
	return art;
}

std::string PlayerLook(const float around[3]) {
	const uint64_t group = Call(g_dc.artGroup, { Sid("player") });
	std::string art;
	if (group)
		ForEachSpawner([&](uintptr_t, const SpawnerHead& h) {
			std::string a = ReadName(h.art);
			if (a.empty() || Sid(a.c_str()) != group) return true;
			art = std::move(a);
			return false;
		});
	static uint64_t loggedGroup = 0;
	if (group != loggedGroup) {
		loggedGroup = group;
		// В лог один раз на облик: видно, сработало ли сопоставление.
		char buf[96];
		snprintf(buf, sizeof(buf), "look: player art group %016llX -> %s", static_cast<unsigned long long>(group),
			art.empty() ? "(no match, nearest player start)" : art.c_str());
		Log("%s", buf);
	}
	return art.empty() ? HeroLook(around) : art;
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
