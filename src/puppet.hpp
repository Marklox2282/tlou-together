#pragma once
// Партнёр в мире: NPC-марионетка без своей воли, которая идёт по следу (позы друга из сети или,
// для проверки без сети, свои позы с задержкой), и перенос игрока к нему.

#include <cstdint>
#include <string>

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
// F5: загрузить главу и чекпоинт партнёра и после загрузки встать рядом с ним;
// own (Shift+F5) - перезагрузить свою, проверка без партнёра.
void RequestJoin(bool own);
void SetNetActive(bool on);     // сеть включена: читать свою главу для отправки
void SetPartnerChapter(const engine::Chapter& c);
engine::Chapter MyChapter();

// Раз за кадр в скриптовом контексте (хук интерпретатора); OnFrame - из обработчика кадра.
void OnFrame();
void ScriptTick();
uint64_t TakeWorkMicroseconds();  // время работы ScriptTick с прошлого вызова

uint64_t Current();  // id марионетки (для журнала падений)

// Роли. Герой - главный персонаж главы (Сара, Джоэл, Элли); у него партнёра изображает настоящий
// спутник главы (Тесс, Элли, Билл...) с выключенным ИИ, а если спутника нет - копия NPC.
// Напарник - сам выглядит как спутник главы, а его ИИ-спутник спрятан; партнёр (герой) у него -
// копия NPC в облике героя.
enum class Role { Hero, Companion };
bool MeFiring();                   // свой игрок стреляет (читается в скриптовом потоке)
void SetPartnerFiring(bool on);    // партнёр стреляет: марионетка стреляет тоже
void SetRole(Role r);
Role GetRole();
// Имена персонажей текущей главы для лобби ("joel", "tess"); пусто - неизвестно.
std::string HeroCharacter();
std::string CompanionCharacter();

}  // namespace puppet
