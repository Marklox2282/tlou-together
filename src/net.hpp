#pragma once
// Связь двух игроков: Steam (друзья, через Valve, без портов) или UDP по IP (локальная сеть, тесты).
// Без игровых зависимостей, поэтому проверяется отдельной программой test/netsim.cpp.

#include <cstdint>
#include <string>

namespace net {

constexpr uint32_t kMagic = 0x4F433154;  // "T1CO"
constexpr uint8_t kVersion = 1;
enum : uint8_t { kHello = 1, kPose = 2, kBye = 3 };

#pragma pack(push, 1)
struct Packet {
	uint32_t magic = kMagic;
	uint8_t version = kVersion;
	uint8_t type = 0;
	uint16_t reserved = 0;
	uint32_t seq = 0;
	float pos[3]{};
	float rot[4]{};
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 40);

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
	explicit Link(LogFn log) : log_(log) {}
	~Link() { Stop(); }

	bool Start(const Config& cfg, uint64_t nowMs);
	void Stop();
	// Отправляет приветствия при поиске и принимает пакеты. Возвращает true, если пришла поза.
	bool Tick(uint64_t nowMs, Packet& pose);
	void SendPose(const float pos[3], const float rot[4]);

	State GetState() const { return state_; }
	const std::string& PeerName() const { return peerName_; }
	uint64_t LastHeardMs() const { return lastHeard_; }

private:
	void Send(const Packet& p);
	void SendTo(uint64_t steamId, const Packet& p);
	bool Receive(Packet& p, uint64_t& fromSteam);
	void OnConnected(uint64_t steamId, uint64_t nowMs);

	LogFn log_;
	State state_ = State::Off;
	bool steam_ = false;
	uintptr_t sock_ = ~uintptr_t(0);
	uint32_t peerAddr_ = 0;
	uint16_t peerPort_ = 0;
	uint64_t peerSteam_ = 0;
	std::string peerName_;
	uint64_t lastHello_ = 0, lastHeard_ = 0;
	uint32_t seq_ = 0, lastSeq_ = 0;
};

}  // namespace net
