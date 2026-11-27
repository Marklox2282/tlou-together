#pragma once
// Переносные предметы (доски, приставные лестницы): игрок взял и поставил - у партнёра тот же
// предмет (объект с тем же именем спавнера) переносится на новое место.

#include <cstdint>
#include <vector>

namespace objects {

struct Placement { uint64_t object; float pos[3], rot[4]; };

// Скриптовый контекст, раз за кадр (работа - 4 раза в секунду).
void Tick();
// Любой поток (сеть - в игровом кадре):
std::vector<Placement> TakeOutgoing();  // свои перестановки для отправки
void Receive(const Placement& p);       // от партнёра
void Reset();                           // новое подключение

}  // namespace objects
