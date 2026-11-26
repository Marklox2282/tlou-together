#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "enemies.hpp"
#include "log.hpp"
#include "puppet.hpp"

using namespace engine;

namespace puppet {

namespace {

// ---- след ----------------------------------------------------------------------------------
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

// ---- состояние -----------------------------------------------------------------------------
std::atomic<bool> g_wanted{ false }, g_follow{ false }, g_teleport{ false }, g_netActive{ false };
std::atomic<uint64_t> g_myChapter{ 0 }, g_partnerChapter{ 0 }, g_puppet{ 0 }, g_doneFrame{ 0 };
// Повторные попытки: спавнер, чья марионетка сразу исчезла (сюжетный NPC, которого скрипт
// убирает, - npc-robert-1 в Бостоне), больше не берём; пауза растёт после неудач подряд, иначе
// пересоздание каждые 3 с проседало FPS. Список сбрасывается при смене задания сюжета.
ULONGLONG g_lastSpawnTry = 0, g_spawnedAt = 0;
uintptr_t g_lastSpawner = 0;
std::vector<uintptr_t> g_badSpawners;
int g_failures = 0;
uint64_t g_badForChapter = 0;

// Копия NPC без собственной воли: тот же набор, что у марионетки в TLOU2-Coop.
uint64_t Spawn(const Locator& at) {
	Locator me{};
	std::string look = PlayerLocator(me) ? HeroLook(me.pos) : std::string();
	std::string name;
	if (g_badForChapter != g_myChapter) {
		g_badSpawners.clear();
		g_badForChapter = g_myChapter;
	}
	// Персонаж, который сейчас сам есть на уровне по сюжету (Роберт в его миссии), не копируем:
	// игрок видел "лишнего" Роберта. Имя (sid) NPC спавнера совпадает с sid самого спавнера.
	uintptr_t sp = 0;
	// Сюжетного персонажа проверяем по всем его спавнерам: настоящую Марлин уровень создал из
	// другого спавнера, и рядом с ней появлялась вторая.
	auto inScene = [](uintptr_t spawner) {
		uint64_t spSid = 0;
		if (Read(spawner + kSpSid, spSid) && Call(g_dc.entityExists, { spSid })) return true;
		for (uint64_t sid : CharacterSids(HeroName(ReadName(ReadPtr(spawner + kSpArt)))))
			if (Call(g_dc.entityExists, { sid })) return true;
		return false;
	};
	for (int tries = 0; tries < 8; ++tries) {
		sp = FindBuddySpawner(HeroName(look), g_badSpawners, name);
		if (!sp || !inScene(sp)) break;
		Log("puppet: %s is in the scene already, skipping", name.c_str());
		g_badSpawners.push_back(sp);
		sp = 0;
	}
	g_lastSpawner = sp;
	// Образ героя - только на скелет того же пола; иначе у NPC остаётся его собственный образ.
	if (!SameSex(ReadName(ReadPtr(sp + kSpArt)), HeroName(look))) look.clear();
	uint64_t sid = 0;
	uint8_t f1 = 0;
	if (!sp || !Read(sp + kSpSid, sid) || !Read(sp + kSpFlags1, f1)) {
		Log("puppet: no buddy spawner in loaded levels");
		return 0;
	}
	// До вызова движка: если он упадёт, в логе будет видно, на каком спавнере.
	Log("puppet: spawning from %s, look %s", name.c_str(), look.empty() ? "-" : look.c_str());
	// Бит "multi-spawn": иначе движок переиспользует и телепортирует сюжетного NPC уровня.
	auto* flags1 = reinterpret_cast<uint8_t*>(sp + kSpFlags1);
	*flags1 = static_cast<uint8_t>(f1 | kSpMultiSpawn);
	const uint64_t frame = BoundFrame(at.pos, at.rot);
	// Сначала с внешностью героя; если движок такой образ не примет - образ самого спавнера.
	uint64_t npc = frame && !look.empty() ? Call(g_dc.spawnWithLook, { sid, frame, Sid(look.c_str()) }) : 0;
	if (!look.empty()) Log("puppet: look %s %s", look.c_str(), npc && npc != sid ? "applied" : "refused");
	if (!npc || npc == sid) npc = frame ? Call(g_dc.spawnWithLook, { sid, frame, 0 }) : 0;
	*flags1 = f1;
	if (!npc || npc == sid) {
		Log("puppet: spawn from %s failed (npc=%016llX)", name.c_str(), static_cast<unsigned long long>(npc));
		return 0;
	}
	Call(g_dc.scriptMode, { npc, 2, 1 });  // выключить ИИ и сюжетные команды
	for (NativeFn on : { g_dc.setIgnore, g_dc.setBlind, g_dc.setDeaf }) Call(on, { npc, 1 });
	for (NativeFn off : { g_dc.hitReactions, g_dc.stun, g_dc.dodges }) Call(off, { npc, 0 });
	Call(g_dc.setUnshootable, { npc, 1 });  // напарника не подстрелить
	Log("puppet: %s -> %016llX", name.c_str(), static_cast<unsigned long long>(npc));
	return npc;
}

// Друг далеко (другая глава или другой конец уровня): его координаты могут быть в
// незагруженной части мира. Марионетку не создаём и не двигаем, иначе снап утащил бы её туда.
// Сверка по заданию сюжета не годится: id задания меняется каждые несколько секунд (подзадачи),
// и отставший на полминуты друг считался бы "в другой главе".
bool PartnerTooFar(const Locator& target) {
	constexpr float kFar = 300.0f;
	Locator me{};
	const bool tooFar = UsingNetwork() && PlayerLocator(me) && Dist(me.pos, target.pos) > kFar;
	static bool logged = false;
	if (tooFar != logged) {
		logged = tooFar;
		if (tooFar)
			Log("puppet: partner is more than %.0f m away (task %016llX, mine %016llX); waiting", kFar,
				static_cast<unsigned long long>(g_partnerChapter.load()), static_cast<unsigned long long>(g_myChapter.load()));
		else
			Log("puppet: partner is near again");
	}
	return tooFar;
}

// F10 выкл: марионетка прячется (стоп, невидимость, без столкновений), а не удаляется:
// kill-entity посреди кадра ронял игру - движок ещё обновлял анимацию NPC в другом потоке
// (npc.cpp PostAnimUpdate_Async). При F10 вкл та же марионетка появляется снова; при
// перезагрузке уровня игра убирает её сама.
constexpr uint64_t kBoxedSymbol = 7;
std::atomic<bool> g_hidden{ false };

void Hide() {
	const uint64_t puppet = g_puppet;
	if (!puppet || g_hidden || !Call(g_dc.entityExists, { puppet })) return;
	Locator here{};
	if (NpcLocator(puppet, here))
		if (const uint64_t frame = BoundFrame(here.pos, here.rot)) Call(g_dc.moveTo, { puppet, frame, 0, 0, 0, 0, 0, 0 });
	Call(g_dc.setVisible, { puppet, 0 });
	if (const uint64_t none = Call(g_dc.newBoxedValue, { kBoxedSymbol, 0 }))
		Call(g_dc.sendEvent, { Sid("unphysicalize"), puppet, none });
	g_hidden = true;
	Log("puppet %016llX hidden", static_cast<unsigned long long>(puppet));
}

void Show(uint64_t puppet, const Locator& at) {
	Call(g_dc.sendEvent, { Sid("physicalize"), puppet });
	Call(g_dc.setVisible, { puppet, 1 });
	if (const uint64_t frame = BoundFrame(at.pos, at.rot)) Call(g_dc.teleport, { puppet, frame });
	g_hidden = false;
	Log("puppet %016llX shown again", static_cast<unsigned long long>(puppet));
}

void Update() {
	const ULONGLONG now = GetTickCount64();
	uint64_t puppet = g_puppet;
	// entity-exists? ищет объект по имени среди всех объектов уровня: каждый кадр это стоило
	// миллисекунды скриптового потока. Сразу после появления - часто (уровень может тут же
	// удалить сюжетного NPC), потом раз в полсекунды.
	static ULONGLONG lastExistsCheck = 0;
	const bool checkNow = now - g_spawnedAt < 500 || now - lastExistsCheck >= 500;
	if (checkNow) lastExistsCheck = now;
	if (puppet && checkNow && !Call(g_dc.entityExists, { puppet })) {
		const bool instant = now - g_spawnedAt < 2000;
		Log("puppet %016llX is gone %s", static_cast<unsigned long long>(puppet),
			instant ? "right after spawning: the level removes it, trying another spawner" : "(death or checkpoint reload)");
		if (instant && g_lastSpawner) {
			g_badSpawners.push_back(g_lastSpawner);
			++g_failures;
		}
		g_puppet = puppet = 0;
		g_hidden = false;
	}
	Locator target{};
	if (!DelayedTarget(now, target) || PartnerTooFar(target)) return;
	if (!puppet) {
		if (now - g_lastSpawnTry < (g_failures >= 3 ? 15000u : 3000u)) return;
		g_puppet = puppet = Spawn(target);
		const ULONGLONG took = GetTickCount64() - now;
		if (took > 20) Log("puppet: spawn attempt took %llu ms", took);
		g_lastSpawnTry = g_spawnedAt = GetTickCount64();
		if (!puppet) ++g_failures;
		return;
	}
	if (g_hidden) Show(puppet, target);
	if (now - g_spawnedAt > 2000) g_failures = 0;
	// Марионетка идёт сама своей анимацией: npc-move-to(npc, frame, режим, 0...), где режим
	// 0 шаг, 1 бег, 2 спринт (замерено в TLOU2-Coop). Телепорт - только при сильном отставании:
	// телепорт на каждом кадре ронял игру в Fsm::StateMachineBase::SwitchToNewState.
	static ULONGLONG lastCmdAt = 0, lastSnapAt = 0;
	static float lastGoal[3]{};
	static int lastMode = -1;
	static bool wasFollowing = false;
	if (!g_follow) {
		// Последний npc-move-to иначе дорабатывает до цели: даём новую цель - там, где стоит.
		Locator here{};
		if (wasFollowing && NpcLocator(puppet, here))
			if (const uint64_t frame = BoundFrame(here.pos, here.rot)) Call(g_dc.moveTo, { puppet, frame, 0, 0, 0, 0, 0, 0 });
		wasFollowing = false;
		lastMode = -1;
		return;
	}
	wasFollowing = true;
	// Не чаще 10 раз в секунду, даже если команда не понадобилась (раньше пауза отсчитывалась
	// только от команды, и пока марионетка стояла рядом, позиция читалась каждый кадр).
	static ULONGLONG lastCheckAt = 0;
	if (now - lastCmdAt < 100 || now - lastCheckAt < 100) return;
	lastCheckAt = now;
	Locator cur{}, before{};
	if (!NpcLocator(puppet, cur) || !DelayedTarget(now, before, 250)) {
		static ULONGLONG lastWarn = 0;
		if (now - lastWarn > 5000) Log("puppet: can't read its position");
		lastWarn = now;
		return;
	}
	const float err = Dist(cur.pos, target.pos);
	const float targetSpeed = Dist(before.pos, target.pos) / 0.25f;
	// Отстала - догоняет бегом/спринтом (режим растёт с отрывом, см. ниже). Телепорт - только
	// если отрыв больше 15 м или она застряла: 2 с отстаёт больше чем на 3 м и не приближается
	// (прыжки, лестницы, обрывы - туда npc-move-to её не ведёт).
	// Застряла: 2 с сама почти не сдвинулась (меньше 0.5 м), хотя отстаёт больше чем на 3 м.
	// Сокращение отрыва не годится: когда игрок бежит быстрее, отрыв растёт и у бегущей.
	static float movedFrom[3]{};
	static ULONGLONG movedAt = 0;
	if (Dist(cur.pos, movedFrom) > 0.5f || err < 3.0f || !movedAt) {
		memcpy(movedFrom, cur.pos, sizeof(movedFrom));
		movedAt = now;
	}
	const bool stuck = err > 3.0f && now - movedAt > 2000;
	// Телепорт, после которого она так и осталась далеко (точка недоступна), не повторяем каждые
	// 2 с: после двух таких подряд - пауза 10 с.
	static int uselessSnaps = 0;
	static float errAtSnap = 0;
	static bool judged = true;
	if (!judged && now - lastSnapAt > 500) {  // каждый телепорт оцениваем один раз
		judged = true;
		uselessSnaps = err > errAtSnap - 1.0f ? uselessSnaps + 1 : 0;
		if (uselessSnaps == 2) Log("puppet: teleport does not bring it closer, retrying every 10 s");
	}
	if (err > 15.0f || stuck) {
		movedAt = 0;
		if (now - lastSnapAt < (uselessSnaps >= 2 ? 10000u : 1000u)) return;
		errAtSnap = err;
		judged = false;
		lastSnapAt = lastCmdAt = now;
		lastMode = -1;
		if (const uint64_t frame = BoundFrame(target.pos, target.rot)) Call(g_dc.teleport, { puppet, frame });
		Log("puppet: snapped (%.1f m behind%s)", err, stuck ? ", stuck" : "");
		return;
	}
	if (targetSpeed < 0.4f && err < 1.0f) return;  // стоим рядом - ничего не делаем
	const float speed = (std::max)(targetSpeed, err * 0.8f);
	const int mode = speed < 1.8f ? 0 : (speed < 4.2f ? 1 : 2);
	if (mode == lastMode && Dist(target.pos, lastGoal) < 0.75f && now - lastCmdAt < 1500) return;
	if (const uint64_t frame = BoundFrame(target.pos, target.rot))
		Call(g_dc.moveTo, { puppet, frame, static_cast<uint64_t>(mode), 0, 0, 0, 0, 0 });
	memcpy(lastGoal, target.pos, sizeof(lastGoal));
	if (mode != lastMode) Log("puppet: move mode %d (target %.1f m/s, %.1f m away)", mode, targetSpeed, err);
	lastMode = mode;
	lastCmdAt = now;
}

// F7: перенести игрока к напарнику (к марионетке), на метр в сторону. Событие "teleport-player"
// с bound-frame в коробке, как в TLOU2-Coop; тип 18 = bound-frame, проверено в Part I.
constexpr uint64_t kBoxedBoundFrame = 18;

void TeleportToPartner() {
	Locator partner{};
	const uint64_t puppet = g_puppet;
	if (!puppet || !NpcLocator(puppet, partner)) {
		Log("F7: no partner to teleport to");
		return;
	}
	partner.pos[0] += 1.0f;
	const uint64_t frame = BoundFrame(partner.pos, partner.rot);
	const uint64_t boxed = frame ? Call(g_dc.newBoxedValue, { kBoxedBoundFrame, frame }) : 0;
	if (boxed) Call(g_dc.sendEvent, { Sid("teleport-player"), Sid("player"), boxed });
	Log("F7: teleport to partner at (%.1f %.1f %.1f) %s", partner.pos[0], partner.pos[1], partner.pos[2], boxed ? "sent" : "FAILED");
}

void ReadChapter() {
	static ULONGLONG last = 0;
	const ULONGLONG now = GetTickCount64();
	if (now - last < 1000) return;
	last = now;
	const uint64_t chapter = CurrentChapter();
	if (chapter != g_myChapter.exchange(chapter))
		Log("story task %016llX", static_cast<unsigned long long>(chapter));
}

}  // namespace

void Record(uint64_t nowMs, const Locator& l) {
	AcquireSRWLockExclusive(&g_trailLock);
	const Sample& last = g_trail[(g_trailHead + kTrail - 1) % kTrail];
	if (nowMs - last.t >= 15) g_trail[g_trailHead++ % kTrail] = { nowMs, l };
	ReleaseSRWLockExclusive(&g_trailLock);
}

void UseNetwork(bool on) { ClearTrail(on ? kNetDelayMs : kLocalDelayMs); }
bool UsingNetwork() { return g_trailDelay.load() == kNetDelayMs; }
void SetWanted(bool on) { g_wanted = on; }
bool Wanted() { return g_wanted; }
void SetFollow(bool on) { g_follow = on; }
bool Follow() { return g_follow; }
void RequestTeleport() { g_teleport = true; }
void SetNetActive(bool on) { g_netActive = on; }
void SetPartnerChapter(uint64_t chapter) { g_partnerChapter = chapter; }
uint64_t MyChapter() { return g_myChapter; }
uint64_t Current() { return g_puppet; }

// Свой номер кадра: хук интерпретатора срабатывает на каждом вызове скриптовой функции (тысячи
// за кадр на всех потоках), и проверка там должна стоить одно сравнение. Раньше здесь читался
// счётчик кадров игры через проверяемое чтение памяти, и FPS заметно проседал.
std::atomic<uint64_t> g_frame{ 0 };
std::atomic<uint64_t> g_workUs{ 0 };

void OnFrame() { g_frame.fetch_add(1, std::memory_order_relaxed); }
uint64_t TakeWorkMicroseconds() { return g_workUs.exchange(0); }

// Скрипты выполняются на нескольких потоках; работа - один раз за кадр, первым пришедшим.
void ScriptTick() {
	if (!g_wanted.load(std::memory_order_relaxed) && !g_teleport.load(std::memory_order_relaxed) &&
		!g_netActive.load(std::memory_order_relaxed) && !g_puppet.load(std::memory_order_relaxed))
		return;
	const uint64_t frame = g_frame.load(std::memory_order_relaxed);
	uint64_t done = g_doneFrame.load(std::memory_order_relaxed);
	if (done == frame || !g_doneFrame.compare_exchange_strong(done, frame)) return;
	// Работа может ещё идти в другом потоке, когда начался следующий кадр: у неё общее
	// состояние, поэтому один поток за раз.
	static std::atomic_flag busy = ATOMIC_FLAG_INIT;
	if (busy.test_and_set(std::memory_order_acquire)) return;
	LARGE_INTEGER a, b, f;
	QueryPerformanceCounter(&a);
	int64_t s = QpcNow();
	ReadChapter();
	ProfileAdd("[chapter]", MicrosecondsSince(s));
	if (g_teleport.exchange(false)) TeleportToPartner();
	if (!g_wanted && g_puppet) Hide();
	s = QpcNow();
	if (g_wanted) Update();
	ProfileAdd("[update]", MicrosecondsSince(s));
	if (g_netActive) enemies::Tick(g_puppet);
	QueryPerformanceCounter(&b);
	QueryPerformanceFrequency(&f);
	g_workUs.fetch_add(static_cast<uint64_t>((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart), std::memory_order_relaxed);
	busy.clear(std::memory_order_release);
}

}  // namespace puppet
