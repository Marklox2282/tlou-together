#pragma once
// Связь двух игроков: Steam (друзья, через Valve, без портов) или UDP по IP (локальная сеть, тесты).
// Без игровых зависимостей, поэтому проверяется отдельной программой test/netsim.cpp.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace net {

constexpr uint32_t kMagic = 0x4F433154;  // "T1CO"
constexpr uint8_t kProtocol = 3;          // 1 = v7 (без рукопожатия), 2 = v8 (без врагов)
enum : uint8_t { kHello = 1, kPose = 2, kBye = 3, kEnemy = 4, kEnemyPos = 6, kDeath = 7, kSettings = 8 };  // 5 - бывший обмен ролями
constexpr uint16_t kFlagDead = 1;  // kEnemy: враг убит
constexpr uint16_t kFlagHero = 2;  // kPose: отправитель играет за героя главы
constexpr uint16_t kFlagFiring = 4;  // kPose: отправитель стреляет
constexpr uint16_t kFlagCrouch = 8, kFlagAim = 16;  // kPose: присел, целится

#pragma pack(push, 1)
struct Packet {
	uint32_t magic = kMagic;
	uint8_t protocol = kProtocol;
	uint8_t type = 0;
	uint16_t flags = 0;
	uint32_t seq = 0;
	uint32_t modVersion = 0;  // приветствие: версия мода отправителя
	uint64_t chapterNode = 0, chapterTask = 0;  // место отправителя в сюжете (0 = неизвестно)
	float pos[3]{};
	float rot[4]{};
	char name[24]{};          // приветствие: ник отправителя
	uint64_t target = 0;      // kEnemy: имя врага
	float health = 0;         // kEnemy: его здоровье после удара
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 96);

struct Config {
	std::string peerIp;           // пусто = Steam; иначе UDP напрямую
	uint16_t port = 7777;         // свой UDP-порт
	uint16_t peerPort = 7777;
};

enum class State { Off, Searching, Connected };

// Друг из Steam в этой игре (для лобби).
struct Friend {
	enum Status { InGame, Ready, OtherVersion } status = InGame;  // Ready: тоже ищет, та же версия мода
	std::string name;
};

// Не потокобезопасно: все вызовы из одного потока (у мода - игровой кадр).
class Link {
public:
	using LogFn = void (*)(const char* fmt, ...);
	Link(LogFn log, uint32_t modVersion) : log_(log), modVersion_(modVersion) {}
	~Link() { Stop(); }

	bool Start(const Config& cfg, uint64_t nowMs);
	void Stop();
	// Отправляет приветствия при поиске и принимает пакеты. Возвращает true, если пришла поза.
	bool Tick(uint64_t nowMs, Packet& pose);
	// character: выбранный персонаж отправителя (sid имени), в поле target.
	// look: облик отправителя ("t1x-sarah-costume-01"), в поле name.
	void SendPose(const float pos[3], const float rot[4], bool hero = true, bool firing = false, uint64_t character = 0,
		const std::string& look = {}, uint16_t stance = 0);  // stance: kFlagCrouch | kFlagAim
	// Ведущая сторона (позиции врагов): хозяин лобби; без лобби - больший Steam ID (по UDP - меньший порт).
	bool Authority() const;
	void SendEnemy(uint64_t npc, float health, bool dead);  // через Steam - с гарантией доставки
	std::vector<Packet> TakeEnemyEvents();                  // полученные kEnemy и kEnemyPos с прошлого раза
	void SendEnemyPos(uint64_t npc, const float pos[3], const float rot[4]);  // хозяин: где его враг
	void SendDeath();          // свой игрок погиб (с гарантией доставки)
	void SendSettings(uint32_t difficulty);  // ведущая сторона: её сложность
	int PeerDifficulty() const { return peerDifficulty_; }  // -1 - не знаем
	bool TakePartnerDeath();   // партнёр погиб с прошлого раза
	// Друзья в этой игре (только Steam; включает Steam при первом вызове). Готовность - по
	// приветствиям за последние 3 с, поэтому видна, пока ищем.
	std::vector<Friend> Friends(uint64_t nowMs);

