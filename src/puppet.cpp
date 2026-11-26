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
std::atomic<uint64_t> g_puppet{ 0 }, g_doneFrame{ 0 };
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
int g_failures = 0;
uint64_t g_badForChapter = 0;

// ---- роли ----------------------------------------------------------------------------------
std::atomic<int> g_myChar{ 0 }, g_partnerChar{ 0 };
int g_puppetChar = -1;  // для какого выбора партнёра создана копия NPC
SRWLOCK g_lookLock = SRWLOCK_INIT;
std::string g_myLook, g_partnerLook, g_puppetLook;  // свой облик, присланный партнёром, у копии

std::string Locked(const std::string& s) {
	AcquireSRWLockShared(&g_lookLock);
	std::string out = s;
	ReleaseSRWLockShared(&g_lookLock);
	return out;
}
std::atomic<bool> g_meFiring{ false }, g_partnerFiring{ false };
bool g_puppetIsBuddy = false;  // марионетка - настоящий спутник главы (у героя)
uint64_t g_buddyMode = 0;      // его режим скрипта до нас - вернуть при отпускании
SRWLOCK g_namesLock = SRWLOCK_INIT;
std::string g_heroName, g_companionName;

// Без собственной воли: тот же набор, что у марионетки в TLOU2-Coop.
void AiOff(uint64_t npc) {
	Call(g_dc.scriptMode, { npc, 2, 1 });  // выключить ИИ и сюжетные команды
	for (NativeFn on : { g_dc.setIgnore, g_dc.setBlind, g_dc.setDeaf }) Call(on, { npc, 1 });
	for (NativeFn off : { g_dc.hitReactions, g_dc.stun, g_dc.dodges }) Call(off, { npc, 0 });
	Call(g_dc.setUnshootable, { npc, 1 });  // напарника не подстрелить
}

// Спутник главы становится партнёром: его ведёт сеть, а не ИИ.
void TakeOverBuddy(uint64_t npc) {
	g_buddyMode = Call(g_dc.getScriptMode, { npc });
	AiOff(npc);
	g_puppetIsBuddy = true;
	Log("puppet: companion %016llX now follows the partner (its AI is off)", static_cast<unsigned long long>(npc));
}

