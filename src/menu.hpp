#pragma once
// Пункт "МУЛЬТИПЛЕЕР" в главном меню (и в меню паузы) самой игры. Под него занят скрытый на ПК
// пункт "QUIT TO LIBRARY": мод делает его видимым, подменяет подпись на языке игры, а нажатие
// открывает игровое окно "да/нет" с лобби вместо выхода в библиотеку Steam.

#include <string>

namespace menu {

struct Lobby {
	bool on = false;          // сеть включена (поиск или подключены)
	bool connected = false;
	std::string partner;      // ник партнёра, если подключены
};

using LobbyFn = Lobby (*)();  // состояние для текста лобби (вызывается из потока меню)
using ToggleFn = void (*)();  // "Да" в лобби: включить поиск или отключиться

bool Install(LobbyFn lobby, ToggleFn toggle);  // false - какой-то перехват не встал

}  // namespace menu
