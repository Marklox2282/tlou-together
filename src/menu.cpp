#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <safetyhook.hpp>

#include "engine.hpp"
#include "log.hpp"
#include "menu.hpp"

using engine::Sid;

namespace menu {

namespace {

// ---- адреса (Steam build 18995818) ---------------------------------------------------------
// Текст по ключу: const char* (база текстов, ключ = FNV-1a имени вроде "MENU_OPTIONS", bool* найден).
constexpr uintptr_t kTextLookupRva = 0x48EB40;
// Условие "скрыть пункт меню": bool (условие*), имя условия на +8 ("quit-library").
// true = скрыт (проверено в игре: ответ true оставлял пункт скрытым).
constexpr uintptr_t kVisibleRva = 0x1BFE960;
// Обработчик нажатий меню, ветка buttonQuitToLibrary: r15 - меню; конец обработчика 0x1C85F13.
constexpr uintptr_t kPressRva = 0x1C8544A, kPressDoneRva = 0x1C85F13;
// Ответ окна сообщения: rdi - id окна (как у выхода "confirm_exit_to_desktop"), [rsp+0x30] - кнопка.
constexpr uintptr_t kAnswerRva = 0x1C86022;
// Как в обработчике выхода: менеджер страниц по имени стека ("main"/"ingame") и показ окна
// (менеджер, страница, текст, id ответа, 0, 2).
// После создания окна - заголовок (окно, ключ текста); без него окно не показывалось.
constexpr uintptr_t kPageStackRva = 0x236EA68, kMessageBoxRva = 0x127B700, kBoxTitleRva = 0x253ECC0;
constexpr size_t kMainMenuVcall = 0x458;  // меню->вызов: true в главном меню (стек "main"), false в паузе ("ingame")

constexpr uint64_t kLibraryText = Sid("MENU_OPTION_QUIT_TO_LIBRARY_PC");
constexpr uint64_t kLobbyText = Sid("TLOU1COOP_LOBBY");
constexpr uint64_t kLobbyBox = Sid("tlou1coop-lobby");
constexpr uint64_t kQuitLibrary = Sid("quit-library");
constexpr uint64_t kMessageBoxOk = Sid("messageBoxOk");

// ---- переводы ------------------------------------------------------------------------------
// Язык узнаём по тому, как игра сама называет "QUIT TO LIBRARY" (тексты из text2/*.common).
struct Lang {
	const char* library;      // оригинал игры
	const char* title;        // наш пункт
	const char* off;          // лобби: сеть выключена
	const char* searching;    // лобби: ищем
	const char* connected;    // лобби: подключены, %s - ник
	const char* status;       // в названии пункта, пока ищем
};

const Lang kLangs[] = {
	{ "QUIT TO LIBRARY", "MULTIPLAYER",
		"Play the story together with a Steam friend who has this mod.<br>Both of you open Multiplayer and choose Yes.<br><br>Start searching?",
		"Searching for a Steam friend in this game...<br><br>Stop searching?",
		"Connected to %s.<br>In game: F7 - go to your partner, F5 - load your partner's chapter.<br><br>Disconnect?",
		"SEARCHING..." },
	{ "ВЫХОД В БИБЛИОТЕКУ", "МУЛЬТИПЛЕЕР",
		"Проходите сюжет вместе с другом из Steam, у которого тоже стоит мод.<br>Оба откройте «Мультиплеер» и выберите «Да».<br><br>Начать поиск?",
		"Ищем друга из Steam в этой игре...<br><br>Остановить поиск?",
		"Подключено: %s.<br>В игре: F7 - к напарнику, F5 - загрузить главу напарника.<br><br>Отключиться?",
		"ПОИСК..." },
	{ "RETOUR VERS LA BIBLIOTHÈQUE", "MULTIJOUEUR",
		"Jouez l'histoire avec un ami Steam qui a ce mod.<br>Ouvrez tous les deux Multijoueur et choisissez Oui.<br><br>Lancer la recherche ?",
		"Recherche d'un ami Steam dans ce jeu...<br><br>Arrêter la recherche ?",
		"Connecté à %s.<br>En jeu : F7 - rejoindre votre partenaire, F5 - charger son chapitre.<br><br>Se déconnecter ?",
		"RECHERCHE..." },
	{ "BEENDEN UND ZUR BIBLIOTHEK", "MEHRSPIELER",
		"Spielt die Geschichte zusammen mit einem Steam-Freund, der diese Mod hat.<br>Öffnet beide Mehrspieler und wählt Ja.<br><br>Suche starten?",
		"Suche nach einem Steam-Freund in diesem Spiel...<br><br>Suche beenden?",
		"Verbunden mit %s.<br>Im Spiel: F7 - zum Partner, F5 - Kapitel des Partners laden.<br><br>Verbindung trennen?",
		"SUCHE..." },
	{ "TORNA ALLA LIBRERIA", "MULTIGIOCATORE",
		"Gioca la storia insieme a un amico di Steam che ha questa mod.<br>Aprite entrambi Multigiocatore e scegliete Sì.<br><br>Avviare la ricerca?",
		"Ricerca di un amico di Steam in questo gioco...<br><br>Interrompere la ricerca?",
		"Connesso a %s.<br>In gioco: F7 - raggiungi il compagno, F5 - carica il suo capitolo.<br><br>Disconnettersi?",
		"RICERCA..." },
	{ "SALIR A LA BIBLIOTECA", "MULTIJUGADOR",
		"Juega la historia con un amigo de Steam que tenga este mod.<br>Abrid los dos Multijugador y elegid Sí.<br><br>¿Empezar a buscar?",
		"Buscando a un amigo de Steam en este juego...<br><br>¿Dejar de buscar?",
		"Conectado con %s.<br>En el juego: F7 - ir con tu compañero, F5 - cargar su capítulo.<br><br>¿Desconectar?",
		"BUSCANDO..." },
	{ "SAIR PARA A BIBLIOTECA", "MULTIJOGADOR",
		"Jogue a história com um amigo da Steam que tenha este mod.<br>Os dois abrem Multijogador e escolhem Sim.<br><br>Começar a procurar?",
		"Procurando um amigo da Steam neste jogo...<br><br>Parar de procurar?",
		"Conectado a %s.<br>No jogo: F7 - ir até o parceiro, F5 - carregar o capítulo dele.<br><br>Desconectar?",
		"PROCURANDO..." },
	{ "WYJDŹ DO BIBLIOTEKI", "TRYB WIELOOSOBOWY",
		"Przejdź fabułę razem ze znajomym ze Steam, który ma ten mod.<br>Obaj otwórzcie Tryb wieloosobowy i wybierzcie Tak.<br><br>Rozpocząć wyszukiwanie?",
		"Szukanie znajomego ze Steam w tej grze...<br><br>Przerwać wyszukiwanie?",
		"Połączono z: %s.<br>W grze: F7 - do partnera, F5 - wczytaj rozdział partnera.<br><br>Rozłączyć?",
		"SZUKANIE..." },
	{ "KÜTÜPHANEYE ÇIK", "ÇOK OYUNCULU",
		"Hikâyeyi bu moda sahip bir Steam arkadaşınla birlikte oyna.<br>İkiniz de Çok Oyunculu'yu açıp Evet'i seçin.<br><br>Aramaya başlansın mı?",
		"Bu oyunda bir Steam arkadaşı aranıyor...<br><br>Arama durdurulsun mu?",
		"%s ile bağlanıldı.<br>Oyunda: F7 - ortağına git, F5 - ortağının bölümünü yükle.<br><br>Bağlantı kesilsin mi?",
		"ARANIYOR..." },
	{ "ODEJÍT DO KNIHOVNY", "HRA VÍCE HRÁČŮ",
		"Hrajte příběh společně s přítelem ze Steamu, který má tento mód.<br>Oba otevřete Hru více hráčů a zvolte Ano.<br><br>Začít hledat?",
		"Hledání přítele ze Steamu v této hře...<br><br>Přestat hledat?",
		"Připojeno k: %s.<br>Ve hře: F7 - k parťákovi, F5 - načíst jeho kapitolu.<br><br>Odpojit?",
		"HLEDÁNÍ..." },
	{ "KILÉPÉS A KÖNYVTÁRBA", "TÖBBJÁTÉKOS",
		"Játsszátok végig a történetet egy Steam-baráttal, akinél szintén megvan ez a mod.<br>Mindketten nyissátok meg a Többjátékos menüt, és válasszátok az Igent.<br><br>Indulhat a keresés?",
		"Steam-barát keresése ebben a játékban...<br><br>Leállítod a keresést?",
		"Csatlakozva: %s.<br>Játékban: F7 - a társadhoz, F5 - a társad fejezetének betöltése.<br><br>Lecsatlakozol?",
		"KERESÉS..." },
	{ "ΕΞΟΔΟΣ ΣΤΗ ΒΙΒΛΙΟΘΗΚΗ", "ΠΟΛΛΟΙ ΠΑΙΚΤΕΣ",
		"Παίξτε την ιστορία μαζί με έναν φίλο από το Steam που έχει αυτό το mod.<br>Ανοίξτε και οι δύο το Πολλοί παίκτες και επιλέξτε Ναι.<br><br>Έναρξη αναζήτησης;",
		"Αναζήτηση φίλου από το Steam σε αυτό το παιχνίδι...<br><br>Διακοπή αναζήτησης;",
		"Συνδέθηκες με: %s.<br>Στο παιχνίδι: F7 - στον συμπαίκτη, F5 - φόρτωση του κεφαλαίου του.<br><br>Αποσύνδεση;",
		"ΑΝΑΖΗΤΗΣΗ..." },
	{ "STOPPEN EN NAAR BIBLIOTHEEK", "MULTIPLAYER",
		"Speel het verhaal samen met een Steam-vriend die deze mod heeft.<br>Open allebei Multiplayer en kies Ja.<br><br>Beginnen met zoeken?",
		"Zoeken naar een Steam-vriend in dit spel...<br><br>Stoppen met zoeken?",
		"Verbonden met %s.<br>In het spel: F7 - naar je partner, F5 - hoofdstuk van je partner laden.<br><br>Verbinding verbreken?",
		"ZOEKEN..." },
	{ "AFSLUT TIL BIBLIOTEK", "MULTIPLAYER",
		"Spil historien sammen med en Steam-ven, der har dette mod.<br>Åbn begge Multiplayer, og vælg Ja.<br><br>Start søgning?",
		"Søger efter en Steam-ven i dette spil...<br><br>Stop søgning?",
		"Forbundet til %s.<br>I spillet: F7 - gå til din makker, F5 - indlæs din makkers kapitel.<br><br>Afbryd forbindelsen?",
		"SØGER..." },
	{ "POISTU KIRJASTOON", "MONINPELI",
		"Pelaa tarina yhdessä Steam-kaverin kanssa, jolla on tämä modi.<br>Avatkaa kumpikin Moninpeli ja valitkaa Kyllä.<br><br>Aloitetaanko haku?",
		"Etsitään Steam-kaveria tästä pelistä...<br><br>Lopetetaanko haku?",
		"Yhdistetty: %s.<br>Pelissä: F7 - pelikaverin luo, F5 - lataa pelikaverin luku.<br><br>Katkaistaanko yhteys?",
		"HAETAAN..." },
	{ "GÅ TIL BIBLIOTEKET", "FLERSPILLER",
		"Spill historien sammen med en Steam-venn som har denne moden.<br>Åpne begge Flerspiller og velg Ja.<br><br>Starte søket?",
		"Søker etter en Steam-venn i dette spillet...<br><br>Stoppe søket?",
		"Koblet til %s.<br>I spillet: F7 - gå til partneren, F5 - last inn partnerens kapittel.<br><br>Koble fra?",
		"SØKER..." },
	{ "AVSLUTA TILL BIBLIOTEKET", "FLERSPELARLÄGE",
		"Spela berättelsen tillsammans med en Steam-vän som har den här modden.<br>Öppna båda Flerspelarläge och välj Ja.<br><br>Börja söka?",
		"Söker efter en Steam-vän i det här spelet...<br><br>Sluta söka?",
		"Ansluten till %s.<br>I spelet: F7 - gå till din partner, F5 - ladda din partners kapitel.<br><br>Koppla från?",
		"SÖKER..." },
	{ "VRATI SE U LIBRARY", "VIŠE IGRAČA",
		"Igrajte priču zajedno s prijateljem sa Steama koji ima ovaj mod.<br>Obojica otvorite Više igrača i odaberite Da.<br><br>Započeti traženje?",
		"Traženje prijatelja sa Steama u ovoj igri...<br><br>Zaustaviti traženje?",
		"Povezano s: %s.<br>U igri: F7 - do partnera, F5 - učitaj partnerovo poglavlje.<br><br>Prekinuti vezu?",
		"TRAŽENJE..." },
	{ "ライブラリに戻る", "マルチプレイ",
		"このMODを入れたSteamのフレンドと一緒にストーリーを遊べます。<br>2人とも「マルチプレイ」を開いて「はい」を選んでください。<br><br>検索を開始しますか？",
		"このゲームをプレイ中のSteamフレンドを検索中...<br><br>検索をやめますか？",
		"%s と接続しました。<br>ゲーム中: F7 - パートナーの所へ、F5 - パートナーのチャプターをロード。<br><br>切断しますか？",
		"検索中..." },
	{ "라이브러리로 나가기", "멀티플레이",
		"이 모드를 설치한 Steam 친구와 함께 스토리를 플레이하세요.<br>두 사람 모두 멀티플레이를 열고 예를 선택하세요.<br><br>검색을 시작할까요?",
		"이 게임을 하는 Steam 친구를 찾는 중...<br><br>검색을 중지할까요?",
		"%s 님과 연결되었습니다.<br>게임 중: F7 - 파트너에게 이동, F5 - 파트너의 챕터 불러오기.<br><br>연결을 끊을까요?",
		"검색 중..." },
	{ "退出至游戏库", "多人游戏",
		"与同样安装了此模组的 Steam 好友一起体验剧情。<br>双方都打开“多人游戏”并选择“是”。<br><br>开始搜索？",
		"正在搜索此游戏中的 Steam 好友...<br><br>停止搜索？",
		"已连接到 %s。<br>游戏中：F7 - 前往同伴处，F5 - 载入同伴的章节。<br><br>断开连接？",
		"搜索中..." },
	{ "返回收藏庫", "多人遊戲",
		"與同樣安裝了此模組的 Steam 好友一起體驗劇情。<br>雙方都打開「多人遊戲」並選擇「是」。<br><br>開始搜尋？",
		"正在搜尋此遊戲中的 Steam 好友...<br><br>停止搜尋？",
		"已連線至 %s。<br>遊戲中：F7 - 前往夥伴處，F5 - 載入夥伴的章節。<br><br>中斷連線？",
		"搜尋中..." },
	{ "ออกไปยังคลังเกม", "ผู้เล่นหลายคน",
		"เล่นเนื้อเรื่องร่วมกับเพื่อนใน Steam ที่ติดตั้งม็อดนี้<br>ทั้งสองคนเปิดผู้เล่นหลายคนแล้วเลือกใช่<br><br>เริ่มค้นหาหรือไม่",
		"กำลังค้นหาเพื่อนใน Steam ที่เล่นเกมนี้...<br><br>หยุดค้นหาหรือไม่",
		"เชื่อมต่อกับ %s แล้ว<br>ในเกม: F7 - ไปหาคู่หู, F5 - โหลดบทของคู่หู<br><br>ตัดการเชื่อมต่อหรือไม่",
		"กำลังค้นหา..." },
};

const Lang& LangFor(const char* library) {
	if (library)
		for (const Lang& l : kLangs)
			if (!strcmp(l.library, library)) return l;
	return kLangs[0];
}

LobbyFn g_lobby = nullptr;
ToggleFn g_toggle = nullptr;
safetyhook::InlineHook g_textHook, g_visibleHook;
safetyhook::MidHook g_pressHook, g_answerHook;

// Название пункта с состоянием: "MULTIPLAYER · ПОИСК..." / "MULTIPLAYER · ник".
char g_titleText[2][256];
int g_titleTurn = 0;

const char* TitleText(const Lang& l) {
	const Lobby s = g_lobby ? g_lobby() : Lobby{};
	if (!s.on) return l.title;
	char* buf = g_titleText[g_titleTurn ^= 1];
	snprintf(buf, sizeof(g_titleText[0]), "%s · %s", l.title, s.connected ? s.partner.c_str() : l.status);
	return buf;
}

// Строка лобби должна жить, пока игра её показывает: два буфера по очереди.
char g_lobbyText[2][1024];
int g_lobbyTurn = 0;

const char* LobbyText(const Lang& l) {
	const Lobby s = g_lobby ? g_lobby() : Lobby{};
	char* buf = g_lobbyText[g_lobbyTurn ^= 1];
	if (s.connected) snprintf(buf, sizeof(g_lobbyText[0]), l.connected, s.partner.c_str());
	else snprintf(buf, sizeof(g_lobbyText[0]), "%s", s.on ? l.searching : l.off);
	return buf;
}

const char* TextLookup(void* db, uint64_t key, bool* found) {
	if (key == kLibraryText || key == kLobbyText) {
		const Lang& l = LangFor(g_textHook.call<const char*>(db, kLibraryText, nullptr));
		if (found) *found = true;
		return key == kLobbyText ? LobbyText(l) : TitleText(l);
	}
	return g_textHook.call<const char*>(db, key, found);
}

// Вызывается на каждый пункт меню каждый кадр: читаем прямо (указатель дала сама игра).
bool Visible(uintptr_t condition, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
	if (condition && *reinterpret_cast<const uint64_t*>(condition + 8) == kQuitLibrary) return false;
	return g_visibleHook.call<bool>(condition, a2, a3, a4);
}

// Нажали наш пункт: окно "да/нет" игры с текстом лобби вместо подтверждения выхода.
void OnPress(safetyhook::Context& ctx) {
	const uintptr_t base = engine::g_base, menu = ctx.r15;
	using MainMenuFn = bool (*)(uintptr_t);
	const auto mainMenu = reinterpret_cast<MainMenuFn>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(menu) + kMainMenuVcall));
	using StackFn = uintptr_t (*)(uintptr_t, uint64_t);
	using BoxFn = uintptr_t (*)(uintptr_t, const char*, uint64_t, uint64_t, uint64_t, int);
	const uintptr_t stack = reinterpret_cast<StackFn>(base + kPageStackRva)(0, mainMenu(menu) ? Sid("main") : Sid("ingame"));
	const uintptr_t box = stack ? reinterpret_cast<BoxFn>(base + kMessageBoxRva)(stack, "menu/message-box-yes-no", kLobbyText, kLobbyBox, 0, 2) : 0;
	if (box) reinterpret_cast<void (*)(uintptr_t, uint64_t)>(base + kBoxTitleRva)(box, kLibraryText);
	Log("menu: multiplayer pressed, lobby %s", box ? "opened" : "FAILED");
	ctx.rip = base + kPressDoneRva;
}

