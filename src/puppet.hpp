#pragma once
// Партнёр в мире: NPC-марионетка без своей воли, которая идёт по следу (позы друга из сети или,
// для проверки без сети, свои позы с задержкой), и перенос игрока к нему.

#include <cstdint>
#include <string>

#include "engine.hpp"

namespace puppet {

// Из игрового кадра (основной поток):
void Record(uint64_t nowMs, const engine::Locator& l, uint32_t senderMs = 0);  // точка следа
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
// Общая смерть: свой игрок погиб (для отправки) и "партнёр погиб" - через 2 с перезагрузить свой
// чекпоинт, чтобы оба начали с одного места.
bool TakeMyDeath();
void PartnerDied();
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
// Персонаж игрока - каждый выбирает сам (ini character=, кнопка в лобби), выбор уходит партнёру:
//  0 auto      - герой главы, как сам игрок (Сара и Сара, Джоэл и Джоэл): родные анимации;
//  1 companion - спутник главы: у партнёра им становится настоящий спутник (Тесс...) без ИИ,
//                у себя игрок получает облик спутника, а свой ИИ-спутник спрятан;
//  2+          - конкретный персонаж (joel, ellie...), если он есть в загруженных уровнях.
constexpr int kCharacterCount = 13;
const char* CharacterName(int c);           // "auto", "companion", "joel"...
int CharacterFromName(const std::string& name);
uint64_t CharacterSid(int c);               // для сети
int CharacterFromSid(uint64_t sid);         // 0, если неизвестен
void SetMyCharacter(int c);
int MyCharacter();
void SetPartnerCharacter(int c);
std::string MyLook();                       // свой облик для партнёра (арт), пусто - не знаем
void SetPartnerLook(const std::string& art);  // облик партнёра от него самого
int PartnerCharacter();
enum class Role { Hero, Companion };
bool MeFiring();                   // свой игрок стреляет (читается в скриптовом потоке)
void SetPartnerFiring(bool on);    // партнёр стреляет: марионетка стреляет тоже
uint64_t MyWeapon();                 // оружие в руках своего игрока (0 - нет или неизвестно)
void SetPartnerWeapon(uint64_t weapon);  // оружие партнёра: то же даётся марионетке
// Поза: присел, целится. Своя - для отправки; партнёра - марионетка садится и поднимает оружие.
bool MeCrouched();
bool MeAiming();
void SetPartnerStance(bool crouched, bool aiming);
// Враги видят копию партнёра (ini enemies_see_partner, по умолчанию да): она во фракции игрока,
// не "игнорируется" ИИ; урон по ней партнёру не передаётся - у него свои враги бьют его самого.
void SetEnemiesSeePartner(bool on);
// Контур партнёра сквозь стены, когда он дальше 12 м (ini partner_outline, по умолчанию да).
void SetPartnerOutline(bool on);
// Сложность: своя (для ведущей стороны) и "играть на сложности хоста" (гость); -1 - вернуть свою.
int MyDifficulty();
void FollowDifficulty(int difficulty);
Role GetRole();                             // Companion, если мой персонаж - спутник
// Имена персонажей текущей главы для лобби ("joel", "tess"); пусто - неизвестно.
std::string HeroCharacter();
std::string CompanionCharacter();

}  // namespace puppet
