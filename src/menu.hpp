#pragma once
// Пункт "МУЛЬТИПЛЕЕР" в главном меню (и в меню паузы) самой игры. Под него занят скрытый на ПК
// пункт "QUIT TO LIBRARY": мод делает его видимым, подменяет подпись на языке игры, а нажатие
// открывает игровое окно "да/нет" с лобби вместо выхода в библиотеку Steam.

#include <string>
#include <vector>

namespace menu {

struct Lobby {
	bool on = false;          // сеть включена (поиск или подключены)
	bool connected = false;
	std::string partner;      // ник партнёра, если подключены
	struct Friend { std::string name; int status; };  // status: 0 в игре, 1 готов, 2 другая версия мода
	std::vector<Friend> friends;  // друзья из Steam в этой игре
};

using LobbyFn = Lobby (*)();  // состояние для текста лобби (вызывается из потока меню)
using ToggleFn = void (*)();  // "Да" в лобби: включить поиск или отключиться

bool Install(LobbyFn lobby, ToggleFn toggle);  // false - какой-то перехват не встал
void OnFrame();  // из игрового кадра: один раз после запуска игры дописать описание пункта

}  // namespace menu