void ReleaseBuddy(uint64_t npc) {
	if (Call(g_dc.entityExists, { npc })) {
		Call(g_dc.scriptMode, { npc, g_buddyMode, 1 });
		for (NativeFn off : { g_dc.setIgnore, g_dc.setBlind, g_dc.setDeaf }) Call(off, { npc, 0 });
		for (NativeFn on : { g_dc.hitReactions, g_dc.stun, g_dc.dodges }) Call(on, { npc, 1 });
	}
	g_puppetIsBuddy = false;
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
		g_badForChapter = LoadChapter(g_myChapter).task;
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
	std::vector<uintptr_t> skip = g_badSpawners;
	for (int tries = 0; tries < 8; ++tries) {
		sp = FindBuddySpawner(HeroName(look), skip, name);
		if (!sp || !inScene(sp)) break;
		Log("puppet: %s is in the scene already, skipping", name.c_str());
		skip.push_back(sp);
		sp = 0;
	}
	// Ничего не осталось (партнёр выбрал персонажа, который и так здесь, а других NPC нет - пролог):
	// берём любой NPC и надеваем выбранный облик. Двойник лучше невидимого партнёра.
	if (!sp && !look.empty()) {
		sp = FindBuddySpawner(std::string(), g_badSpawners, name);
		if (sp) Log("puppet: no other NPC to use; %s will wear the look", name.c_str());
	}
	g_lastSpawner = sp;
	// Спавнер того же пола выбирается первым (SpawnerRank); другой пол - только если своего нет
	// (пролог: кроме Сары женских NPC нет). Образ всё равно надеваем: лучше Сара с чуть кривым
	// бегом, чем Джоэл вместо Сары.
	if (!look.empty() && !SameSex(ReadName(ReadPtr(sp + kSpArt)), HeroName(look)))
		Log("puppet: no %s-like NPC here, putting the look on another skeleton", HeroName(look).c_str());
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
	AiOff(npc);
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
		g_puppetIsBuddy = false;
	}
	Locator target{};
	if (!DelayedTarget(now, target) || PartnerTooFar(target)) return;
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
		if (PartnerCharacter() == 1 && MyCharacter() != 1)
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
	// Партнёр почти стоит и мы рядом - стоим (микрошаги давали рывки и "широкую стойку").
	if (targetSpeed < 0.5f && err < 1.5f) return;
	const float speed = (std::max)(targetSpeed, err * 0.8f);
	const int mode = speed < 1.8f ? 0 : (speed < 4.2f ? 1 : 2);
	// Цель с упреждением: где партнёр будет через ~1 с при той же скорости. Команда - только если
	// цель ушла больше чем на 2 м, сменился режим или прошло 2.5 с: каждая новая команда
	// перезапускала шаг, и NPC отставал и дёргался (особенно при медленной ходьбе).
	float goal[3];
	const float lead = targetSpeed > 0.5f ? 1.0f : 0.0f;
	for (int i = 0; i < 3; ++i) goal[i] = target.pos[i] + (target.pos[i] - before.pos[i]) * 4.0f * lead;
	goal[1] = target.pos[1];  // по высоте не упреждаем
	// Партнёр остановился: прежняя цель была впереди - поправить на точное место сразу.
	const float resend = lead > 0.0f ? 2.0f : 0.75f;
	if (mode == lastMode && Dist(goal, lastGoal) < resend && now - lastCmdAt < 2500) return;
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
	}
	if (on && mine >= 2) {
		if (g_hiddenBuddy) ShowBuddy(g_hiddenBuddy);
		g_hiddenBuddy = 0;
		Locator me{};
		if (g_heroLook.empty() && PlayerLocator(me)) g_heroLook = HeroLook(me.pos);
		static int triedFor = -1;
		static uint64_t triedTask = 0;
		if (g_lookNow.empty() && (triedFor != mine || triedTask != lastTask)) {  // перебор спавнеров - раз на главу
			triedFor = mine;
			triedTask = lastTask;
			SwapLook(ArtOfCharacter(CharacterName(mine)));
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
	if (now - last < 1000) return;
	last = now;
	const Chapter c = CurrentChapter();
	if (c == LoadChapter(g_myChapter)) return;
	StoreChapter(g_myChapter, c);
	Log("story: chapter %016llX task %016llX", static_cast<unsigned long long>(c.node), static_cast<unsigned long long>(c.task));
}

// ---- F5: к партнёру в его главу ------------------------------------------------------------
// task-play-debug! грузит главу с чекпоинта задания. Потом ждём, пока появятся игрок и
// марионетка (партнёр шлёт позы и во время нашей загрузки), и переносим игрока к ней.
constexpr ULONGLONG kJoinTimeoutMs = 90000, kJoinSettleMs = 4000;
std::atomic<int> g_joinRequest{ 0 };  // 0 нет, 1 к партнёру, 2 своя глава
std::atomic<ULONGLONG> g_joinAt{ 0 };  // идёт загрузка с F5 (проверяется вне блокировки)
ULONGLONG g_joinReadyAt = 0;
Chapter g_joinTarget;

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
	g_joinAt = GetTickCount64();
	g_joinReadyAt = 0;
}

