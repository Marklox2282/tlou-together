#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

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
ULONGLONG g_lastSpawnTry = 0;

// Копия NPC без собственной воли: тот же набор, что у марионетки в TLOU2-Coop.
uint64_t Spawn(const Locator& at) {
	Locator me{};
	const std::string look = PlayerLocator(me) ? HeroLook(me.pos) : std::string();
	std::string name;
	const uintptr_t sp = FindBuddySpawner(HeroName(look), name);
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
	Log("puppet: %s -> %016llX", name.c_str(), static_cast<unsigned long long>(npc));
	return npc;
}

// Друг в другой главе: его координаты относятся к другому уровню. Марионетку не создаём и не
// двигаем (снап туда утащил бы её в незагруженное место).
bool ChapterMismatch() {
	const uint64_t mine = g_myChapter, theirs = g_partnerChapter;
	const bool mismatch = mine && theirs && mine != theirs;
	static bool logged = false;
	if (mismatch != logged) {
		logged = mismatch;
		if (mismatch)
			Log("puppet: partner is in another part of the story (task %016llX, mine %016llX); waiting",
				static_cast<unsigned long long>(theirs), static_cast<unsigned long long>(mine));
		else
			Log("puppet: partner is in the same part of the story now");
	}
	return mismatch;
}

void Update() {
	const ULONGLONG now = GetTickCount64();
	uint64_t puppet = g_puppet;
	if (puppet && !Call(g_dc.entityExists, { puppet })) {
		Log("puppet %016llX is gone (death or checkpoint reload)", static_cast<unsigned long long>(puppet));
		g_puppet = puppet = 0;
	}
	Locator target{};
	if (ChapterMismatch() || !DelayedTarget(now, target)) return;
	if (!puppet) {
		if (now - g_lastSpawnTry < 3000) return;
		g_puppet = Spawn(target);
		const ULONGLONG took = GetTickCount64() - now;
		if (took > 20) Log("puppet: spawn attempt took %llu ms", took);
		g_lastSpawnTry = GetTickCount64();
		return;
	}
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
	if (now - lastCmdAt < 100) return;
	Locator cur{}, before{};
	if (!NpcLocator(puppet, cur) || !DelayedTarget(now, before, 250)) {
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
		if (const uint64_t frame = BoundFrame(target.pos, target.rot)) Call(g_dc.teleport, { puppet, frame });
		Log("puppet: snapped (%.1f m behind)", err);
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

// Скрипты выполняются на нескольких потоках; работа - один раз за кадр, первым пришедшим.
void ScriptTick() {
	if (!g_wanted.load(std::memory_order_relaxed) && !g_teleport.load(std::memory_order_relaxed) &&
		!g_netActive.load(std::memory_order_relaxed))
		return;
	const uint64_t frame = FrameCounter();
	uint64_t done = g_doneFrame.load(std::memory_order_relaxed);
	if (!frame || done == frame || !g_doneFrame.compare_exchange_strong(done, frame)) return;
	ReadChapter();
	if (g_teleport.exchange(false)) TeleportToPartner();
	if (g_wanted) Update();
}

}  // namespace puppet
