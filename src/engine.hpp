#pragma once
// Всё, что знает о внутренностях игры: адреса, чтение памяти, функции движка, игрок, спавнеры.
// Адреса - только для Steam build 18995818 (PE timestamp kBuildTimestamp).

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace engine {

constexpr uint32_t kBuildTimestamp = 0x685BEBE1;
constexpr uintptr_t kFrameTickRva = 0x11D45C7;  // конец кадра: ++frameCounter (основной поток)
// Обёртка, через которую интерпретатор скриптов вызывает любую функцию движка (0x208BBF8):
// 0x208BB1C открывает скриптовую область, потом call rax. Точка между ними - в скриптовом
// контексте (без него spawn падал) и вне чужих функций движка (вложенный вызов вешал игру).
// Найдено замером: через неё идут ~100 разных функций, через другие места - единицы.
constexpr uintptr_t kScriptInvokeRva = 0x208BC36;

extern uintptr_t g_base;

// ---- память --------------------------------------------------------------------------------
// Неверный адрес даёт false, а не вылет игры (проверка VirtualQuery, без исключений).
bool SafeCopy(void* dst, uintptr_t src, size_t n);
template <typename T>
bool Read(uintptr_t address, T& out) { return SafeCopy(&out, address, sizeof(T)); }
uintptr_t ReadPtr(uintptr_t a);
std::string ReadName(uintptr_t p);

constexpr uint64_t Sid(const char* s) {
	uint64_t h = 0xCBF29CE484222325;
	while (*s) h = (h ^ static_cast<uint8_t>(*s++)) * 0x100000001B3;
	return h;
}

uint32_t BuildTimestamp();

// ---- функции движка (скриптовые "натив"ы) ---------------------------------------------------
// Вызов: fn(ret*, argc, argv*, 0); аргументы - 8-байтовые слоты, float в младших 32 битах.
// Вызывать только в скриптовом контексте (см. kScriptInvokeRva).
using NativeFn = void (*)(uint64_t* ret, int argc, const uint64_t* argv, uint64_t unused);

struct Natives {
	NativeFn allocVector, allocQuat, allocBoundFrame, spawnWithLook, entityExists, teleport, scriptMode,
		setIgnore, setBlind, setDeaf, hitReactions, stun, dodges, objectLocator, moveTo, newBoxedValue, sendEvent,
		setVisible, setUnshootable, darrayCount, darrayAt, getHealth, setHealth, isDead, npcDie, taskNode, taskSubnode, taskPlay, getScriptMode, swapLook, isFiring, fireWeapon, cameraLocator, artGroup, playerHealth, isCrouched, isAiming, setDemeanor, weaponUp, weaponDown, getFaction, setFaction, highlight, allocColor, getDifficulty, setDifficulty, hudMessage,
		playerWeapon, npcWeapon, giveWeapon, destroyWeapon,
		totalAmmo,        // необязательная: может быть nullptr
		setObjectLocator, // необязательная: может быть nullptr
		aimAtPoint,       // необязательная: может быть nullptr
		clearAimAt,       // необязательная: может быть nullptr
		cinematicPlaying, // необязательная: может быть nullptr
		moviePlaying,     // необязательная: может быть nullptr
		flashlightOn,     // необязательная: может быть nullptr
		setPropFlashlight,// необязательная: может быть nullptr
		ignoreCollision,  // необязательная: может быть nullptr
		collisionBodies,  // необязательная: может быть nullptr
		npcForcePush,     // необязательная: может быть nullptr
		throwProjectile,  // необязательная: бросок предмета (throw-projectile)
		healProgress,     // необязательная: player-get-heal-progress
		gesture,          // необязательная: gesture_ (жест/анимация NPC)
		gestureParams,    // необязательная: alloc-gesture-play-params
		animState;        // необязательная: get-object-anim-state (состояние анимации, sid)
};
extern Natives g_dc;
bool InitNatives();  // false, если какой-то не найден

uint64_t F(float v);
uint64_t Call(NativeFn fn, std::initializer_list<uint64_t> args);
// Вызовы и время по функциям движка с прошлого раза ("npc-move-to 20x 3.1ms, ...").
std::string TakeProfile();
// Свои замеры в том же отчёте: name - строковый литерал (сравнивается по адресу).
void ProfileAdd(const char* name, uint64_t us);
int64_t QpcNow();
uint64_t MicrosecondsSince(int64_t qpcStart);

// ---- объекты -------------------------------------------------------------------------------
struct Locator { float pos[4], rot[4]; };

uintptr_t PlayerEntity();                  // указатель NdGameObject игрока (0 если нет)
uintptr_t ResolveEntity(uint64_t sid);     // указатель NdGameObject/NPC по StringId64 (0 если нет)
bool PlayerLocator(Locator& l);            // без скриптового контекста, только чтение
bool CameraLocator(Locator& l);            // камера игрока (вперёд - +Z), скриптовый контекст
// Видно ли точку на экране: ближе maxDist и в конусе ~70° от взгляда камеры (с запасом на
// широкий FOV). Камеры нет - считаем, что видно: лучше не перенести, чем перенести на глазах.
struct CameraView { bool ok; Locator cam; float fwd[3]; };
CameraView CurrentView();                  // скриптовый контекст
bool Seen(const CameraView& v, const float p[3], float maxDist = 25.0f);
bool NpcLocator(uint64_t npc, Locator& out);
uint64_t BoundFrame(const float pos[3], const float rot[4]);
// Место в сюжете: узел графа заданий (глава) и задание в нём (чекпоинт). Скриптовый контекст.
struct Chapter {
	uint64_t node = 0, task = 0;
	bool operator==(const Chapter&) const = default;
	explicit operator bool() const { return node && task; }
};
Chapter CurrentChapter();
// Идёт ли сейчас катсцена / видеовставка (скриптовый контекст).
bool InCutscene();
// Загрузить это место сюжета (task-play-debug!, как "join" в TLOU2-Coop). Скриптовый контекст.
void PlayChapter(const Chapter& c);

// ---- спавнеры ------------------------------------------------------------------------------
constexpr uintptr_t kSpSid = 0x38, kSpName = 0x50, kSpType = 0x70, kSpArt = 0x78, kSpFlags0 = 0x80, kSpFlags1 = 0x81;
constexpr uint8_t kSpDisabled = 0x08, kSpMultiSpawn = 0x10;

// Заголовок спавнера одним чтением (раскладка как в Part II).
struct SpawnerHead {
	float pos[4], rot[4];
	uint8_t pad0[0x18];
	uint64_t sid;        // +0x38
	uint8_t pad1[0x10];
	uintptr_t name;      // +0x50
	uint8_t pad2[0x18];
	uint64_t type;       // +0x70, 0 = точка появления игрока
	uintptr_t art;       // +0x78
	uint8_t flags0, flags1;
};
static_assert(offsetof(SpawnerHead, sid) == kSpSid && offsetof(SpawnerHead, name) == kSpName &&
	offsetof(SpawnerHead, type) == kSpType && offsetof(SpawnerHead, art) == kSpArt &&
	offsetof(SpawnerHead, flags0) == kSpFlags0 && offsetof(SpawnerHead, flags1) == kSpFlags1);

// Человек-NPC для партнёра, под пол и имя героя (hero - "joel", пусто = любой); exclude -
// спавнеры, которые уже не сработали.
uintptr_t FindBuddySpawner(const std::string& hero, const std::vector<uintptr_t>& exclude, std::string& nameOut);
// Имена (sid) NPC всех спавнеров этого сюжетного персонажа ("marlene") на загруженных уровнях.
std::vector<uint64_t> CharacterSids(const std::string& character);
// Переносные предметы (доски, лестницы): имена (sid) объектов спавнеров с "plank"/"ladder" в имени
// или арте; names - их имена для журнала. Перебор всех спавнеров: звать редко.
std::vector<uint64_t> CarryableSids(std::vector<std::string>& names);
// Спутник главы (Тесс, Элли, Билл...): первый живой NPC из списка спутников игры
// ('all-buddy-npcs), кроме exclude. 0 - спутника нет. Скриптовый контекст.
uint64_t CurrentBuddy(uint64_t exclude);
// Костюм персонажа ("tess" -> "t1x-tess-costume-01") по спавнерам загруженных уровней; пусто - нет.
std::string ArtOfCharacter(const std::string& character);
// Арт спавнера, из которого создан NPC с этим именем ("t1x-tess-costume-01"); пусто - не нашли.
std::string ArtOfNpc(uint64_t npc);
// Костюм героя главы: арт ближайшей к точке точки появления игрока ("t1x-joel-costume-01").
std::string HeroLook(const float around[3]);
// Облик самого игрока ("t1x-sarah-costume-01"): get-art-group игрока, сверенный с артами
// спавнеров; не нашлось - HeroLook (ближайшая точка появления игрока). Скриптовый контекст.
std::string PlayerLook(const float around[3]);
std::string HeroName(const std::string& art);  // "t1x-joel-costume-01" -> "joel"
bool IsFemaleHero(const std::string& name);
// Пригодность спавнера с артом art для партнёра героя hero: 0 - не годится, больше - лучше.
int SpawnerRank(const std::string& art, const std::string& hero);
// Образ героя hero подходит скелету спавнера с артом art (мужской образ на женском скелете
// бегал криво).
bool SameSex(const std::string& art, const std::string& hero);

}  // namespace engine
