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
	std::string myCharacter, partnerCharacter;  // "auto", "companion", "joel"...
	std::string heroName, companionName;  // "joel", "tess"; пусто - неизвестно
	bool authority = false;   // true если этот игрок запускает (хост)
	bool myReady = false;     // наша готовность в главном меню
	bool partnerReady = false;// готовность напарника
	unsigned version = 0;     // версия мода (внизу окна)
};

enum Action { None, CreateLobby, Invite, Leave, SwapRoles, JoinByCode, StartGame, GoToPartner, ToggleReady, HostLaunch };

using LobbyFn = Lobby (*)();  // состояние для текста лобби (вызывается из потока меню)
using ActionFn = void (*)(Action);  // кнопка лобби (вызывается из потока меню)

using CodeFn = std::string (*)();  // код лобби в буфере обмена ("0D7C-RFW"), пусто - нет кода
bool Install(LobbyFn lobby, ActionFn action, CodeFn clipboardCode);  // false - какой-то перехват не встал
void OnFrame();  // из игрового кадра: описание пункта (один раз), слежение за состоянием лобби
// Из скриптового контекста (хук интерпретатора, много раз за кадр - должно быть дёшево):
// закрыть устаревшее окно лобби (menu2-pop-page), чтобы меню открыло новое.
void ScriptTick();
// Сообщение на экране (всплывающее сообщение HUD игры): текст отдаёт перехват текстов по своему
// ключу. Любой поток; показывается из ScriptTick на основном потоке.
void Notify(const std::string& text);
// Хозяин начал игру: открыть меню сюжета и у гостя (как его кнопка НАЧАТЬ ИГРУ). Только если гость
// открывал МУЛЬТИПЛЕЕР в главном меню (знаем объект меню); ждёт, пока окно лобби закрыто.
bool StartGameFromPartner();
// Показать лобби заново, как только наше окно закрыто (например, друг подключился).
void ShowLobbyAgain();

}  // namespace menu
