#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "anim.hpp"
#include "enemies.hpp"
#include "log.hpp"
#include "objects.hpp"
#include "menu.hpp"
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
uint32_t g_senderBase = 0, g_lastSenderMs = 0;
ULONGLONG g_localBase = 0, g_lastArrivalMs = 0;

struct LocalAnimFrame { ULONGLONG t; std::vector<uint8_t> data; };
std::deque<LocalAnimFrame> g_localAnim;
SRWLOCK g_localAnimLock = SRWLOCK_INIT;

void ClearTrail(ULONGLONG delay) {
	AcquireSRWLockExclusive(&g_trailLock);
	memset(g_trail, 0, sizeof(g_trail));
	g_trailHead = 0;
	g_senderBase = g_lastSenderMs = 0;
	g_localBase = g_lastArrivalMs = 0;
	g_trailDelay = delay;
	ReleaseSRWLockExclusive(&g_trailLock);
	AcquireSRWLockExclusive(&g_localAnimLock);
	g_localAnim.clear();
	ReleaseSRWLockExclusive(&g_localAnimLock);
}

void LerpLocator(const Locator& a, const Locator& b, float alpha, Locator& out) {
	for (int i = 0; i < 3; ++i) out.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * alpha;
	out.pos[3] = a.pos[3];
	const float dot = a.rot[0] * b.rot[0] + a.rot[1] * b.rot[1] + a.rot[2] * b.rot[2] + a.rot[3] * b.rot[3];
	const float sign = dot < 0.0f ? -1.0f : 1.0f;
	float lenSq = 0.0f;
	for (int i = 0; i < 4; ++i) {
		out.rot[i] = a.rot[i] + (b.rot[i] * sign - a.rot[i]) * alpha;
		lenSq += out.rot[i] * out.rot[i];
	}
	if (lenSq > 1e-8f) {
		const float inv = 1.0f / std::sqrt(lenSq);
		for (int i = 0; i < 4; ++i) out.rot[i] *= inv;
	}
}

bool DelayedTarget(ULONGLONG now, Locator& out, ULONGLONG extra = 0) {
	const ULONGLONG delay = g_trailDelay.load() + extra;
	const ULONGLONG want = now > delay ? now - delay : 0;
	AcquireSRWLockShared(&g_trailLock);
	const Sample* older = nullptr;
	const Sample* newer = nullptr;
	for (int i = 1; i <= kTrail; ++i) {
		const Sample& s = g_trail[(g_trailHead - i + kTrail * 2) % kTrail];
		if (!s.t) break;
		if (s.t <= want) {
			older = &s;
			break;
		}
		newer = &s;
	}
	bool found = false;
	if (older && newer && newer->t > older->t) {
		const float alpha = static_cast<float>(want - older->t) / static_cast<float>(newer->t - older->t);
		LerpLocator(older->l, newer->l, std::clamp(alpha, 0.0f, 1.0f), out);
		found = true;
	} else if (older) {
		out = older->l;
		found = true;
	} else if (newer) {
		out = newer->l;
		found = true;
	}
	ReleaseSRWLockShared(&g_trailLock);
	return found;
}

