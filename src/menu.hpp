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
	bool lobby = false;       // мы в лобби Steam
	bool host = false;        // своё лобби
	bool busy = false;        // лобби создаётся или мы в него входим
	std::string owner;        // хозяин чужого лобби
	std::string code;         // код своего лобби ("K7QX-92F")
	bool codeError = false;   // пробовали войти по коду, а в буфере обмена не код
	int failure = 0;          // лобби не создалось (1) / не вошли (2) - net::Link::LobbyFailure
	bool iAmHero = true;      // роли: я герой главы или его спутник
	std::string myCharacter, partnerCharacter;  // "auto", "companion", "joel"...
	std::string heroName, companionName;  // "joel", "tess"; пусто - неизвестно
};

enum Action { None, CreateLobby, Invite, Leave, SwapRoles, JoinByCode };

using LobbyFn = Lobby (*)();  // состояние для текста лобби (вызывается из потока меню)
using ActionFn = void (*)(Action);  // кнопка лобби (вызывается из потока меню)

bool Install(LobbyFn lobby, ActionFn action);  // false - какой-то перехват не встал
void OnFrame();  // из игрового кадра: описание пункта (один раз), слежение за состоянием лобби
// Из скриптового контекста (хук интерпретатора, много раз за кадр - должно быть дёшево):
// закрыть устаревшее окно лобби (menu2-pop-page), чтобы меню открыло новое.
void ScriptTick();

}  // namespace menu