	// ---- лобби Steam: создать, пригласить (оверлей Steam), войти по приглашению, выйти ----
	// Участники лобби соединяются между собой обычным образом; гость шлёт приветствия хозяину.
	void CreateLobby(uint64_t nowMs);
	void InviteToLobby();        // окно приглашения Steam (как Shift+Tab)
	// Код лобби: номер лобби Steam + секрет хозяина ("0D7C-RFW-K3P9"). Лобби публичное (иначе по коду
	// не войти) и находится поиском, поэтому хозяин подключает только знающих секрет - или своих
	// друзей в Steam (приглашение через Steam приходит без кода).
	struct LobbyKey { uint64_t lobby = 0; uint32_t secret = 0; };
	void JoinLobby(LobbyKey key, uint64_t nowMs);
	void LeaveLobby();
	uint64_t Lobby() const { return lobby_; }
	bool LobbyBusy() const { return lobbyCall_ != 0; }  // создаём или входим
	bool LobbyHost() const { return lobbyHost_; }
	// Последняя неудача с лобби: 0 нет, 1 не создалось, 2 не вошли; сбрасывается новой попыткой.
	int LobbyFailure() const { return lobbyFailure_; }
	std::string LobbyOwnerName() const;
	// Приглашение, принятое в Steam (событие Steam или "+connect_lobby" в командной строке).
	static uint64_t TakeInvite();
	// Код лобби для входа без списка друзей: "K7QX-92F" (32-битный номер лобби в base32 Крокфорда).
	static std::string LobbyCode(const LobbyKey& key);
	static LobbyKey LobbyFromCode(const std::string& code);  // lobby 0 - не код
	std::string Code() const { return lobbyHost_ && lobby_ ? LobbyCode({ lobby_, secret_ }) : std::string(); }
	// Пускать ли в своё лобби: секрет из приветствия совпал или это друг в Steam.
	static bool Admit(uint32_t mySecret, uint64_t helloSecret, bool isFriend) { return isFriend || helloSecret == mySecret; }
	using Chapter = std::pair<uint64_t, uint64_t>;  // узел и задание (engine::Chapter)
	void SetChapter(Chapter c) { chapter_ = c; }
	void SetName(const std::string& name) { name_ = name; }
	void SetBeep(bool on) { beep_ = on; }  // звук Windows при соединении (в игре - да, в тестах - нет)

	State GetState() const { return state_; }
	const std::string& PeerName() const { return peerName_; }
	Chapter PeerChapter() const { return peerChapter_; }
	uint64_t LastHeardMs() const { return lastHeard_; }

private:
	Packet Make(uint8_t type) const;
	void Send(const Packet& p, bool reliable = false);
	void SendTo(uint64_t steamId, const Packet& p, bool reliable = false);
	bool Receive(Packet& p, uint64_t& fromSteam);
	void OnHello(const Packet& p, uint64_t from, uint64_t nowMs);
	void Disconnected(const char* why);

	LogFn log_;
	uint32_t modVersion_;
	std::string name_;
	Chapter chapter_{};
	State state_ = State::Off;
	bool steam_ = false;
	uintptr_t sock_ = ~uintptr_t(0);
	uint32_t peerAddr_ = 0;
	uint16_t peerPort_ = 0;
	uint64_t peerSteam_ = 0;
	std::string peerName_;
	Chapter peerChapter_{};
	uint64_t lastHello_ = 0, lastHeard_ = 0, lastMismatchLog_ = 0;
	uint32_t seq_ = 0, lastSeq_ = 0;
	bool oldProtocolLogged_ = false;
	std::vector<Packet> enemyEvents_;
	struct Heard { uint64_t steamId; uint32_t modVersion; uint64_t atMs; };
	std::vector<Heard> heard_;  // последние приветствия от друзей
	void PollLobby(uint64_t nowMs);
	uint64_t lobby_ = 0, lobbyCall_ = 0, target_ = 0;  // target_: хозяин лобби (гость шлёт только ему)
	int lobbyCallKind_ = 0;                              // 1 создание, 2 вход
	bool lobbyHost_ = false;
	int lobbyFailure_ = 0;
	uint32_t secret_ = 0, joinSecret_ = 0;  // свой секрет (хозяин) и из кода (гость)
	uint64_t lastRejectLog_ = 0;
	bool leaveAfterTick_ = false;
	bool partnerDied_ = false;
	int peerDifficulty_ = -1;
	uint16_t port_ = 0;
	bool beep_ = true;
};

}  // namespace net