float Dist(const float a[3], const float b[3]) {
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Разница поворотов вокруг вертикали (радианы, -pi..pi) по кватернионам {x, y, z, w}.
float YawDiff(const float a[4], const float b[4]) {
	auto yaw = [](const float q[4]) { return std::atan2(2.0f * (q[3] * q[1] + q[0] * q[2]), 1.0f - 2.0f * (q[1] * q[1] + q[0] * q[0])); };
	float d = yaw(a) - yaw(b);
	while (d > 3.14159265f) d -= 6.28318531f;
	while (d < -3.14159265f) d += 6.28318531f;
	return d;
}

// ---- состояние -----------------------------------------------------------------------------
std::atomic<bool> g_wanted{ false }, g_follow{ false }, g_teleport{ false }, g_netActive{ false };
std::atomic<uint64_t> g_puppet{ 0 }, g_doneFrame{ 0 };
std::atomic<float> g_lastErrM{ 0.0f };
// Место в сюжете: своё (читается в скриптовом потоке) и партнёра (из сети, игровой кадр).
SRWLOCK g_chapterLock = SRWLOCK_INIT;
Chapter g_myChapter, g_partnerChapter;

Chapter LoadChapter(const Chapter& c) {
	AcquireSRWLockShared(&g_chapterLock);
	const Chapter out = c;
	ReleaseSRWLockShared(&g_chapterLock);
	return out;
}

void StoreChapter(Chapter& c, const Chapter& v) {
	AcquireSRWLockExclusive(&g_chapterLock);
	c = v;
	ReleaseSRWLockExclusive(&g_chapterLock);
}
// Повторные попытки: спавнер, чья марионетка сразу исчезла (сюжетный NPC, которого скрипт
// убирает, - npc-robert-1 в Бостоне), больше не берём; пауза растёт после неудач подряд, иначе
// пересоздание каждые 3 с проседало FPS. Список сбрасывается при смене задания сюжета.
ULONGLONG g_lastSpawnTry = 0, g_spawnedAt = 0;
uintptr_t g_lastSpawner = 0;
std::vector<uintptr_t> g_badSpawners;
std::vector<std::string> g_badHeroes;  // сюжетные персонажи, чей спавнер отдаёт настоящего спутника
int g_failures = 0;
uint64_t g_badForChapter = 0;

// ---- роли ----------------------------------------------------------------------------------
std::atomic<int> g_myChar{ 0 }, g_partnerChar{ 0 };
int g_puppetChar = -1;  // для какого выбора партнёра создана копия NPC
SRWLOCK g_lookLock = SRWLOCK_INIT;
std::string g_myLook, g_partnerLook, g_puppetLook;  // свой облик, присланный партнёром, запрошенный для копии
// Убранные под уровень копии: спавнер может вернуть ту же сущность снова - её не берём.
std::vector<uint64_t> g_putAway;

std::string Locked(const std::string& s) {
	AcquireSRWLockShared(&g_lookLock);
	std::string out = s;
	ReleaseSRWLockShared(&g_lookLock);
	return out;
}
std::atomic<bool> g_meFiring{ false }, g_partnerFiring{ false };
std::atomic<uint64_t> g_myWeapon{ 0 }, g_partnerWeapon{ 0 };
uint64_t g_buddyWeapon = 0;  // оружие спутника до нас - вернуть при отпускании
std::atomic<bool> g_enemiesSee{ true }, g_outline{ true }, g_animSync{ false };
std::atomic<int> g_myDifficulty{ -1 }, g_wantDifficulty{ -1 };
std::atomic<int> g_ownDifficulty{ -1 };  // своя сложность до хоста - вернуть при отключении
uint64_t g_buddyFaction = 0;  // фракция спутника до нас - вернуть при отпускании
std::atomic<bool> g_meCrouched{ false }, g_meAiming{ false }, g_partnerCrouched{ false }, g_partnerAiming{ false };
std::atomic<bool> g_meFlashlight{ false }, g_partnerFlashlight{ false }, g_partnerInCutscene{ false };
std::atomic<bool> g_meHealing{ false }, g_partnerHealing{ false };
SRWLOCK g_throwLock = SRWLOCK_INIT;
std::vector<Throw> g_myThrows, g_partnerThrows;  // под g_throwLock
// Бросаемые предметы Part I (имена из инвентаря игрока): их "патроны" убывают при броске.
bool IsThrowable(uint64_t w) {
	return w == Sid("t1x-brick") || w == Sid("t1x-bottle") || w == Sid("t1x-molotov") || w == Sid("t1x-nail-bomb") ||
		w == Sid("t1x-smoke-bomb") || w == Sid("brick") || w == Sid("bottle") || w == Sid("molotov") || w == Sid("smoke-bomb");
}
std::atomic<float> g_meAimPitch{ 0.0f }, g_partnerAimPitch{ 0.0f };
std::atomic<bool> g_inCutscene{ false };
// Подъём по лестнице, прыжок, перелезание, перенос доски: в это время npc-move-to копию туда не
// ведёт, поэтому копию ставим точно по позиции партнёра и (если стоит хук) берём его анимацию.
std::atomic<bool> g_meTraversing{ false }, g_climbAnims{ true };
std::atomic<ULONGLONG> g_meTraversalAt{ 0 }, g_partnerTraversalAt{ 0 };
// Состояния анимации игрока (из anim-player*.bin игры), при которых он лезет/прыгает/падает.
const char* const kTraversalStates[] = {
	"s_climb-idle-shake", "s_climb-idle-shake-m", "s_climb-reach-back-shake", "s_climb-reach-shake-1000",
	"s_climb-reach-shake-1200", "s_climb-reach-shake-130", "s_climb-reach-shake-300", "s_climb-reach-shake-430",
	"s_climb-reach-shake-600", "s_climb-reach-shake-foot", "s_fall-slope", "s_ground-landing",
	"s_hit-wall-jump-up", "s_hit-wall-jump-up^fall", "s_ladder-climb-dn", "s_ladder-climb-dn-1^2rung",
	"s_ladder-climb-dn-2^1rung", "s_ladder-climb-dn-2rung", "s_ladder-climb-dn-slide",
	"s_ladder-climb-off-top-fall", "s_ladder-climb-off-top-stand", "s_ladder-climb-up",
	"s_ladder-climb-up-1^2rung", "s_ladder-climb-up-2^1rung", "s_ladder-climb-up-2rung", "s_ladder-drop",
	"s_ladder-enter-bot", "s_ladder-enter-bot-bw", "s_ladder-enter-jump-landing", "s_ladder-enter-placeholder",
	"s_ladder-enter-top", "s_ladder-idle", "s_ladder-look-down", "s_ladder-swim-enter-bot",
	"s_ledge-climb-up^stand", "s_ledge-climb-up^vault", "s_ledge-drop-start", "s_pickup-plank-partial",
	"s_pickup-plank-single", "s_piton-drop-to-wall-land", "s_piton-jump-to-wall-land", "s_pre-ledge-climb-up",
	"s_prone-exit-fall", "s_putdown-plank-single", "s_rope-climb-2-75m", "s_rope-climb-dn",
	"s_rope-climb-dn^idle", "s_rope-climb-drop-down", "s_rope-climb-drop-down-backwards",
	"s_rope-climb-drop-down-left", "s_rope-climb-drop-down-right", "s_rope-climb-exit-dn^idle",
	"s_rope-climb-idle", "s_rope-climb-ledge-chest^knee", "s_rope-climb-ledge-idle^chest",
	"s_rope-climb-ledge-knee^idle", "s_rope-climb-start-custom", "s_rope-climb-start-fall",
	"s_rope-climb-start-swim", "s_rope-climb-swing", "s_rope-climb-up", "s_rope-climb-up^idle", "s_slide-fall",
	"s_slide-intro", "s_slide-land", "s_slide-loop-l", "s_slide-loop-r", "s_slide-run-up-intro",
	"s_slide-sit-land", "s_slide-sit-loop-r", "s_wall-landing", "s_jump", "s_jump-up", "s_jump-down", "s_vault",
	// Захваты и удушения (anim-player/player-grapple): тоже копируем анимацию.
	"s_grapple-180", "s_grapple-aim-idle", "s_grapple-aim^stand", "s_grapple-counter", "s_grapple-idle",
	"s_grapple-npc-death", "s_grapple-npc-death-aim", "s_grapple-stand", "s_grapple-strafe-backward",
	"s_grapple-strafe-forward", "s_grapple-struggle", "s_grapple-death",
	"s_mantle", "s_climb-up", "s_climb-down", "s_fall", "s_falling", "s_land",
};
const char* TraversalStateName(uint64_t state) {
	static const std::unordered_map<uint64_t, const char*> map = [] {
		std::unordered_map<uint64_t, const char*> m;
		for (const char* n : kTraversalStates) m[Sid(n)] = n;
		return m;
	}();
	if (!state) return nullptr;
	const auto it = map.find(state);
	return it == map.end() ? nullptr : it->second;
}
bool g_puppetIsBuddy = false;  // марионетка - настоящий спутник главы (у героя)
bool g_borrowedBuddy = false;  // спутник изображает партнёра лишь потому, что копию сделать не из чего
uint64_t g_buddyMode = 0;      // его режим скрипта до нас - вернуть при отпускании
SRWLOCK g_namesLock = SRWLOCK_INIT;
std::string g_heroName, g_companionName;

// Партнёра изображает настоящий спутник главы только по сети, когда партнёр явно выбрал companion
// (и мы сами не companion). В одиночном тесте F10 всегда спавним отдельную копию, не отключая Элли.
bool PartnerIsBuddy() { return UsingNetwork() && PartnerCharacter() == 1 && MyCharacter() != 1; }

bool g_hasBuddyFaction = false;
uint64_t g_lastBuddyFaction = 0;

void ApplyAllyFaction(uint64_t npc) {
	if (const uint64_t buddy = CurrentBuddy(npc)) {
		g_lastBuddyFaction = Call(g_dc.getFaction, { buddy });
		g_hasBuddyFaction = true;
	}
	const uint64_t faction = g_hasBuddyFaction ? g_lastBuddyFaction : Call(g_dc.getFaction, { Sid("player") });
	Call(g_dc.setFaction, { npc, faction });
}

// Без собственной воли: тот же набор, что у марионетки в TLOU2-Coop.
void AiOff(uint64_t npc) {
	Call(g_dc.scriptMode, { npc, 2, 1 });  // выключить ИИ и сюжетные команды
	for (NativeFn on : { g_dc.setBlind, g_dc.setDeaf }) Call(on, { npc, 1 });
	// Враги по замыслу игры не замечают спутников (стелс): чтобы они охотились и на партнёра, его
	// копия - во фракции спутника главы (союзника игрок не может схватить и задушить).
	// Если при перезагрузке спутник ещё не заспавнился, берём последнюю известную фракцию спутника.
	Call(g_dc.setIgnore, { npc, g_enemiesSee ? 0ull : 1ull });
	ApplyAllyFaction(npc);
	for (NativeFn off : { g_dc.hitReactions, g_dc.stun, g_dc.dodges }) Call(off, { npc, 0 });
	Call(g_dc.setUnshootable, { npc, 1 });  // напарника не подстрелить
	if (g_dc.npcForcePush) Call(g_dc.npcForcePush, { npc, 0, F(0.0f) });
	if (g_dc.collisionBodies) Call(g_dc.collisionBodies, { Sid("player"), 0, npc, 0, 0 });
}

// Спутник главы становится партнёром: его ведёт сеть, а не ИИ.
void TakeOverBuddy(uint64_t npc) {
	g_buddyMode = Call(g_dc.getScriptMode, { npc });
	g_buddyFaction = Call(g_dc.getFaction, { npc });
	g_buddyWeapon = Call(g_dc.npcWeapon, { npc });
	Log("puppet: companion weapon %016llX", static_cast<unsigned long long>(g_buddyWeapon));
	AiOff(npc);
	g_puppetIsBuddy = true;
	Log("puppet: companion %016llX now follows the partner (its AI is off)", static_cast<unsigned long long>(npc));
}

void ReleaseBuddy(uint64_t npc) {
	if (Call(g_dc.entityExists, { npc })) {
		Call(g_dc.scriptMode, { npc, g_buddyMode, 1 });
		if (g_buddyFaction) Call(g_dc.setFaction, { npc, g_buddyFaction });
		for (NativeFn off : { g_dc.setIgnore, g_dc.setBlind, g_dc.setDeaf }) Call(off, { npc, 0 });
		for (NativeFn on : { g_dc.hitReactions, g_dc.stun, g_dc.dodges }) Call(on, { npc, 1 });
		if (g_dc.collisionBodies) Call(g_dc.collisionBodies, { Sid("player"), 0, npc, 0, 1 });
		// Своё оружие обратно, если мы его меняли на оружие партнёра.
		if (g_buddyWeapon && Call(g_dc.npcWeapon, { npc }) != g_buddyWeapon) {
			Call(g_dc.destroyWeapon, { npc });
			Call(g_dc.giveWeapon, { npc, g_buddyWeapon, 999, 1 });
		}
	}
	g_buddyWeapon = 0;
	g_puppetIsBuddy = false;
	g_borrowedBuddy = false;
	Log("puppet: companion %016llX has its AI back", static_cast<unsigned long long>(npc));
}

// Копия NPC без собственной воли.
uint64_t Spawn(const Locator& at) {
	Locator me{};
	// Облик - выбор партнёра: auto (и спутник без спутника) - герой главы, иначе его персонаж.
	std::string look = PlayerLocator(me) ? HeroLook(me.pos) : std::string();
	g_puppetChar = PartnerCharacter();
	// Партнёр сам сообщает свой облик - он точнее нашей догадки (у друга в прологе мы угадывали
	// Джоэла вместо Сары по ближайшей точке появления).
	if (std::string sent = Locked(g_partnerLook); !sent.empty() && g_puppetChar != 1) look = std::move(sent);
	if (g_puppetChar >= 2)
		if (std::string art = ArtOfCharacter(CharacterName(g_puppetChar)); !art.empty()) look = std::move(art);
	if (g_puppetChar == 1)  // спутник копией (свой спутник спрятан или его нет): облик спутника главы
		if (const uint64_t buddy = CurrentBuddy(0))
			if (std::string art = ArtOfNpc(buddy); !art.empty()) look = std::move(art);
	g_puppetLook = look;
	std::string name;
	if (g_badForChapter != LoadChapter(g_myChapter).task) {
		g_badSpawners.clear();
		g_badHeroes.clear();
		g_putAway.clear();
		g_badForChapter = LoadChapter(g_myChapter).task;
	}
	// Персонаж, который сейчас сам есть на уровне по сюжету (Роберт, Тесс, Марлин), не копируем:
	// один проход по всем спавнерам вместо O(N^2) и без лимита в 64 попытки.
	std::vector<uint64_t> buddies;
	std::vector<std::string> skipHeroes = g_badHeroes;
	for (uint64_t b = CurrentBuddy(0); b && buddies.size() < 4; b = CurrentBuddy(b)) {
		if (std::find(buddies.begin(), buddies.end(), b) != buddies.end()) break;
		buddies.push_back(b);
		if (const std::string h = HeroName(ArtOfNpc(b)); !h.empty() &&
			std::find(skipHeroes.begin(), skipHeroes.end(), h) == skipHeroes.end()) {
			skipHeroes.push_back(h);
		}
	}
	std::vector<BuddyCandidate> candidates = FindBuddyCandidates(HeroName(look), g_badSpawners, skipHeroes);
	// Других людей нет: копия героя из его же спавнера (у Джоэла на уровне есть свой спавнер).
	bool heroSpawner = false;
	if (candidates.empty()) {
		candidates = FindHeroCandidates(HeroName(look), g_badSpawners);
		heroSpawner = !candidates.empty();
		if (heroSpawner) Log("puppet: no other people here - trying %s's own spawner", HeroName(look).c_str());
	}
	if (candidates.empty()) {
		static uint64_t listedFor = 0;
		if (listedFor != g_badForChapter) {
			listedFor = g_badForChapter;
			std::string list;
			const std::vector<std::string> arts = SpawnerArts(40);
			for (const std::string& a : arts) list += (list.empty() ? "" : ", ") + a;
			Log("puppet: spawner looks here: %s", list.c_str());
		}
		Log("puppet: no buddy spawner in loaded levels");
		return 0;
	}
	const uint64_t frame = BoundFrame(at.pos, at.rot);
	if (!frame) return 0;
	for (size_t idx = 0; idx < candidates.size() && idx < 4; ++idx) {
		const BuddyCandidate& cand = candidates[idx];
		const uintptr_t sp = cand.spawner;
		g_lastSpawner = sp;
		std::string tryLook = look;
		// Скелет другого пола: облик не надеваем, иначе на приседе/стойках бывает Т-поза.
		if (!tryLook.empty() && !SameSex(cand.art, HeroName(tryLook))) {
			Log("puppet: no %s-like NPC here; the copy keeps its own look (%s) so it animates properly",
				HeroName(tryLook).c_str(), cand.art.c_str());
			tryLook.clear();
		}
		uint64_t sid = 0;
		uint8_t f1 = 0;
		if (!Read(sp + kSpSid, sid) || !Read(sp + kSpFlags1, f1)) {
			g_badSpawners.push_back(sp);
			continue;
		}
		Log("puppet: spawning from %s, look %s", cand.name.c_str(), tryLook.empty() ? "-" : tryLook.c_str());
		auto* flags1 = reinterpret_cast<uint8_t*>(sp + kSpFlags1);
		*flags1 = static_cast<uint8_t>(f1 | kSpMultiSpawn);
		uint64_t npc = !tryLook.empty() ? Call(g_dc.spawnWithLook, { sid, frame, Sid(tryLook.c_str()) }) : 0;
		if (!tryLook.empty()) Log("puppet: look %s %s", tryLook.c_str(), npc && npc != sid ? "applied" : "refused");
		if (!npc || npc == sid) npc = Call(g_dc.spawnWithLook, { sid, frame, 0 });
		*flags1 = f1;
		if (!npc || npc == sid) {
			Log("puppet: spawn from %s failed (npc=%016llX)", cand.name.c_str(), static_cast<unsigned long long>(npc));
			g_badSpawners.push_back(sp);
			continue;
		}
		// Игра отдала настоящего спутника главы вместо новой копии - не трогаем его, спавнер в чёрный список.
		// Спавнер героя мог вернуть самого игрока - его, конечно, не трогаем.
		const bool isPlayer = heroSpawner && ResolveEntity(npc) == PlayerEntity();
		if (isPlayer || std::find(buddies.begin(), buddies.end(), npc) != buddies.end() || npc == CurrentBuddy(0) ||
			(!heroSpawner && !cand.hero.empty() && npc == Sid(cand.hero.c_str()))) {
			Log("puppet: %s gave the chapter's real companion %016llX, not a copy - leaving it alone", cand.name.c_str(),
				static_cast<unsigned long long>(npc));
			g_badSpawners.push_back(sp);
			if (!cand.hero.empty()) g_badHeroes.push_back(cand.hero);
			continue;
		}
		// Спавнер вернул уже убранную копию (та же сущность) - она спрятана под уровнем.
		if (std::find(g_putAway.begin(), g_putAway.end(), npc) != g_putAway.end()) {
			Log("puppet: %s gave back the old copy %016llX - skipping this spawner", cand.name.c_str(),
				static_cast<unsigned long long>(npc));
			g_badSpawners.push_back(sp);
			continue;
		}
		// g_puppetLook остаётся запрошенным обликом: если облик не надет (копия в чужом скелете),
		// сравнение с обликом партнёра иначе пересоздавало бы копию каждый кадр.
		AiOff(npc);
		const uint64_t wantWeapon = UsingNetwork() ? g_partnerWeapon.load() : g_myWeapon.load();
		if (!wantWeapon && Call(g_dc.npcWeapon, { npc })) Call(g_dc.destroyWeapon, { npc });
		Log("puppet: %s -> %016llX", cand.name.c_str(), static_cast<unsigned long long>(npc));
		return npc;
	}
	return 0;
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
				static_cast<unsigned long long>(LoadChapter(g_partnerChapter).task),
				static_cast<unsigned long long>(LoadChapter(g_myChapter).task));
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
	anim::SetTargets(PlayerEntity(), 0);  // мгновенный Detach до скрытия/освобождения NPC
	const uint64_t puppet = g_puppet;
	if (puppet && g_puppetIsBuddy) {  // настоящего спутника не прячем, а отдаём обратно ИИ
		ReleaseBuddy(puppet);
		g_puppet = 0;
		return;
	}
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
	if (const uint64_t frame = BoundFrame(at.pos, at.rot)) PlaceNpc(puppet, frame);
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
		anim::SetTargets(PlayerEntity(), 0);
		const bool instant = now - g_spawnedAt < 2000;
		Log("puppet %016llX is gone %s", static_cast<unsigned long long>(puppet),
			instant ? "right after spawning: the level removes it, trying another spawner" : "(death or checkpoint reload)");
		if (instant && g_lastSpawner) {
			g_badSpawners.push_back(g_lastSpawner);
			++g_failures;
		}
		g_puppet = puppet = 0;
		g_hidden = false;
		g_puppetIsBuddy = false;
		g_borrowedBuddy = false;
	}
	Locator target{}, me{};
	if (!PlayerLocator(me) || !DelayedTarget(now, target) || PartnerTooFar(target)) return;
	(void)me;
	if (!puppet) {
		if (now - g_lastSpawnTry < (g_failures >= 3 ? 15000u : 3000u)) return;
		// Облик партнёра приходит с его позами: ждём его до 5 с, иначе создали бы копию по догадке
		// и тут же пересоздали (лишняя копия рядом).
		static ULONGLONG waitingSince = 0;
		if (UsingNetwork() && PartnerCharacter() != 1 && Locked(g_partnerLook).empty()) {
			if (!waitingSince) waitingSince = now;
			if (now - waitingSince < 5000) return;
		}
		waitingSince = 0;
		// Партнёра-спутника изображает настоящий спутник главы, если он есть и не спрятан у нас
		// (мы сами спутник - наш спрятан, тогда партнёр - копия в облике спутника).
		if (PartnerIsBuddy())
			if (const uint64_t buddy = CurrentBuddy(0)) {
				TakeOverBuddy(buddy);
				g_puppet = puppet = buddy;
				g_lastSpawner = 0;
				g_lastSpawnTry = g_spawnedAt = now;
				return;
			}
		g_puppet = puppet = Spawn(target);
		const ULONGLONG took = GetTickCount64() - now;
		if (took > 20) Log("puppet: spawn attempt took %llu ms", took);
		g_lastSpawnTry = g_spawnedAt = GetTickCount64();
		if (!puppet) ++g_failures;
		// На этом участке уровня нет людей для копии: партнёра временно изображает спутник главы
		// (без него партнёра просто не видно). Раз в 15 с снова пробуем сделать копию.
		if (!puppet && g_failures >= 2)
			if (const uint64_t buddy = CurrentBuddy(0)) {
				TakeOverBuddy(buddy);
				g_borrowedBuddy = true;
				g_puppet = puppet = buddy;
				g_lastSpawner = 0;
				Log("puppet: no NPC to copy here - the companion stands in for the partner (F10 gives it back)");
			}
		return;
	}
	// Спутник взят взаймы: как только копию можно сделать, вернуть его уровню.
	if (puppet && g_borrowedBuddy && now - g_lastSpawnTry >= 15000) {
		g_lastSpawnTry = now;
		Locator at{};
		if (NpcLocator(puppet, at))
			if (const uint64_t copy = Spawn(at)) {
				const uint64_t buddy = puppet;
				anim::SetTargets(PlayerEntity(), 0);
				ReleaseBuddy(buddy);
				g_puppet = puppet = copy;
				g_spawnedAt = now;
				Log("puppet: made a copy %016llX, companion %016llX is back with the level",
					static_cast<unsigned long long>(copy), static_cast<unsigned long long>(buddy));
				return;
			}
	}
	if (puppet && checkNow && !g_puppetIsBuddy && PartnerIsBuddy()) {
		if (const uint64_t buddy = CurrentBuddy(puppet)) {
			const uint64_t old = puppet;
			Locator at{};
			Hide();
			if (NpcLocator(old, at)) {
				at.pos[1] -= 1000.0f;
				if (const uint64_t frame = BoundFrame(at.pos, at.rot)) PlaceNpc(old, frame);
			}
			TakeOverBuddy(buddy);
			g_puppet = puppet = buddy;
			g_lastSpawner = 0;
			g_lastSpawnTry = g_spawnedAt = now;
			Log("puppet: switched from temp copy %016llX to real companion %016llX",
				static_cast<unsigned long long>(old), static_cast<unsigned long long>(buddy));
			return;
		}
	}
	if (g_hidden) Show(puppet, target);
	if (now - g_spawnedAt > 2000) g_failures = 0;
	// Локальная петля анимаций для одиночного теста (F10): отдаём кадр с той же задержкой 1 с.
	if (!UsingNetwork()) {
		std::vector<uint8_t> replay;
		AcquireSRWLockExclusive(&g_localAnimLock);
		while (!g_localAnim.empty() && now - g_localAnim.front().t >= kLocalDelayMs) {
			replay = std::move(g_localAnim.front().data);
			g_localAnim.pop_front();
		}
		ReleaseSRWLockExclusive(&g_localAnimLock);
		if (!replay.empty()) anim::PushRx(replay.data(), replay.size(), now);
	}
	static ULONGLONG lastCmdAt = 0, lastSnapAt = 0;
	static float lastGoal[3]{};
	static int lastMode = -1;
	static bool wasFollowing = false;
	static uint64_t placedPuppet = 0;
	if (puppet != placedPuppet) placedPuppet = 0;
	const bool animRx = anim::HasActiveRx(now);
	if (!g_follow) {
		if (wasFollowing && (!g_dc.setObjectLocator || !animRx)) {
			Locator here{};
			if (NpcLocator(puppet, here))
				if (const uint64_t frame = BoundFrame(here.pos, here.rot)) Call(g_dc.moveTo, { puppet, frame, 0, 0, 0, 0, 0, 0 });
		}
		wasFollowing = false;
		lastMode = -1;
		return;
	}
	wasFollowing = true;
	// Точное покадровое позиционирование через set-object-locator: только когда реально
	// приходят анимации костей (animRx). Без анимаций при подъёме/уклоне set-object-locator
	// отрывал копию от пола и она "летела" статуей в воздухе — пусть идёт через npc-move-to / snap.
	static bool wasExact = false;
	const bool exact = g_dc.setObjectLocator && animRx;
	if (exact != wasExact) {
		wasExact = exact;
		lastMode = -1;
		Log("puppet: %s", exact ? "follows partner's animation exactly" : "back to walking on its own");
		Locator here{};
		if (exact && NpcLocator(puppet, here))  // остановить свой путь ИИ, чтобы не тянул в сторону
			if (const uint64_t frame = BoundFrame(here.pos, here.rot)) Call(g_dc.moveTo, { puppet, frame, 0, 0, 0, 0, 0, 0 });
	}
	if (exact) {
		Locator cur{};
		const bool haveCur = NpcLocator(puppet, cur);
		const float err = haveCur ? Dist(cur.pos, target.pos) : 999.0f;
		if (haveCur) g_lastErrM.store(err, std::memory_order_relaxed);
		if (const uint64_t frame = BoundFrame(target.pos, target.rot)) {
			if (!placedPuppet || err > 15.0f) {
				PlaceNpc(puppet, frame);
				placedPuppet = puppet;
			} else {
				Call(g_dc.setObjectLocator, { puppet, frame });
			}
		}
		return;
	}
	// Запасной путь через npc-move-to (только если set-object-locator недоступен).
	// Не чаще 10 раз в секунду, даже если команда не понадобилась (ранше пауза отсчитывалась
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
	g_lastErrM.store(err, std::memory_order_relaxed);
	const float rawSpeed = Dist(before.pos, target.pos) / 0.25f;
	const float targetSpeed = rawSpeed > 8.0f ? 0.0f : rawSpeed;  // скачок/телепорт партнёра не считаем бегом (не упреждаем в стену)
	// Отстала - догоняет бегом/спринтом (режим растёт с отрывом, см. ниже). Телепорт - только
	// если отрыв больше 15 м или она застряла: 2.5 с отстаёт больше чем на 4 м и не приближается
	// (прыжки, лестницы, обрывы - туда npc-move-to её не ведёт).
	// Застряла: 2.5 с сама почти не сдвинулась (меньше 0.5 м), хотя отстаёт больше чем на 4 м.
	// Сокращение отрыва не годится: когда игрок бежит быстрее, отрыв растёт и у бегущей.
	static float movedFrom[3]{};
	static ULONGLONG movedAt = 0;
	if (Dist(cur.pos, movedFrom) > 0.5f || err < 4.0f || !movedAt) {
		memcpy(movedFrom, cur.pos, sizeof(movedFrom));
		movedAt = now;
	}
	const bool stuck = err > 4.0f && now - movedAt > 2500;
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
	// Телепорт на глазах выглядит резко: пока марионетка в кадре и отстала меньше 30 м, даём ей до
	// 3 с догнать бегом (команда ниже), переносим - когда игрок отвернулся или время вышло.
	// Вне кадра переносим уже при отрыве 4 м: этого никто не видит, а бегом она отстаёт.
	static ULONGLONG snapWantedSince = 0;
	const CameraView view = CurrentView();
	const bool onScreen = Seen(view, cur.pos, 40.0f) || Seen(view, target.pos, 40.0f);
	const bool wantSnap = err > 15.0f || stuck || (UsingNetwork() && (err > 10.0f || (err > 4.5f && !onScreen)));
	bool snapNow = wantSnap;
	if (snapNow && err < 30.0f && !UsingNetwork()) {  // по сети - сразу: рассинхрон хуже рывка
		if (!snapWantedSince) snapWantedSince = now;
		if (now - snapWantedSince < 3000 && onScreen) snapNow = false;
	}
	if (!wantSnap) snapWantedSince = 0;
	if (snapNow) {
		snapWantedSince = 0;
		movedAt = 0;
		if (now - lastSnapAt < (uselessSnaps >= 2 ? 10000u : 1000u)) return;
		errAtSnap = err;
		judged = false;
		lastSnapAt = lastCmdAt = now;
		lastMode = -1;
		if (const uint64_t frame = BoundFrame(target.pos, target.rot)) PlaceNpc(puppet, frame);
		Log("puppet: snapped (%.1f m behind%s)", err, stuck ? ", stuck" : "");
		return;
	}
	// Партнёр стоит: через 0.7 с ставим марионетку точно на его место и с его поворотом (переносом
	// на месте - он же сбрасывает застывшую "широкую стойку" начала шага), дальше держим совпадение
	// не чаще раза в 2 с. Микрошаги командами давали рывки, а поворот стоя не менялся вовсе.
	static ULONGLONG standingSince = 0, lastSettle = 0;
	if (targetSpeed < 0.3f) {
		if (!standingSince) standingSince = now;
	} else {
		standingSince = 0;
	}
	if (UsingNetwork() && standingSince && now - standingSince >= 700 && err < 3.0f) {  // без сети партнёр - мы сами: встала бы в нас
		const float yawOff = std::fabs(YawDiff(cur.rot, target.rot));
		if ((err > 0.8f || yawOff > 0.45f) && now - lastSettle >= 2000) {
			lastSettle = lastCmdAt = now;
			lastMode = -1;
			if (const uint64_t frame = BoundFrame(target.pos, target.rot)) PlaceNpc(puppet, frame);
		}
		// Стоит без команд - освежаем "стой здесь" и стойку (0 = ambient, 3 = crouch), чтобы не застывала в шаге.
		static ULONGLONG lastIdleRefresh = 0;
		if (now - standingSince >= 800 && now - lastIdleRefresh >= 3500 && now - lastSettle >= 800) {
			lastIdleRefresh = lastCmdAt = now;
			const bool crouch = UsingNetwork() ? g_partnerCrouched.load() : g_meCrouched.load();
			if (const uint64_t frame = BoundFrame(cur.pos, target.rot)) Call(g_dc.moveTo, { puppet, frame, 0, 0, 0, 0, 0, 0 });
			Call(g_dc.setDemeanor, { puppet, crouch ? 3ull : 0ull, 0ull });  // номер: 0 ambient, 1 uneasy, 2 aggressive, 3 crouch, 4 search, 5 crawl (npc-demeanor-matrix); название читалось как мусор -> боевая стойка
		}
		return;
	}
	if (targetSpeed < 0.5f && err < 0.8f) return;
	// Мягкий догон по ровной земле (не тянем по диагонали в воздухе через ямы/ступени).
	if (UsingNetwork() && g_dc.setObjectLocator && targetSpeed > 0.5f && err > 4.5f && err < 15.0f &&
		std::fabs(target.pos[1] - cur.pos[1]) < 0.8f) {
		const float k = 0.5f * (err - 4.5f) / err;
		float pulled[3];
		for (int i = 0; i < 3; ++i) pulled[i] = cur.pos[i] + (target.pos[i] - cur.pos[i]) * k;
		// Высоту тянем вместе с горизонталью: если держать старую, на ступеньках и уклонах копия уходила
		// в пол. ("Полёт" раньше давала настоящая Тесс, которую мод ошибочно водил, - это исправлено.)
		if (const uint64_t frame = BoundFrame(pulled, cur.rot)) Call(g_dc.setObjectLocator, { puppet, frame });
	}
	// Скорость партнёра плюс догон: гистерезис не даёт переключать ходьба/бег/спринт туда-сюда каждые 0.2 с.
	const float speed = targetSpeed + (err > 1.0f ? (err - 1.0f) * 1.2f : 0.0f);
	int mode = lastMode;
	if (mode < 0) mode = speed < 1.5f ? 0 : (speed < 3.5f ? 1 : 2);
	else if (mode == 0 && speed > 1.7f) mode = speed > 3.7f ? 2 : 1;
	else if (mode == 1) {
		if (speed < 1.3f) mode = 0;
		else if (speed > 3.7f) mode = 2;
	} else if (mode == 2 && speed < 3.2f) {
		mode = speed < 1.3f ? 0 : 1;
	}
	// Цель с упреждением не дальше 1.0 м (при скачках скорости не улетает в стены).
	float goal[3]{ target.pos[0], target.pos[1], target.pos[2] };
	const float lead = targetSpeed > 0.5f ? 0.35f : 0.0f;
	if (lead > 0.0f) {
		const float dx = (target.pos[0] - before.pos[0]) * 4.0f * lead;
		const float dz = (target.pos[2] - before.pos[2]) * 4.0f * lead;
		const float d = std::sqrt(dx * dx + dz * dz);
		const float s = d > 1.0f ? (1.0f / d) : 1.0f;
		goal[0] += dx * s;
		goal[2] += dz * s;
	}
	// Партнёр остановился: прежняя цель была впереди - поправить на точное место сразу.
	const float resend = lead > 0.0f ? 1.2f : 0.75f;
	if (mode == lastMode && Dist(goal, lastGoal) < resend && now - lastCmdAt < 1500) return;
	if (const uint64_t frame = BoundFrame(goal, target.rot))
		Call(g_dc.moveTo, { puppet, frame, static_cast<uint64_t>(mode), 0, 0, 0, 0, 0 });
	memcpy(lastGoal, goal, sizeof(lastGoal));
	if (mode != lastMode) Log("puppet: move mode %d (target %.1f m/s, %.1f m away)", mode, targetSpeed, err);
	lastMode = mode;
	lastCmdAt = now;
}

