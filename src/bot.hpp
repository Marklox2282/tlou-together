#pragma once
// Встроенный бот-эмулятор 2-го игрока (F12): подключается к моду по UDP 127.0.0.1:17778 <-> 17777,
// ходит вокруг игрока по сценарию (бег, присед, фонарик, оружие, прицел, выстрел, лечение)
// и проверяет спавн, ошибку следования, Т-позы и доставку сетевых событий дверей/врагов.

#include <cstdint>

#include "engine.hpp"
#include "net.hpp"

namespace bot {

bool Active();
void Toggle(uint64_t nowMs, uint32_t modVersion, net::Link::LogFn log, net::Link& playerLink,
	bool havePlayer, const engine::Locator& me, const engine::Chapter& ch);
void Stop(net::Link& playerLink);
void Tick(uint64_t nowMs, bool havePlayer, const engine::Chapter& ch, net::Link& playerLink);

}  // namespace bot