// Ответ в окне: наше - обрабатываем сами, а игре подсовываем id, которого она не знает.
void OnAnswer(safetyhook::Context& ctx) {
	if (ctx.rdi != kLobbyBox) return;
	const bool yes = *reinterpret_cast<const uint64_t*>(ctx.rsp + 0x30) == kMessageBoxOk;
	if (yes) Log("menu: lobby answered yes");  // второе событие того же окна - закрытие, не ответ
	if (yes && g_toggle) g_toggle();
	ctx.rdi = 1;
}

template <typename Hook, typename Fn>
bool Make(Hook& out, uintptr_t rva, Fn fn, const char* what) {
	auto h = Hook::create(reinterpret_cast<void*>(engine::g_base + rva), fn);
	if (h) out = std::move(*h);
	if (!h) Log("menu: %s hook FAILED", what);
	return static_cast<bool>(h);
}

}  // namespace

bool Install(LobbyFn lobby, ToggleFn toggle) {
	g_lobby = lobby;
	g_toggle = toggle;
	// Порядок важен: сначала нажатие и ответ, видимость - последней. Если что-то не встало,
	// снимаем всё: иначе видимый пункт выходил бы в библиотеку.
	const bool ok = Make(g_pressHook, kPressRva, OnPress, "press") && Make(g_answerHook, kAnswerRva, OnAnswer, "answer") &&
		Make(g_textHook, kTextLookupRva, TextLookup, "text") && Make(g_visibleHook, kVisibleRva, Visible, "visibility");
	if (!ok) {
		g_visibleHook = {};
		g_textHook = {};
		g_answerHook = {};
		g_pressHook = {};
	}
	Log("menu: %s", ok ? "multiplayer item on" : "multiplayer item off");
	return ok;
}

}  // namespace menu