// F7: перенести игрока к напарнику (к марионетке), на метр в сторону. Событие "teleport-player"
// с bound-frame в коробке, как в TLOU2-Coop; тип 18 = bound-frame, проверено в Part I.
constexpr uint64_t kBoxedBoundFrame = 18;

void TeleportToPartner() {
	// В катсцене (своей или партнёра) позиция партнёра - не место на карте: переносило под карту.
	if (g_inCutscene || g_partnerInCutscene) {
		Log("F7: not now - a cutscene is playing");
		return;
	}
	// По сети - к настоящей позиции партнёра (копия могла застрять в другом месте), без сети - к копии.
	Locator partner{};
	const uint64_t puppet = g_puppet;
	const bool real = UsingNetwork() && DelayedTarget(GetTickCount64(), partner);
	if (!real && (!puppet || !NpcLocator(puppet, partner))) {
		Log("F7: no partner to teleport to");
		return;
	}
	partner.pos[0] += 1.0f;
	const uint64_t frame = BoundFrame(partner.pos, partner.rot);
	const uint64_t boxed = frame ? Call(g_dc.newBoxedValue, { kBoxedBoundFrame, frame }) : 0;
	if (boxed) Call(g_dc.sendEvent, { Sid("teleport-player"), Sid("player"), boxed });
	Log("F7: teleport to partner at (%.1f %.1f %.1f) %s", partner.pos[0], partner.pos[1], partner.pos[2], boxed ? "sent" : "FAILED");
}

