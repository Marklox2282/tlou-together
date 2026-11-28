#pragma once
// Переносные предметы (доски, приставные лестницы): игрок взял и поставил - у партнёра тот же
// предмет (объект с тем же именем спавнера) переносится на новое место.

#include <cstdint>
#include <vector>

namespace objects {

struct Placement { uint64_t object; float pos[3], rot[4]; };

// Поставить хук на открытие дверей (после проверки версии игры).
bool InitDoors();
void UninstallDoors();  // обновление мода на лету

// Скриптовый контекст, раз за кадр (двери - каждый кадр, доски/лестницы - 4 раза в секунду).
void Tick();
// Любой поток (сеть - в игровом кадре):
std::vector<Placement> TakeOutgoing();  // свои перестановки для отправки
void Receive(const Placement& p);       // от партнёра
std::vector<uint64_t> TakeDoors();      // открытые нами двери для отправки партнёру
void ReceiveDoor(uint64_t door);        // дверь, открытая партнёром
void ResendDoors();                     // дослать уже открытые двери опоздавшему партнёру
void OnReload();                        // перезагрузка чекпоинта / смерть
void Reset();                           // новое подключение

}  // namespace objects
