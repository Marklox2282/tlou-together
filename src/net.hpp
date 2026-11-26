#pragma once
// Связь двух игроков: Steam (друзья, через Valve, без портов) или UDP по IP (локальная сеть, тесты).
// Без игровых зависимостей, поэтому проверяется отдельной программой test/netsim.cpp.

#include <cstdint>
#include <string>

namespace net {

constexpr uint32_t kMagic = 0x4F433154;  // "T1CO"
constexpr uint8_t kProtocol = 2;          // 1 = v7 (без рукопожатия)
enum : uint8_t { kHello = 1, kPose = 2, kBye = 3 };

#pragma pack(push, 1)
struct Packet {
	uint32_t magic = kMagic;
	uint8_t protocol = kProtocol;
	uint8_t type = 0;
	uint16_t flags = 0;
	uint32_t seq = 0;
	uint32_t modVersion = 0;  // приветствие: версия мода отправителя
	uint64_t chapter = 0;     // текущее сюжетное задание отправителя (0 = неизвестно)
	float pos[3]{};
	float rot[4]{};
	char name[24]{};          // приветствие: ник отправителя
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 76);

struct Config {
	std::string peerIp;           // пусто = Steam; иначе UDP напрямую
	uint16_t port = 7777;         // свой UDP-порт
	uint16_t peerPort = 7777;
};

enum class State { Off, Searching, Connected };

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
	void SendPose(const float pos[3], const float rot[4]);
	void SetChapter(uint64_t chapter) { chapter_ = chapter; }
	void SetName(const std::string& name) { name_ = name; }

	State GetState() const { return state_; }
	const std::string& PeerName() const { return peerName_; }
	uint64_t PeerChapter() const { return peerChapter_; }
	uint64_t LastHeardMs() const { return lastHeard_; }

private:
	Packet Make(uint8_t type) const;
	void Send(const Packet& p);
	void SendTo(uint64_t steamId, const Packet& p);
	bool Receive(Packet& p, uint64_t& fromSteam);
	void OnHello(const Packet& p, uint64_t from, uint64_t nowMs);
	void Disconnected(const char* why);

	LogFn log_;
	uint32_t modVersion_;
	std::string name_;
	uint64_t chapter_ = 0;
	State state_ = State::Off;
	bool steam_ = false;
	uintptr_t sock_ = ~uintptr_t(0);
	uint32_t peerAddr_ = 0;
	uint16_t peerPort_ = 0;
	uint64_t peerSteam_ = 0;
	std::string peerName_;
	uint64_t peerChapter_ = 0;
	uint64_t lastHello_ = 0, lastHeard_ = 0, lastMismatchLog_ = 0;
	uint32_t seq_ = 0, lastSeq_ = 0;
	bool oldProtocolLogged_ = false;
};

}  // namespace net