// ---- напарник: сам выглядит как спутник главы, его ИИ-спутник спрятан ------------------------
uint64_t g_hiddenBuddy = 0;
std::string g_heroLook, g_lookNow;  // облик героя (вернуть) и текущий подменённый облик

void ShowBuddy(uint64_t npc) {
	if (!npc || !Call(g_dc.entityExists, { npc })) return;
	Call(g_dc.sendEvent, { Sid("physicalize"), npc });
	Call(g_dc.setVisible, { npc, 1 });
	for (NativeFn off : { g_dc.setIgnore, g_dc.setBlind, g_dc.setDeaf }) Call(off, { npc, 0 });
	Log("companion: AI companion %016llX visible again", static_cast<unsigned long long>(npc));
}

void HideBuddy(uint64_t npc) {
	Call(g_dc.setVisible, { npc, 0 });
	if (const uint64_t none = Call(g_dc.newBoxedValue, { kBoxedSymbol, 0 })) Call(g_dc.sendEvent, { Sid("unphysicalize"), npc, none });
	for (NativeFn on : { g_dc.setIgnore, g_dc.setBlind, g_dc.setDeaf }) Call(on, { npc, 1 });
	Log("companion: AI companion %016llX hidden (you are the companion)", static_cast<unsigned long long>(npc));
}

void SwapLook(const std::string& art) {
	if (art.empty() || art == g_lookNow) return;
	Call(g_dc.swapLook, { Sid(art.c_str()), 1, 1 });
	g_lookNow = art;
	Log("companion: player look -> %s", art.c_str());
}

// Раз в полсекунды. on: сеть или тест включены; иначе всё вернуть как было.
// Свой облик: спутник - его облик и его ИИ-копия спрятана; персонаж - его костюм; auto - свой.
void CompanionTick(bool on) {
	static ULONGLONG last = 0;
	static uint64_t lastTask = 0;
	const ULONGLONG now = GetTickCount64();
	if (now - last < 500) return;
	last = now;
	const int mine = MyCharacter();
	if (const uint64_t task = LoadChapter(g_myChapter).task; task != lastTask) {
		lastTask = task;  // после загрузки облик игрока снова родной - подменим заново
		g_lookNow.clear();
		g_heroLook.clear();
	}
	if (on && mine >= 2) {
		if (g_hiddenBuddy) ShowBuddy(g_hiddenBuddy);
		g_hiddenBuddy = 0;
		Locator me{};
		if (g_heroLook.empty() && PlayerLocator(me)) g_heroLook = HeroLook(me.pos);
		static int triedFor = -1;
		static uint64_t triedTask = 0;
		if (g_lookNow.empty() || triedFor != mine || triedTask != lastTask) {  // перебор спавнеров - раз на главу или при смене персонажа
			triedFor = mine;
			triedTask = lastTask;
			const std::string art = ArtOfCharacter(CharacterName(mine));
			if (!art.empty()) SwapLook(art);
			else if (!g_lookNow.empty() && !g_heroLook.empty() && g_lookNow != g_heroLook) SwapLook(g_heroLook);
		}
		return;
	}
	on = on && mine == 1;
	if (!on) {
		if (g_hiddenBuddy) ShowBuddy(g_hiddenBuddy);
		g_hiddenBuddy = 0;
		if (!g_lookNow.empty() && !g_heroLook.empty() && g_lookNow != g_heroLook) SwapLook(g_heroLook);
		g_lookNow.clear();
		return;
	}
	const uint64_t buddy = CurrentBuddy(g_puppet);
	if (buddy == g_hiddenBuddy) return;
	if (g_hiddenBuddy) ShowBuddy(g_hiddenBuddy);
	g_hiddenBuddy = buddy;
	if (!buddy) return;
	Locator me{};
	if (g_heroLook.empty() && PlayerLocator(me)) g_heroLook = HeroLook(me.pos);
	HideBuddy(buddy);
	SwapLook(ArtOfNpc(buddy));
}

