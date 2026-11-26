#pragma once
// Партнёр в мире: NPC-марионетка без своей воли, которая идёт по следу (позы друга из сети или,
// для проверки без сети, свои позы с задержкой), и перенос игрока к нему.

#include <cstdint>

#include "engine.hpp"

namespace puppet {

// Из игрового кадра (основной поток):
void Record(uint64_t nowMs, const engine::Locator& l);  // точка следа
void UseNetwork(bool on);       // on: след - позы друга, задержка 120 мс; off: свои позы, 1 с
bool UsingNetwork();
void SetWanted(bool on);        // марионетка нужна (F10 или подключился друг)
bool Wanted();
void SetFollow(bool on);
bool Follow();
void RequestTeleport();         // F7
void SetNetActive(bool on);     // сеть включена: читать свою главу для отправки
void SetPartnerChapter(uint64_t chapter);
uint64_t MyChapter();

// Раз за кадр в скриптовом контексте (хук интерпретатора).
void ScriptTick();

uint64_t Current();  // id марионетки (для журнала падений)

}  // namespace puppet