// После загрузки: игрок есть, глава та, марионетка рядом с партнёром - переносим (без сети -
// просто сообщаем, что загрузилось).
void FinishJoin() {
	if (!g_joinAt) return;
	const ULONGLONG now = GetTickCount64();
	if (now - g_joinAt > kJoinTimeoutMs) {
		Log("F5: gave up waiting for the load to finish");
		g_joinAt = 0;
		return;
	}
	Locator me{};
	const bool ready = now - g_joinAt > kJoinSettleMs && PlayerLocator(me) && LoadChapter(g_myChapter) == g_joinTarget &&
		(!UsingNetwork() || (g_puppet && !g_hidden));
	if (!ready) {
		g_joinReadyAt = 0;
		return;
	}
	if (!g_joinReadyAt) g_joinReadyAt = now;
	if (now - g_joinReadyAt < 1500) return;  // ещё пару кадров после загрузки
	g_joinAt = 0;
	if (UsingNetwork()) TeleportToPartner();
	else Log("F5: chapter loaded");
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
void SetPartnerChapter(const Chapter& c) { StoreChapter(g_partnerChapter, c); }
Chapter MyChapter() { return LoadChapter(g_myChapter); }
void RequestJoin(bool own) { g_joinRequest = own ? 2 : 1; }
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
void SetPartnerFiring(bool on) { g_partnerFiring = on; }
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

// Свой номер кадра: хук интерпретатора срабатывает на каждом вызове скриптовой функции (тысячи
// за кадр на всех потоках), и проверка там должна стоить одно сравнение. Раньше здесь читался
// счётчик кадров игры через проверяемое чтение памяти, и FPS заметно проседал.
std::atomic<uint64_t> g_frame{ 0 };
std::atomic<bool> g_hiddenBuddyAtomic{ false };  // спрятан спутник: тик нужен, чтобы вернуть его
std::atomic<uint64_t> g_workUs{ 0 };

void OnFrame() { g_frame.fetch_add(1, std::memory_order_relaxed); }
uint64_t TakeWorkMicroseconds() { return g_workUs.exchange(0); }

// Скрипты выполняются на нескольких потоках; работа - один раз за кадр, первым пришедшим.
void ScriptTick() {
	if (!g_wanted.load(std::memory_order_relaxed) && !g_teleport.load(std::memory_order_relaxed) &&
		!g_joinRequest.load(std::memory_order_relaxed) && !g_joinAt && !g_hiddenBuddyAtomic.load(std::memory_order_relaxed) &&
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
	ProfileAdd("[chapter]", MicrosecondsSince(s));
	if (g_teleport.exchange(false)) TeleportToPartner();
	if (const int join = g_joinRequest.exchange(0)) StartJoin(join == 2);
	FinishJoin();
	// Роль сменилась на напарника, а партнёра изображал наш спутник: отдать его ИИ (дальше он спрятан).
	// Партнёр больше не спутник, а его изображал наш спутник - отдать того ИИ.
	// И если мы сами стали спутником: наш спутник теперь спрятан, партнёру нужна копия.
	if ((PartnerCharacter() != 1 || MyCharacter() == 1) && g_puppetIsBuddy) Hide();
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
			if (const uint64_t frame = BoundFrame(at.pos, at.rot)) Call(g_dc.teleport, { old, frame });
		}
		Log("puppet: old copy %016llX put away, making a new one", static_cast<unsigned long long>(old));
		g_puppet = 0;
		g_hidden = false;
		g_lastSpawnTry = 0;
	}
	if (!g_wanted && g_puppet) Hide();
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
			if (look.empty()) {
				static uint64_t lastGroup = 0;
				static std::string lastArt;
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
	g_meFiring = (Call(g_dc.isFiring, {}) & 1) != 0;
	// Партнёр стреляет (по сети; без сети - свой игрок, для проверки): стреляет и марионетка.
	// Только настоящий спутник главы: он союзник и в игрока не попадает; копия NPC - нет.
	static ULONGLONG lastShot = 0;
	const bool shoot = UsingNetwork() ? g_partnerFiring.load() : g_meFiring.load();
	if (shoot && g_puppetIsBuddy && g_puppet && !g_hidden && GetTickCount64() - lastShot >= 180) {
		lastShot = GetTickCount64();
		Call(g_dc.fireWeapon, { g_puppet });
	}
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