// Имена героя и спутника главы для лобби: раз в 2 с, спавнеры перебираем только при смене спутника.
void NamesTick() {
	static ULONGLONG last = 0;
	static uint64_t lastBuddy = ~0ull;
	const ULONGLONG now = GetTickCount64();
	if (now - last < 2000) return;
	last = now;
	const uint64_t buddy = g_hiddenBuddy ? g_hiddenBuddy : CurrentBuddy(g_puppetIsBuddy ? 0 : g_puppet.load());
	if (buddy == lastBuddy) return;
	lastBuddy = buddy;
	Locator me{};
	const std::string hero = PlayerLocator(me) ? HeroName(HeroLook(me.pos)) : std::string();
	const std::string companion = buddy ? HeroName(ArtOfNpc(buddy)) : std::string();
	AcquireSRWLockExclusive(&g_namesLock);
	g_heroName = hero.empty() ? g_heroName : hero;
	g_companionName = companion;
	ReleaseSRWLockExclusive(&g_namesLock);
	Log("roles: hero %s, companion %s", hero.empty() ? "?" : hero.c_str(), companion.empty() ? "none" : companion.c_str());
}

void ReadChapter() {
	static ULONGLONG last = 0;
	const ULONGLONG now = GetTickCount64();
	if (now - last < 33) return;
	last = now;
	const Chapter c = CurrentChapter();
	if (c == LoadChapter(g_myChapter)) return;
	StoreChapter(g_myChapter, c);
	Log("story: chapter %016llX task %016llX", static_cast<unsigned long long>(c.node), static_cast<unsigned long long>(c.task));
}

// ---- F5: к партнёру в его главу ------------------------------------------------------------
// task-play-debug! грузит главу с чекпоинта задания. Потом ждём, пока появятся игрок и
// марионетка (партнёр шлёт позы и во время нашей загрузки), и переносим игрока к ней.
constexpr ULONGLONG kJoinTimeoutMs = 90000, kJoinSettleMs = 2500;
std::atomic<int> g_joinRequest{ 0 };  // 0 нет, 1 к партнёру, 2 своя глава
std::atomic<ULONGLONG> g_joinAt{ 0 };  // идёт загрузка с F5 (проверяется вне блокировки)
ULONGLONG g_joinReadyAt = 0;
Chapter g_joinTarget;
bool g_joinOwn = false;

void StartJoin(bool own) {
	const Chapter target = LoadChapter(own ? g_myChapter : g_partnerChapter);
	if (!target) {
		Log(own ? "F5: my chapter is unknown yet" : "F5: partner's chapter is unknown (not connected?)");
		return;
	}
	Log("F5: loading %s chapter %016llX task %016llX", own ? "my own" : "partner's",
		static_cast<unsigned long long>(target.node), static_cast<unsigned long long>(target.task));
	PlayChapter(target);
	g_joinTarget = target;
	g_joinOwn = own;
	g_joinAt = GetTickCount64();
	g_joinReadyAt = 0;
}

// После загрузки: игрок есть и нужная глава открылась - переносим к партнёру по сетевой
// позиции (не ждём 90 с спавна марионетки, если партнёр далеко или ещё догружается).
void FinishJoin() {
	if (!g_joinAt) return;
	const ULONGLONG now = GetTickCount64();
	if (now - g_joinAt > kJoinTimeoutMs) {
		Log("F5: gave up waiting for the load to finish");
		g_joinAt = 0;
		return;
	}
	Locator me{}, partner{};
	const bool ready = now - g_joinAt > kJoinSettleMs && PlayerLocator(me) && LoadChapter(g_myChapter) == g_joinTarget;
	if (!ready) {
		g_joinReadyAt = 0;
		return;
	}
	if (!g_joinReadyAt) g_joinReadyAt = now;
	if (now - g_joinReadyAt < 1500) return;  // ещё пару кадров после загрузки
	if (UsingNetwork() && !g_joinOwn && !DelayedTarget(now, partner) && now - g_joinReadyAt < 4000) return;
	g_joinAt = 0;
	if (UsingNetwork() && (!g_joinOwn || DelayedTarget(now, partner))) TeleportToPartner();
	else Log("F5: chapter loaded");
}

}  // namespace

void Record(uint64_t nowMs, const Locator& l, uint32_t senderMs) {
	AcquireSRWLockExclusive(&g_trailLock);
	ULONGLONG t = nowMs;
	// Время отправителя сглаживает сетевой джиттер: привязываем к минимальной задержке
	// (если пакет пришёл быстрее базы -> подтягиваем базу; если сильно отстал -> сбрасываем).
	if (senderMs) {
		if (!g_lastArrivalMs || nowMs - g_lastArrivalMs > 500 || senderMs < g_lastSenderMs || senderMs - g_lastSenderMs > 500) {
			g_senderBase = senderMs;
			g_localBase = nowMs;
		}
		g_lastSenderMs = senderMs;
		g_lastArrivalMs = nowMs;
		t = g_localBase + (senderMs - g_senderBase);
		if (t > nowMs || t + 180 < nowMs) {
			g_senderBase = senderMs;
			g_localBase = nowMs;
			t = nowMs;
		}
	}
	const Sample& last = g_trail[(g_trailHead + kTrail - 1) % kTrail];
	if (t >= last.t && (t - last.t >= 12 || !last.t)) g_trail[g_trailHead++ % kTrail] = { t, l };
	ReleaseSRWLockExclusive(&g_trailLock);
}

void RecordLocalAnim(uint64_t nowMs, std::vector<uint8_t> frame) {
	if (frame.empty()) return;
	AcquireSRWLockExclusive(&g_localAnimLock);
	while (g_localAnim.size() >= 128) g_localAnim.pop_front();
	g_localAnim.push_back({ nowMs, std::move(frame) });
	ReleaseSRWLockExclusive(&g_localAnimLock);
}

void UseNetwork(bool on) { ClearTrail(on ? kNetDelayMs : kLocalDelayMs); }
bool UsingNetwork() { return g_trailDelay.load() == kNetDelayMs; }
void SetWanted(bool on) { g_wanted = on; }
bool Wanted() { return g_wanted; }
void SetFollow(bool on) { g_follow = on; }
bool Follow() { return g_follow; }
void RequestTeleport() { g_teleport = true; }
void SetNetActive(bool on) { g_netActive = on; }
void SetPartnerChapter(const Chapter& c) { StoreChapter(g_partnerChapter, c); }
Chapter MyChapter() { return LoadChapter(g_myChapter); }
void RequestJoin(bool own) { g_joinRequest = own ? 2 : 1; }

std::atomic<bool> g_myDeath{ false };
std::atomic<ULONGLONG> g_partnerDiedAt{ 0 };
bool TakeMyDeath() { return g_myDeath.exchange(false); }
void PartnerDied() { g_partnerDiedAt = GetTickCount64(); }
const char* const kCharacters[kCharacterCount] = { "auto", "companion", "joel", "ellie", "sarah", "tess", "tommy",
	"bill", "henry", "sam", "riley", "marlene", "maria" };
const char* CharacterName(int c) { return c >= 0 && c < kCharacterCount ? kCharacters[c] : kCharacters[0]; }
int CharacterFromName(const std::string& name) {
	for (int i = 0; i < kCharacterCount; ++i)
		if (name == kCharacters[i]) return i;
	return 0;
}
uint64_t CharacterSid(int c) { return Sid(CharacterName(c)); }
int CharacterFromSid(uint64_t sid) {
	for (int i = 0; i < kCharacterCount; ++i)
		if (Sid(kCharacters[i]) == sid) return i;
	return 0;
}
void SetMyCharacter(int c) {
	if (c != g_myChar.exchange(c)) Log("character: I play as %s", CharacterName(c));
}
int MyCharacter() { return g_myChar; }
void SetPartnerCharacter(int c) {
	if (c != g_partnerChar.exchange(c)) Log("character: partner plays as %s", CharacterName(c));
}
int PartnerCharacter() { return g_partnerChar; }
std::string MyLook() { return Locked(g_myLook); }
void SetPartnerLook(const std::string& art) {
	AcquireSRWLockExclusive(&g_lookLock);
	const bool changed = art != g_partnerLook;
	g_partnerLook = art;
	ReleaseSRWLockExclusive(&g_lookLock);
	if (changed && !art.empty()) Log("character: partner looks like %s", art.c_str());
}
Role GetRole() { return MyCharacter() == 1 ? Role::Companion : Role::Hero; }
bool MeFiring() { return g_meFiring; }
bool MeCrouched() { return g_meCrouched; }
bool MeAiming() { return g_meAiming; }
float MeAimPitch() { return g_meAimPitch; }
bool MeInCutscene() { return g_inCutscene; }
bool MeFlashlight() { return g_meFlashlight; }
void SetEnemiesSeePartner(bool on) { g_enemiesSee = on; }
void SetPartnerOutline(bool on) { g_outline = on; }
void SetAnimSync(bool on) { g_animSync = on; }
void SetClimbAnims(bool on) { g_climbAnims = on; }
bool ClimbAnims() { return g_climbAnims; }
bool MeTraversing() { return g_meTraversing; }
void SetPartnerTraversing(bool on) {
	if (on) g_partnerTraversalAt = GetTickCount64();
}
int MyDifficulty() { return g_myDifficulty; }
void FollowDifficulty(int difficulty) { g_wantDifficulty = difficulty; }
void SetPartnerStance(bool crouched, bool aiming) {
	g_partnerCrouched = crouched;
	g_partnerAiming = aiming;
}
void SetPartnerAimPitch(float pitch) { g_partnerAimPitch = pitch; }
void SetPartnerFlashlight(bool on) { g_partnerFlashlight = on; }
void SetPartnerInCutscene(bool on) { g_partnerInCutscene = on; }
std::vector<Throw> TakeMyThrows() {
	std::vector<Throw> out;
	AcquireSRWLockExclusive(&g_throwLock);
	out.swap(g_myThrows);
	ReleaseSRWLockExclusive(&g_throwLock);
	return out;
}
void PartnerThrew(const Throw& t) {
	AcquireSRWLockExclusive(&g_throwLock);
	if (g_partnerThrows.size() < 8) g_partnerThrows.push_back(t);
	ReleaseSRWLockExclusive(&g_throwLock);
}
bool MeHealing() { return g_meHealing; }
void SetPartnerHealing(bool on) { g_partnerHealing = on; }
void SetPartnerFiring(bool on) { g_partnerFiring = on; }
uint64_t MyWeapon() { return g_myWeapon; }
void SetPartnerWeapon(uint64_t weapon) { g_partnerWeapon = weapon; }
std::string HeroCharacter() {
	AcquireSRWLockShared(&g_namesLock);
	std::string s = g_heroName;
	ReleaseSRWLockShared(&g_namesLock);
	return s;
}
std::string CompanionCharacter() {
	AcquireSRWLockShared(&g_namesLock);
	std::string s = g_companionName;
	ReleaseSRWLockShared(&g_namesLock);
	return s;
}
uint64_t Current() { return g_puppet; }
float LastErrorMeters() { return g_puppet ? g_lastErrM.load(std::memory_order_relaxed) : 0.0f; }

// Свой номер кадра: хук интерпретатора срабатывает на каждом вызове скриптовой функции (тысячи
// за кадр на всех потоках), и проверка там должна стоить одно сравнение. Раньше здесь читался
// счётчик кадров игры через проверяемое чтение памяти, и FPS заметно проседал.
std::atomic<uint64_t> g_frame{ 0 };
std::atomic<bool> g_hiddenBuddyAtomic{ false };  // спрятан спутник: тик нужен, чтобы вернуть его
std::atomic<uint64_t> g_workUs{ 0 };

void OnFrame() { g_frame.fetch_add(1, std::memory_order_relaxed); }
uint64_t TakeWorkMicroseconds() { return g_workUs.exchange(0); }

// Свой подъём/прыжок (раз в 0.1 с): состояние анимации игрока из списка или почти вертикальное
// движение (лестница, уступ, падение; лестницы в домах - пологие и сюда не попадают).
static void TraversalTick() {
	static ULONGLONG last = 0, prevAt = 0;
	static float prev[3]{};
	const ULONGLONG now = GetTickCount64();
	if (now - last < 100) return;
	last = now;
	Locator me{};
	if (!PlayerLocator(me)) {
		g_meTraversing = false;
		prevAt = 0;
		return;
	}
	const uint64_t state = g_dc.animState ? Call(g_dc.animState, { Sid("player") }) : 0;
	const char* stateName = TraversalStateName(state);
	bool vertical = false;
	if (prevAt && now - prevAt < 500) {
		const float dt = (now - prevAt) / 1000.0f;
		const float vy = (me.pos[1] - prev[1]) / dt;
		const float dx = me.pos[0] - prev[0], dz = me.pos[2] - prev[2];
		const float vh = std::sqrt(dx * dx + dz * dz) / dt;
		vertical = std::fabs(vy) > 0.6f && std::fabs(vy) > 1.5f * vh;
	}
	memcpy(prev, me.pos, sizeof(prev));
	prevAt = now;
	// Рукопашку и схватки в список не берём (без anim-хука копия лишь замирала статуей).
	if (stateName || vertical) g_meTraversalAt = now;
	const ULONGLONG at = g_meTraversalAt;
	const bool on = at && now - at < 600;
	if (on != g_meTraversing.exchange(on)) {
		if (on) Log("me: climbing/jumping (%s)", stateName ? stateName : "vertical move");
		else Log("me: back on the ground");
	}
	// Для настройки списка: первые 80 разных состояний анимации в журнал.
	static std::unordered_set<uint64_t> seen;
	if (state && seen.size() < 80 && seen.insert(state).second)
		Log("me: anim state %016llX%s%s", static_cast<unsigned long long>(state), stateName ? " " : "", stateName ? stateName : "");
}

// Скрипты выполняются на нескольких потоках; работа - один раз за кадр, первым пришедшим.
void ScriptTick() {
	if (!g_wanted.load(std::memory_order_relaxed) && !g_teleport.load(std::memory_order_relaxed) &&
		!g_joinRequest.load(std::memory_order_relaxed) && !g_joinAt && !g_partnerDiedAt && g_ownDifficulty < 0 && !g_hiddenBuddyAtomic.load(std::memory_order_relaxed) &&
		g_myChar.load(std::memory_order_relaxed) == 0 &&
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
	{
		static ULONGLONG lastCutCheck = 0;
		const ULONGLONG tNow = GetTickCount64();
		if (tNow - lastCutCheck >= 33) {
			lastCutCheck = tNow;
			const bool ic = InCutscene();
			if (ic != g_inCutscene.exchange(ic))
				Log("story: cutscene %s", ic ? "started" : "ended");
		}
	}
	ProfileAdd("[chapter]", MicrosecondsSince(s));
	if (g_teleport.exchange(false)) TeleportToPartner();
	if (const int join = g_joinRequest.exchange(0)) StartJoin(join == 2);
	if (g_netActive || g_wanted) TraversalTick();
	// Своя смерть: здоровье упало до 0 (раз за жизнь; снова живы - снова следим).
	static ULONGLONG lastHealth = 0;
	static bool deadNow = false;
	if (g_netActive && GetTickCount64() - lastHealth >= 200) {
		lastHealth = GetTickCount64();
		Locator me{};
		if (PlayerLocator(me)) {
			const int health = static_cast<int>(Call(g_dc.playerHealth, {}));
			static bool seenAlive = false, logged = false;
			if (!logged && health > 0) {
				logged = true;
				Log("death: player health reads %d", health);  // что get-player-health отдаёт живому
			}
			// Смерть - только после того, как видели живого: иначе неверное чтение (всегда 0) слало бы
			// "погиб" без конца и перезагружало партнёра.
			if (health > 0) seenAlive = true;
			if (health <= 0 && seenAlive && !deadNow) {
				deadNow = true;
				g_myDeath = true;
				Log("death: I died - telling the partner");
			} else if (health > 0) {
				deadNow = false;
			}
		}
	}
	// Партнёр погиб: через 2 с перезагружаем свой чекпоинт (не во время своей загрузки и не мёртвыми -
	// своя смерть игра перезагрузит сама).
	if (const ULONGLONG at = g_partnerDiedAt; at && GetTickCount64() - at >= 2000) {
		g_partnerDiedAt = 0;
		if (!g_joinAt && !deadNow) {
			Log("death: partner died - reloading the checkpoint to restart together");
			StartJoin(true);
		}
	}
	FinishJoin();
	// Роль сменилась на напарника, а партнёра изображал наш спутник: отдать его ИИ (дальше он спрятан).
	// Партнёр больше не спутник, а его изображал наш спутник - отдать того ИИ.
	// И если мы сами стали спутником: наш спутник теперь спрятан, партнёру нужна копия.
	if (!PartnerIsBuddy() && g_puppetIsBuddy && !g_borrowedBuddy) Hide();
	// Партнёр выбрал другого персонажа - копию NPC пересоздать в новом облике.
	const bool lookChanged = g_puppetChar >= 0 && g_puppetChar != 1 && !Locked(g_partnerLook).empty() &&
		g_puppetLook != Locked(g_partnerLook) && PartnerCharacter() < 2;
	if (g_puppet && !g_puppetIsBuddy && g_puppetChar >= 0 && (g_puppetChar != PartnerCharacter() || lookChanged)) {
		// Старую копию не удалить (kill посреди кадра ронял игру), а спрятанная иногда снова
		// показывалась рядом с новой: прячем и уносим на 1 км под уровень.
		const uint64_t old = g_puppet;
		Locator at{};
		Hide();
		if (NpcLocator(old, at)) {
			at.pos[1] -= 1000.0f;
			if (const uint64_t frame = BoundFrame(at.pos, at.rot)) PlaceNpc(old, frame);
		}
		g_putAway.push_back(old);
		Log("puppet: old copy %016llX put away, making a new one", static_cast<unsigned long long>(old));
		g_puppet = 0;
		g_hidden = false;
		g_lastSpawnTry = 0;
	}
	if (!g_wanted && g_puppet) Hide();
	static bool cutscenePausedBuddy = false;
	static bool cutsceneHidClone = false;
	// Копию на катсцену не прячем и спутника не отпускаем: определение катсцены срабатывало ложно
	// (40 с, пока игрок жал F7), копия пропадала и стояла - жёсткий рассинхрон. Как до v31.
	const bool inCutsceneNow = false;
	if (inCutsceneNow) {
		if (g_puppet && g_puppetIsBuddy && !cutscenePausedBuddy) {
			cutscenePausedBuddy = true;
			Call(g_dc.scriptMode, { g_puppet, g_buddyMode, 1 });
			for (NativeFn off : { g_dc.setIgnore, g_dc.setBlind, g_dc.setDeaf }) Call(off, { g_puppet, 0 });
			Log("puppet: yielded companion to cutscene");
		} else if (g_puppet && !g_puppetIsBuddy && !g_hidden && !cutsceneHidClone) {
			cutsceneHidClone = true;
			Hide();
			Log("puppet: hidden during cutscene");
		}
	} else {
		if (cutscenePausedBuddy && g_puppet && g_puppetIsBuddy) {
			AiOff(g_puppet);
			Log("puppet: reclaimed companion after cutscene");
		}
		cutscenePausedBuddy = false;
		if (cutsceneHidClone) cutsceneHidClone = false;
	}
	CompanionTick(g_wanted || g_netActive);
	g_hiddenBuddyAtomic = g_hiddenBuddy != 0;
	if (g_wanted || g_netActive) NamesTick();
	// Свой облик для партнёра: раз в 3 с (перебор спавнеров только при смене группы арта).
	static ULONGLONG lastLook = 0;
	if (g_netActive && GetTickCount64() - lastLook >= 3000) {
		lastLook = GetTickCount64();
		Locator me{};
		if (PlayerLocator(me)) {
			std::string look = MyCharacter() >= 2 ? ArtOfCharacter(CharacterName(MyCharacter())) : std::string();
			// Выбран персонаж, которого на этом уровне нет (его облик не загружен): сказать игроку.
			static std::string missingSaid;
			if (MyCharacter() >= 2 && look.empty()) {
				std::string name = CharacterName(MyCharacter());
				const std::string key = name + "@" + std::to_string(LoadChapter(g_myChapter).task);
				if (key != missingSaid) {
					missingSaid = key;
					if (!name.empty()) name[0] = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
					menu::Notify(menu::NoteCharacterMissing, name);
					Log("character: %s is not on this level - the partner sees the hero", name.c_str());
				}
			} else {
				missingSaid.clear();
			}
			if (look.empty()) {
				static uint64_t lastGroup = 0, lastLookTask = 0;
				static std::string lastArt;
				const uint64_t curLookTask = LoadChapter(g_myChapter).task;
				if (curLookTask && curLookTask != lastLookTask) {
					lastLookTask = curLookTask;
					lastArt.clear();
				}
				const uint64_t group = Call(g_dc.artGroup, { Sid("player") });
				if (group != lastGroup || lastArt.empty()) {
					lastGroup = group;
					lastArt = PlayerLook(me.pos);
				}
				look = lastArt;
			}
			AcquireSRWLockExclusive(&g_lookLock);
			g_myLook = look;
			ReleaseSRWLockExclusive(&g_lookLock);
		}
	}
	// Свой выстрел: только когда игрок есть в мире (в меню/загрузке нативы игрока не зовём).
	Locator selfLoc{};
	const bool haveSelf = PlayerLocator(selfLoc);
	{
		static uint64_t ammoOf = 0;
		static int64_t lastAmmo = -1;
		static ULONGLONG shotAt = 0;
		if (!haveSelf) {
			ammoOf = 0;
			lastAmmo = -1;
			shotAt = 0;
			g_meFiring = false;
		} else {
			const bool flag = (Call(g_dc.isFiring, {}) & 1) != 0;
			bool spent = false;
			const uint64_t weapon = g_myWeapon;
			if (g_dc.totalAmmo && weapon) {
				const int64_t ammo = static_cast<int32_t>(Call(g_dc.totalAmmo, { weapon }));
				spent = weapon == ammoOf && lastAmmo >= 0 && ammo < lastAmmo;
				ammoOf = weapon;
				lastAmmo = ammo;
			}
			// Бросок, а не выстрел: в руках бросаемый предмет и его стало меньше. Откуда - от головы
			// игрока, куда - точка в 15 м по взгляду камеры.
			if (spent && IsThrowable(weapon)) {
				spent = false;
				const CameraView cam = CurrentView();
				if (cam.ok) {
					Throw t{ weapon, { selfLoc.pos[0], selfLoc.pos[1] + 1.5f, selfLoc.pos[2] }, {} };
					for (int i = 0; i < 3; ++i) t.to[i] = cam.cam.pos[i] + cam.fwd[i] * 15.0f;
					AcquireSRWLockExclusive(&g_throwLock);
					if (g_myThrows.size() < 8) g_myThrows.push_back(t);
					if (!UsingNetwork() && g_partnerThrows.size() < 8) g_partnerThrows.push_back(t);  // F10: копия повторит сама
					ReleaseSRWLockExclusive(&g_throwLock);
					Log("me: threw %016llX", static_cast<unsigned long long>(weapon));
				}
			}
			// Флаг держим 120 мс: поза уходит раз в 50 мс, выстрел в один кадр иначе проскочит между пакетами.
			if (flag || spent) {
				shotAt = GetTickCount64();
				Log("me: shot (%s)", flag ? "flag" : "ammo");
			}
			g_meFiring = shotAt && GetTickCount64() - shotAt < 120;
			if (flag || spent) enemies::NoteOwnShot();
		}
	}
	// Поза: своя - для отправки (5 раз в секунду хватает), партнёра - на марионетку при смене.
	// Манеры NPC из скриптов игры: 'crouch - присед, 'ambient - обычная.
	// Копию бьют враги - держим её живой: здоровье обратно до того, с каким она появилась.
	static ULONGLONG lastHeal = 0;
	static uint64_t healOf = 0;
	static uint32_t fullHealth = 0;
	if (g_enemiesSee && g_puppet && GetTickCount64() - lastHeal >= 200) {
		lastHeal = GetTickCount64();
		const uint64_t puppet = g_puppet;
		const uint32_t h = static_cast<uint32_t>(Call(g_dc.getHealth, { puppet }));
		float now = 0;
		memcpy(&now, &h, 4);
		if (puppet != healOf) {
			healOf = puppet;
			fullHealth = h;
		} else {
			float full = 0;
			memcpy(&full, &fullHealth, 4);
			if (now < full && now > 0.0f) Call(g_dc.setHealth, { puppet, fullHealth });
		}
	}
	// Контур далёкого партнёра: игра держит подсветку один кадр, поэтому - каждый кадр, пока далеко.
	// Даль проверяем раз в 0.3 с (позиции читаются из памяти).
	static ULONGLONG lastFar = 0;
	static bool partnerFar = false;
	if (g_outline && g_puppet && !g_hidden && UsingNetwork()) {
		if (GetTickCount64() - lastFar >= 300) {
			lastFar = GetTickCount64();
			Locator me{}, them{};
			partnerFar = PlayerLocator(me) && NpcLocator(g_puppet, them) && Dist(me.pos, them.pos) > 12.0f;
		}
		if (partnerFar)
			if (const uint64_t color = Call(g_dc.allocColor, { F(0.55f), F(0.8f), F(1.0f), F(1.0f), 0 }))
				Call(g_dc.highlight, { g_puppet, 0, color });
	}
	// Сложность: читаем свою; гость переходит на сложность хоста, а без хоста возвращает свою.
	static ULONGLONG lastDifficulty = 0;
	if (GetTickCount64() - lastDifficulty >= 2000) {
		lastDifficulty = GetTickCount64();
		Locator me{};
		if (PlayerLocator(me)) {
			const int mine = static_cast<int>(Call(g_dc.getDifficulty, {}));
			g_myDifficulty = mine;
			const int want = g_wantDifficulty;
			if (want >= 0 && want < 16 && want != mine) {
				if (g_ownDifficulty < 0) g_ownDifficulty = mine;
				Call(g_dc.setDifficulty, { static_cast<uint64_t>(want) });
				Log("difficulty: %d -> host's %d", mine, want);
			} else if (want < 0 && g_ownDifficulty >= 0) {
				if (g_ownDifficulty != mine) Call(g_dc.setDifficulty, { static_cast<uint64_t>(g_ownDifficulty.load()) });
				Log("difficulty: back to my own %d", g_ownDifficulty.load());
				g_ownDifficulty = -1;
			}
		}
	}
	static ULONGLONG lastStance = 0;
	if (GetTickCount64() - lastStance >= 100) {  // присед/прицел - без заметной задержки
		lastStance = GetTickCount64();
		static uint64_t appliedTo = 0, flOf = 0, colOf = 0, aimOf = 0, aimBanned = 0;
		static bool crouchNow = false, flNow = false, colHidden = true, aimNow = false;
		static uint64_t weaponOf = 0, weaponNow = 0, refusedWeapon = 0;
		static ULONGLONG lastWeaponCheck = 0, refusedAt = 0;
		static bool hadSelfStance = false;
		static uint64_t lastStanceTask = 0;
		const uint64_t curTask = LoadChapter(g_myChapter).task;
		if (haveSelf && (!hadSelfStance || (curTask && curTask != lastStanceTask))) {
			// После загрузки/смены чекпоинта игра может пересоздать спутника с тем же sid (t1x-tess, t1x-ellie).
			appliedTo = flOf = colOf = aimOf = weaponOf = 0;
			colHidden = true;
		}
		hadSelfStance = haveSelf;
		if (curTask) lastStanceTask = curTask;
		if (haveSelf) {
			g_meCrouched = (Call(g_dc.isCrouched, {}) & 1) != 0;
			g_meAiming = (Call(g_dc.isAiming, {}) & 1) != 0;
			if (g_dc.flashlightOn) g_meFlashlight = (Call(g_dc.flashlightOn, {}) & 1) != 0;
			if (g_meAiming) {
				const CameraView cam = CurrentView();
				if (cam.ok) g_meAimPitch = std::asin(std::clamp(cam.fwd[1], -0.98f, 0.98f));
			}
			g_myWeapon = Call(g_dc.playerWeapon, {});
		} else {
			g_meCrouched = false;
			g_meAiming = false;
			g_meFlashlight = false;
			g_myWeapon = 0;
		}
		const uint64_t puppet = g_puppet;
		const bool crouch = UsingNetwork() ? g_partnerCrouched.load() : g_meCrouched.load();
		const bool fl = UsingNetwork() ? g_partnerFlashlight.load() : g_meFlashlight.load();
		// Если при спавне спутник главы ещё не успел появиться (после смерти/перезагрузки), обновляем фракцию копии,
		// как только он появился - чтобы копию солдата нельзя было схватить (grab).
		if (puppet && !g_hidden && !g_puppetIsBuddy) {
			if (const uint64_t buddy = CurrentBuddy(puppet)) {
				const uint64_t bf = Call(g_dc.getFaction, { buddy });
				g_lastBuddyFaction = bf;
				g_hasBuddyFaction = true;
				if (Call(g_dc.getFaction, { puppet }) != bf) {
					Call(g_dc.setFaction, { puppet, bf });
					Log("puppet: faction updated to companion's (%llu)", static_cast<unsigned long long>(bf));
				}
			}
		}
		// Чтобы марионетка не толкала игрока в узких проходах / при телепорте (F7).
		if (g_dc.ignoreCollision && puppet && (puppet != colOf || g_hidden != colHidden)) {
			Call(g_dc.ignoreCollision, { g_hidden ? 0 : puppet });
			colOf = puppet;
			colHidden = g_hidden;
		} else if (g_dc.ignoreCollision && !puppet && colOf) {
			Call(g_dc.ignoreCollision, { 0 });
			colOf = 0;
			colHidden = true;
		}
		if (!puppet || g_hidden) {
			appliedTo = 0;
			flOf = 0;
		}
		// Фонарик партнёра (в тёмных локациях).
		if (g_dc.setPropFlashlight && puppet && !g_hidden && (puppet != flOf || fl != flNow)) {
			Call(g_dc.setPropFlashlight, { puppet, fl ? 1ull : 0ull });
			flOf = puppet;
			flNow = fl;
		}
		// Когда приходят анимации партнёра (anim::HasActiveRx), манеры NPC ИИ не трогаем, иначе
		// они перебивают перенесённые слои. npc-set-demeanor ждёт число (0=ambient, 3=crouch) и флаг force.
		const bool aiDriven = !anim::HasActiveRx(GetTickCount64());  // позу ведёт не перенос анимаций
		if (puppet && !g_hidden && aiDriven && (puppet != appliedTo || crouch != crouchNow)) {
			Call(g_dc.setDemeanor, { puppet, crouch ? 3ull : 0ull, 0ull });  // номер: 0 ambient, 1 uneasy, 2 aggressive, 3 crouch, 4 search, 5 crawl (npc-demeanor-matrix); название читалось как мусор -> боевая стойка
			if (crouch != crouchNow) Log("puppet: %s", crouch ? "crouches" : "stands up");
			appliedTo = puppet;
			crouchNow = crouch;
		}
		// Оружие: у марионетки в руках то же, что у партнёра (без него npc-fire-weapon не стреляет).
		// Если движок сбросил оружие копии (например, после рукопашки), перевыдаём при проверке раз в 0.5 с.
		static ULONGLONG unarmedRefusedAt = 0;
		const uint64_t weapon = UsingNetwork() ? g_partnerWeapon.load() : g_myWeapon.load();
		if (puppet != weaponOf) {
			weaponNow = ~0ull;
			refusedWeapon = 0;
			lastWeaponCheck = 0;
			unarmedRefusedAt = 0;
		}
		const ULONGLONG wNow = GetTickCount64();
		if (puppet && !g_hidden && (weapon != weaponNow || wNow - lastWeaponCheck >= 500)) {
			lastWeaponCheck = wNow;
			const uint64_t had = Call(g_dc.npcWeapon, { puppet });
			if (weapon == 0) {
				if (had && (!unarmedRefusedAt || wNow - unarmedRefusedAt >= 2500)) {
					Call(g_dc.destroyWeapon, { puppet });
					const uint64_t nowHad = Call(g_dc.npcWeapon, { puppet });
					unarmedRefusedAt = nowHad ? wNow : 0;
					Log("puppet: weapon %016llX -> 0 (now %016llX)", static_cast<unsigned long long>(had),
						static_cast<unsigned long long>(nowHad));
					aimNow = false;
				}
			} else if (had != weapon && (weapon != refusedWeapon || wNow - refusedAt >= 2500)) {
				if (had) Call(g_dc.destroyWeapon, { puppet });
				Call(g_dc.giveWeapon, { puppet, weapon, 999, 1 });
				const uint64_t nowHad = Call(g_dc.npcWeapon, { puppet });
				if (nowHad != weapon) {
					refusedWeapon = weapon;
					refusedAt = wNow;
				} else {
					refusedWeapon = 0;
				}
				if (weapon != weaponNow || nowHad == weapon) {
					Log("puppet: weapon %016llX -> %016llX (now %016llX)", static_cast<unsigned long long>(had),
						static_cast<unsigned long long>(weapon), static_cast<unsigned long long>(nowHad));
				}
				aimNow = false;  // новое оружие - поднять снова, если партнёр целится
			}
			weaponNow = weapon;
		}
		weaponOf = puppet;
		// Прицел партнёра: оружие вверх/вниз и наклон ствола (npc-aim-at-point). На скелете без
		// анимаций прицеливания игра даёт Т-позу: это видит сторож в хуке анимаций (только
		// bindpose, ни одного клипа) - тогда оружие вниз и прицел у этой копии больше не включаем.
		// Без хука сторожа нет: прицел включаем как в v30 (Т-позу давал чужой скелет, его больше не берём).
		const bool aimWant = UsingNetwork() ? g_partnerAiming.load() : g_meAiming.load();
		if (puppet != aimOf) {
			aimOf = puppet;
			aimNow = true;  // новая копия: сразу "оружие вниз" (вооружённый NPC - цикада, солдат - держит его наготове)
		}
		if (puppet && aimNow && anim::PuppetTposeStreak() >= 3 && aimBanned != puppet) {
			aimBanned = puppet;
			Call(g_dc.weaponDown, { puppet });
			if (g_dc.clearAimAt) Call(g_dc.clearAimAt, { puppet });
			aimNow = false;
			Log("puppet: aiming gives this copy a T-pose - aiming off for it");
		}
		const bool aimOk = puppet && puppet != aimBanned && !g_hidden && aiDriven && weapon != 0;
		const bool aimTarget = aimOk && aimWant;
		if (puppet && !g_hidden && aimTarget != aimNow && (aimOk || aimNow)) {
			Call(aimTarget ? g_dc.weaponUp : g_dc.weaponDown, { puppet });
			if (!aimTarget && g_dc.clearAimAt) Call(g_dc.clearAimAt, { puppet });
			aimNow = aimTarget;
		}
		if (aimNow && g_dc.aimAtPoint) {
			Locator tgt{};
			if (DelayedTarget(GetTickCount64(), tgt)) {
				const float pitch = std::clamp(UsingNetwork() ? g_partnerAimPitch.load() : g_meAimPitch.load(), -1.2f, 1.2f);
				const float* q = tgt.rot;
				float hx = 2.0f * (q[0] * q[2] + q[3] * q[1]);
				float hz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
				const float hlen = std::sqrt(hx * hx + hz * hz);
				if (hlen > 1e-4f) { hx /= hlen; hz /= hlen; } else { hx = 0.0f; hz = 1.0f; }
				const float cp = std::cos(pitch), sp = std::sin(pitch);
				const uint64_t pt = Call(g_dc.allocVector, { F(tgt.pos[0] + hx * cp * 20.0f), F(tgt.pos[1] + 1.35f + sp * 20.0f),
					F(tgt.pos[2] + hz * cp * 20.0f), F(1.0f), 0 });
				if (pt) Call(g_dc.aimAtPoint, { puppet, pt, F(0.15f), 0 });
			}
		}
	}
	// Привязка реальных указателей игрока (TX) и марионетки (RX) для хука CreateAnimCmds.
	// При отсутствии/скрытии марионетки сбрасываем указатель мгновенно (без задержки), а клипы и
	// feather-маски резолвим прямо здесь, в безопасном игровом/скриптовом потоке.
	if (anim::HooksOk()) {
		static ULONGLONG lastAnimBind = 0;
		static uint64_t lastBoundPuppet = 0;
		static uintptr_t cachedPuppetPtr = 0;
		const ULONGLONG nowAnim = GetTickCount64();
		const uint64_t curPuppet = g_puppet.load();
		if (!curPuppet || g_hidden) {
			cachedPuppetPtr = 0;
			lastBoundPuppet = 0;
		} else if (curPuppet != lastBoundPuppet || !cachedPuppetPtr || nowAnim - lastAnimBind >= 33) {
			lastAnimBind = nowAnim;
			lastBoundPuppet = curPuppet;
			cachedPuppetPtr = ResolveEntity(curPuppet);
		}
		anim::SetTargets(PlayerEntity(), cachedPuppetPtr);
		const bool txWanted = (UsingNetwork() && g_netActive.load()) || (!UsingNetwork() && g_wanted.load());
		anim::SetTxEnabled(txWanted && (g_animSync.load(std::memory_order_relaxed) || g_meTraversing.load()));
		const bool rxWanted = !g_inCutscene.load(std::memory_order_relaxed);
		anim::SetRxEnabled(rxWanted);
		if (rxWanted && cachedPuppetPtr) anim::PrepareRx(nowAnim);
	} else {
		anim::SetTxEnabled(false);
		anim::SetRxEnabled(false);
	}
	// Партнёр стреляет (по сети; без сети - свой игрок, для проверки): стреляет и марионетка.
	// Только союзник: спутник главы или копия во фракции игрока (enemies_see_partner) - в игрока не попадает.
	static ULONGLONG lastShot = 0;
	const bool shoot = UsingNetwork() ? g_partnerFiring.load() : g_meFiring.load();
	if (shoot && (g_puppetIsBuddy || g_enemiesSee) && g_puppet && !g_hidden && GetTickCount64() - lastShot >= 180) {
		const uint64_t heldWeapon = Call(g_dc.npcWeapon, { g_puppet });
		if (heldWeapon) {
			lastShot = GetTickCount64();
			if (UsingNetwork()) enemies::NoteCopyShot();  // её пули - только для вида
			const uint64_t r = Call(g_dc.fireWeapon, { g_puppet });
			Log("puppet: fires (%llu, weapon %016llX)", static_cast<unsigned long long>(r),
				static_cast<unsigned long long>(heldWeapon));
		}
	}
	// Броски партнёра: тот же предмет летит из его места в его точку (настоящий: горит, взрывается).
	// Без сети (F10) - свой бросок через 1 с из места копии.
	{
		std::vector<Throw> throws;
		AcquireSRWLockExclusive(&g_throwLock);
		throws.swap(g_partnerThrows);
		ReleaseSRWLockExclusive(&g_throwLock);
		for (Throw t : throws) {
			if (!g_dc.throwProjectile || g_inCutscene) break;
			if (!UsingNetwork()) {
				Locator c{};
				if (!g_puppet || !NpcLocator(g_puppet, c)) continue;
				t.from[0] = c.pos[0], t.from[1] = c.pos[1] + 1.5f, t.from[2] = c.pos[2];
			}
			const uint64_t from = Call(g_dc.allocVector, { F(t.from[0]), F(t.from[1]), F(t.from[2]), F(1.0f), 0 });
			const uint64_t to = Call(g_dc.allocVector, { F(t.to[0]), F(t.to[1]), F(t.to[2]), F(1.0f), 0 });
			const uint64_t r = from && to ? Call(g_dc.throwProjectile, { from, to, t.item, F(-1.0f) }) : 0;
			Log("puppet: throws %016llX (%016llX)", static_cast<unsigned long long>(t.item), static_cast<unsigned long long>(r));
		}
	}
	// Лечение: своё - по прогрессу аптечки; партнёра - копия делает жест лечения при начале.
	{
		static ULONGLONG lastHealCheck = 0;
		// Только когда игрок есть: без него (главное меню) player-get-heal-progress роняет игру.
		Locator self{};
		if (g_dc.healProgress && GetTickCount64() - lastHealCheck >= 100 && PlayerLocator(self)) {
			lastHealCheck = GetTickCount64();
			const uint32_t bits = static_cast<uint32_t>(Call(g_dc.healProgress, {}));
			float progress = 0.0f;
			memcpy(&progress, &bits, 4);
			const bool healing = std::isfinite(progress) && progress > 0.0f && progress < 1.0f;
			if (healing != g_meHealing.exchange(healing) && healing) Log("me: healing");
		}
		static bool healShown = false;
		const bool heal = UsingNetwork() ? g_partnerHealing.load() : g_meHealing.load();
		if (heal && !healShown && g_puppet && !g_hidden && !g_inCutscene && g_dc.gesture && g_dc.gestureParams) {
			const uint64_t params = Call(g_dc.gestureParams, {});
			const uint64_t r = Call(g_dc.gesture, { g_puppet, Sid("npc-heal"), params });
			Log("puppet: heal gesture (%016llX)", static_cast<unsigned long long>(r));
		}
		healShown = heal;
	}
	s = QpcNow();
	if (g_wanted) Update();
	ProfileAdd("[update]", MicrosecondsSince(s));
	if (g_netActive) enemies::Tick(g_puppet);
	if (g_netActive) objects::Tick();
	QueryPerformanceCounter(&b);
	QueryPerformanceFrequency(&f);
	g_workUs.fetch_add(static_cast<uint64_t>((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart), std::memory_order_relaxed);
	busy.clear(std::memory_order_release);
}

}  // namespace puppet
