#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include <safetyhook.hpp>

#include "clipboard.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "menu.hpp"

using engine::Sid;
using engine::Call;
using engine::F;
using engine::g_dc;

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
// Реестр "источников данных" пунктов главного меню: (реестр, класс, имя пункта, текст, описание, 0).
// Так игра даёт описание под списком ("Play the main story..."); наш пункт добавляем туда же.
constexpr uintptr_t kDataSourcesRva = 0x8089550, kAddDataSourceRva = 0x25D6390;
constexpr size_t kMainMenuVcall = 0x458;  // меню->вызов: true в главном меню (стек "main"), false в паузе ("ingame")

constexpr uint64_t kLibraryText = Sid("MENU_OPTION_QUIT_TO_LIBRARY_PC");
constexpr uint64_t kLobbyText = Sid("TLOU1COOP_LOBBY"), kLobbyText2 = Sid("TLOU1COOP_LOBBY2");  // чередуются: новый ключ - игра точно перечитает текст
constexpr uint64_t kDescText = Sid("TLOU1COOP_DESCRIPTION");
constexpr uint64_t kNoteText = Sid("TLOU1COOP_NOTE");  // текст сообщения на экране
constexpr uint64_t kLobbyBox = Sid("tlou1coop-lobby");
constexpr uint64_t kQuitLibrary = Sid("quit-library");
constexpr uint64_t kMessageBoxOk = Sid("messageBoxOk"), kMessageBoxCancel = Sid("messageBoxCancel");

// ---- переводы ------------------------------------------------------------------------------
// Язык узнаём по тому, как игра сама называет "QUIT TO LIBRARY" (тексты из text2/*.common).
struct Lang {
	const char* library;    // оригинал игры
	const char* title;      // наш пункт
	const char* intro;      // лобби ещё нет
	const char* hostWait;   // своё лобби, ждём друга
	const char* joinWait;   // в чужом лобби, %s - хозяин
	const char* connected;  // подключены, %s - ник
	const char* busy;       // создаём / входим / ищем
	const char* create;     // кнопки
	const char* invite;
	const char* leave;
	const char* tag;        // в названии пункта, пока в лобби
	const char* swap;       // кнопка, когда подключены
	const char* you;        // "Вы: Джоэл (герой)"
	const char* hero;
	const char* companion;
	const char* joinCode;   // кнопка
	const char* codeLine;   // %s - код лобби
	const char* noCode;     // в буфере обмена не код
	const char* help;       // что делают кнопки первого окна
	const char* refresh;    // окно не обновляется само
	const char* waitStart;  // нажали начать игру, а напарник ещё не подключён
	const char* createFailed;  // Steam не создал лобби
	const char* joinFailed;    // не вошли в лобби по коду / приглашению
	const char* start;         // кнопка "начать игру", когда подключены
	const char* leaveHint;     // как выйти из лобби, когда подключены
	const char* inClipboard;   // %s - код в буфере обмена
	const char* noClipboard;   // в буфере нет кода
	const char* goTo;          // кнопка в меню паузы: к напарнику (как F7)
};

const Lang kLangs[] = {
	{ "QUIT TO LIBRARY", "MULTIPLAYER",
		"Create a lobby and invite a Steam friend to play the story together.<br>Your friend needs this mod too.",
		"Your lobby is open.<br>Invite a friend: the Steam window opens, choose a friend there.<br>When they accept, you connect automatically.",
		"You joined %s's lobby.<br>Close this window (Esc) and wait: when the host presses START GAME, the story menu opens by itself. Choose Continue and you will be taken to the host.",
		"Connected to %s.<br>In game: F7 - go to your partner, F5 - load your partner's chapter.",
		"Connecting...", "CREATE LOBBY", "INVITE FRIEND", "LEAVE LOBBY", "LOBBY",
		"CHANGE CHARACTER", "You", "hero", "companion",
		"JOIN BY CODE",
		"Lobby code: %s<br>It is copied - paste it to your friend in a chat.",
		"Copy your friend's lobby code first (Ctrl+C), then choose Join by code.",
		"CREATE LOBBY - you host and get a code for your friend.<br>JOIN BY CODE - copy your friend's code first (Ctrl+C).",
		"This window does not update by itself: close it and open Multiplayer again in a few seconds.",
		"Your partner is still connecting. You can start the game once they are here.",
		"Could not create the lobby: no connection to Steam. Check that Steam is online and that a VPN does not block it.",
		"Could not join the lobby: it is closed, full or the code is wrong.",
		"START GAME", "F6 - leave the lobby.", "In your clipboard: %s", "Your clipboard has no lobby code.",
		"GO TO PARTNER" },
	{ "ВЫХОД В БИБЛИОТЕКУ", "МУЛЬТИПЛЕЕР",
		"Создайте лобби и пригласите друга из Steam, чтобы пройти сюжет вместе.<br>У друга тоже должен стоять мод.",
		"Лобби создано.<br>Пригласите друга: откроется окно Steam, выберите в нём друга.<br>Когда он примет приглашение, вы соединитесь сами.",
		"Вы в лобби игрока %s.<br>Закройте это окно (Esc) и ждите: когда хозяин нажмёт НАЧАТЬ ИГРУ, меню сюжета откроется само. Выберите «Продолжить» - вас перенесёт к хозяину.",
		"Подключено: %s.<br>В игре: F7 - к напарнику, F5 - загрузить главу напарника.",
		"Соединение...", "СОЗДАТЬ ЛОББИ", "ПРИГЛАСИТЬ ДРУГА", "ПОКИНУТЬ ЛОББИ", "ЛОББИ",
		"СМЕНИТЬ ПЕРСОНАЖА", "Вы", "герой", "напарник",
		"ВОЙТИ ПО КОДУ",
		"Код лобби: %s<br>Он уже скопирован - вставьте его другу в чат.",
		"Сначала скопируйте код лобби друга (Ctrl+C), потом выберите «Войти по коду».",
		"СОЗДАТЬ ЛОББИ - вы хост и получаете код для друга.<br>ВОЙТИ ПО КОДУ - сначала скопируйте код друга (Ctrl+C).",
		"Это окно само не обновляется: закройте его и откройте «Мультиплеер» снова через пару секунд.",
		"Напарник ещё подключается. Начать игру можно, когда он подключится.",
		"Не удалось создать лобби: нет связи со Steam. Проверьте, что Steam в сети и VPN его не блокирует.",
		"Не удалось войти в лобби: оно закрыто, заполнено или код неверный.",
		"НАЧАТЬ ИГРУ", "F6 - покинуть лобби.", "В буфере обмена: %s", "В буфере обмена нет кода лобби.",
		"К НАПАРНИКУ" },
	{ "RETOUR VERS LA BIBLIOTHÈQUE", "MULTIJOUEUR",
		"Créez un salon et invitez un ami Steam pour jouer l'histoire ensemble.<br>Votre ami doit aussi avoir ce mod.",
		"Votre salon est ouvert.<br>Invitez un ami : la fenêtre Steam s'ouvre, choisissez-y un ami.<br>Dès qu'il accepte, la connexion se fait toute seule.",
		"Vous avez rejoint le salon de %s.<br>Fermez cette fenêtre (Échap) et patientez : quand l'hôte lance la partie, le menu de l'histoire s'ouvre tout seul. Choisissez Continuer pour rejoindre l'hôte.",
		"Connecté à %s.<br>En jeu : F7 - rejoindre votre partenaire, F5 - charger son chapitre.",
		"Connexion...", "CRÉER UN SALON", "INVITER UN AMI", "QUITTER LE SALON", "SALON",
		"CHANGER DE PERSONNAGE", "Vous", "héros", "compagnon",
		"REJOINDRE PAR CODE",
		"Code du salon : %s<br>Il est copié - collez-le à votre ami dans un chat.",
		"Copiez d'abord le code du salon de votre ami (Ctrl+C), puis choisissez Rejoindre par code.",
		"CRÉER UN SALON - vous hébergez et recevez un code pour votre ami.<br>REJOINDRE PAR CODE - copiez d'abord le code de votre ami (Ctrl+C).",
		"Cette fenêtre ne se met pas à jour : fermez-la et rouvrez Multijoueur dans quelques secondes.",
		"Votre partenaire se connecte encore. Vous pourrez lancer la partie quand il sera là.",
		"Impossible de créer le salon : pas de connexion à Steam. Vérifiez que Steam est en ligne et qu'un VPN ne le bloque pas.",
		"Impossible de rejoindre le salon : il est fermé, plein ou le code est faux.",
		"COMMENCER", "F6 - quitter le salon.", "Dans votre presse-papiers : %s", "Votre presse-papiers ne contient pas de code.",
		"REJOINDRE LE PARTENAIRE" },
	{ "BEENDEN UND ZUR BIBLIOTHEK", "MEHRSPIELER",
		"Erstelle eine Lobby und lade einen Steam-Freund ein, um die Geschichte gemeinsam zu spielen.<br>Dein Freund braucht diese Mod auch.",
		"Deine Lobby ist offen.<br>Lade einen Freund ein: Das Steam-Fenster öffnet sich, wähle dort einen Freund.<br>Sobald er annimmt, verbindet ihr euch automatisch.",
		"Du bist in der Lobby von %s.<br>Schließe dieses Fenster (Esc) und warte: Startet der Host das Spiel, öffnet sich das Story-Menü von selbst. Wähle Fortsetzen, dann kommst du zum Host.",
		"Verbunden mit %s.<br>Im Spiel: F7 - zum Partner, F5 - Kapitel des Partners laden.",
		"Verbinde...", "LOBBY ERSTELLEN", "FREUND EINLADEN", "LOBBY VERLASSEN", "LOBBY",
		"FIGUR WECHSELN", "Du", "Held", "Begleiter",
		"MIT CODE BEITRETEN",
		"Lobby-Code: %s<br>Er ist kopiert - schick ihn deinem Freund im Chat.",
		"Kopiere zuerst den Lobby-Code deines Freundes (Strg+C) und wähle dann Mit Code beitreten.",
		"LOBBY ERSTELLEN - du bist Host und bekommst einen Code für deinen Freund.<br>MIT CODE BEITRETEN - kopiere zuerst den Code deines Freundes (Strg+C).",
		"Dieses Fenster aktualisiert sich nicht: schließe es und öffne Mehrspieler in ein paar Sekunden erneut.",
		"Dein Partner verbindet sich noch. Du kannst das Spiel starten, sobald er da ist.",
		"Lobby konnte nicht erstellt werden: keine Verbindung zu Steam. Prüfe, ob Steam online ist und kein VPN es blockiert.",
		"Lobby-Beitritt fehlgeschlagen: geschlossen, voll oder falscher Code.",
		"SPIEL STARTEN", "F6 - Lobby verlassen.", "In der Zwischenablage: %s", "In der Zwischenablage ist kein Lobby-Code.",
		"ZUM PARTNER" },
	{ "TORNA ALLA LIBRERIA", "MULTIGIOCATORE",
		"Crea una lobby e invita un amico di Steam per giocare la storia insieme.<br>Anche il tuo amico deve avere questa mod.",
		"La tua lobby è aperta.<br>Invita un amico: si apre la finestra di Steam, scegli lì un amico.<br>Quando accetta, vi collegate automaticamente.",
		"Sei nella lobby di %s.<br>Chiudi questa finestra (Esc) e attendi: quando l'host avvia la partita, il menu della storia si apre da solo. Scegli Continua e verrai portato dall'host.",
		"Connesso a %s.<br>In gioco: F7 - raggiungi il compagno, F5 - carica il suo capitolo.",
		"Connessione...", "CREA LOBBY", "INVITA UN AMICO", "ESCI DALLA LOBBY", "LOBBY",
		"CAMBIA PERSONAGGIO", "Tu", "eroe", "compagno",
		"ENTRA CON CODICE",
		"Codice lobby: %s<br>È già copiato - incollalo al tuo amico in chat.",
		"Copia prima il codice lobby del tuo amico (Ctrl+C), poi scegli Entra con codice.",
		"CREA LOBBY - ospiti tu e ricevi un codice per il tuo amico.<br>ENTRA CON CODICE - copia prima il codice del tuo amico (Ctrl+C).",
		"Questa finestra non si aggiorna da sola: chiudila e riapri Multigiocatore tra qualche secondo.",
		"Il tuo compagno si sta ancora collegando. Potrai iniziare quando sarà qui.",
		"Impossibile creare la lobby: nessuna connessione a Steam. Controlla che Steam sia online e che una VPN non lo blocchi.",
		"Impossibile entrare nella lobby: è chiusa, piena o il codice è sbagliato.",
		"INIZIA", "F6 - esci dalla lobby.", "Negli appunti: %s", "Negli appunti non c'è un codice lobby.",
		"RAGGIUNGI IL COMPAGNO" },
	{ "SALIR A LA BIBLIOTECA", "MULTIJUGADOR",
		"Crea una sala e invita a un amigo de Steam para jugar la historia juntos.<br>Tu amigo también necesita este mod.",
		"Tu sala está abierta.<br>Invita a un amigo: se abre la ventana de Steam, elige allí a un amigo.<br>Cuando acepte, os conectaréis automáticamente.",
		"Estás en la sala de %s.<br>Cierra esta ventana (Esc) y espera: cuando el anfitrión inicie la partida, el menú de la historia se abrirá solo. Elige Continuar y te llevará con el anfitrión.",
		"Conectado con %s.<br>En el juego: F7 - ir con tu compañero, F5 - cargar su capítulo.",
		"Conectando...", "CREAR SALA", "INVITAR A UN AMIGO", "SALIR DE LA SALA", "SALA",
		"CAMBIAR PERSONAJE", "Tú", "protagonista", "compañero",
		"UNIRSE CON CÓDIGO",
		"Código de sala: %s<br>Ya está copiado: pégaselo a tu amigo en un chat.",
		"Copia primero el código de sala de tu amigo (Ctrl+C) y elige Unirse con código.",
		"CREAR SALA - eres el anfitrión y recibes un código para tu amigo.<br>UNIRSE CON CÓDIGO - copia primero el código de tu amigo (Ctrl+C).",
		"Esta ventana no se actualiza sola: ciérrala y vuelve a abrir Multijugador en unos segundos.",
		"Tu compañero todavía se está conectando. Podrás empezar cuando esté aquí.",
		"No se pudo crear la sala: sin conexión con Steam. Comprueba que Steam esté en línea y que una VPN no lo bloquee.",
		"No se pudo entrar en la sala: está cerrada, llena o el código es incorrecto.",
		"EMPEZAR", "F6 - salir de la sala.", "En el portapapeles: %s", "No hay ningún código de sala en el portapapeles.",
		"IR CON EL COMPAÑERO" },
	{ "SAIR PARA A BIBLIOTECA", "MULTIJOGADOR",
		"Crie um lobby e convide um amigo da Steam para jogar a história juntos.<br>Seu amigo também precisa deste mod.",
		"Seu lobby está aberto.<br>Convide um amigo: a janela da Steam abre, escolha um amigo nela.<br>Quando ele aceitar, vocês se conectam automaticamente.",
		"Você está no lobby de %s.<br>Feche esta janela (Esc) e aguarde: quando o anfitrião iniciar o jogo, o menu da história abre sozinho. Escolha Continuar e você irá até o anfitrião.",
		"Conectado a %s.<br>No jogo: F7 - ir até o parceiro, F5 - carregar o capítulo dele.",
		"Conectando...", "CRIAR LOBBY", "CONVIDAR AMIGO", "SAIR DO LOBBY", "LOBBY",
		"TROCAR PERSONAGEM", "Você", "protagonista", "parceiro",
		"ENTRAR COM CÓDIGO",
		"Código do lobby: %s<br>Ele já foi copiado - cole para o seu amigo no chat.",
		"Copie primeiro o código do lobby do seu amigo (Ctrl+C) e escolha Entrar com código.",
		"CRIAR LOBBY - você é o anfitrião e recebe um código para o amigo.<br>ENTRAR COM CÓDIGO - copie primeiro o código do amigo (Ctrl+C).",
		"Esta janela não se atualiza sozinha: feche-a e abra Multijogador de novo em alguns segundos.",
		"Seu parceiro ainda está se conectando. Você pode começar quando ele chegar.",
		"Não foi possível criar o lobby: sem conexão com a Steam. Verifique se a Steam está online e se uma VPN não a bloqueia.",
		"Não foi possível entrar no lobby: está fechado, cheio ou o código está errado.",
		"COMEÇAR", "F6 - sair do lobby.", "Na área de transferência: %s", "Não há código de lobby na área de transferência.",
		"IR ATÉ O PARCEIRO" },
	{ "WYJDŹ DO BIBLIOTEKI", "TRYB WIELOOSOBOWY",
		"Utwórz lobby i zaproś znajomego ze Steam, aby przejść fabułę razem.<br>Znajomy też musi mieć ten mod.",
		"Twoje lobby jest otwarte.<br>Zaproś znajomego: otworzy się okno Steam, wybierz w nim znajomego.<br>Gdy przyjmie zaproszenie, połączycie się automatycznie.",
		"Jesteś w lobby gracza %s.<br>Zamknij to okno (Esc) i czekaj: gdy gospodarz rozpocznie grę, menu fabuły otworzy się samo. Wybierz Kontynuuj, a trafisz do gospodarza.",
		"Połączono z: %s.<br>W grze: F7 - do partnera, F5 - wczytaj rozdział partnera.",
		"Łączenie...", "UTWÓRZ LOBBY", "ZAPROŚ ZNAJOMEGO", "OPUŚĆ LOBBY", "LOBBY",
		"ZMIEŃ POSTAĆ", "Ty", "bohater", "towarzysz",
		"DOŁĄCZ KODEM",
		"Kod lobby: %s<br>Jest już skopiowany - wklej go znajomemu na czacie.",
		"Najpierw skopiuj kod lobby znajomego (Ctrl+C), potem wybierz Dołącz kodem.",
		"UTWÓRZ LOBBY - jesteś hostem i dostajesz kod dla znajomego.<br>DOŁĄCZ KODEM - najpierw skopiuj kod znajomego (Ctrl+C).",
		"To okno samo się nie odświeża: zamknij je i otwórz Tryb wieloosobowy ponownie za kilka sekund.",
		"Partner wciąż się łączy. Grę możesz zacząć, gdy dołączy.",
		"Nie udało się utworzyć lobby: brak połączenia ze Steam. Sprawdź, czy Steam jest online i czy VPN go nie blokuje.",
		"Nie udało się dołączyć: lobby jest zamknięte, pełne lub kod jest błędny.",
		"ROZPOCZNIJ GRĘ", "F6 - opuść lobby.", "W schowku: %s", "W schowku nie ma kodu lobby.",
		"DO PARTNERA" },
	{ "KÜTÜPHANEYE ÇIK", "ÇOK OYUNCULU",
		"Bir lobi kur ve hikâyeyi birlikte oynamak için bir Steam arkadaşını davet et.<br>Arkadaşında da bu mod olmalı.",
		"Lobin açık.<br>Bir arkadaşını davet et: Steam penceresi açılır, oradan bir arkadaş seç.<br>Kabul edince otomatik olarak bağlanırsınız.",
		"%s adlı oyuncunun lobisindesin.<br>Bu pencereyi kapat (Esc) ve bekle: kurucu oyunu başlattığında hikâye menüsü kendiliğinden açılır. Devam Et'i seç, kurucunun yanına götürüleceksin.",
		"%s ile bağlanıldı.<br>Oyunda: F7 - ortağına git, F5 - ortağının bölümünü yükle.",
		"Bağlanılıyor...", "LOBİ KUR", "ARKADAŞ DAVET ET", "LOBİDEN ÇIK", "LOBİ",
		"KARAKTERİ DEĞİŞ", "Sen", "kahraman", "yoldaş",
		"KODLA KATIL",
		"Lobi kodu: %s<br>Kopyalandı - sohbette arkadaşına yapıştır.",
		"Önce arkadaşının lobi kodunu kopyala (Ctrl+C), sonra Kodla katıl'ı seç.",
		"LOBİ KUR - sen ev sahibisin ve arkadaşın için bir kod alırsın.<br>KODLA KATIL - önce arkadaşının kodunu kopyala (Ctrl+C).",
		"Bu pencere kendiliğinden güncellenmez: kapat ve birkaç saniye sonra Çok Oyunculu'yu yeniden aç.",
		"Ortağın hâlâ bağlanıyor. O gelince oyunu başlatabilirsin.",
		"Lobi kurulamadı: Steam bağlantısı yok. Steam'in çevrimiçi olduğunu ve bir VPN'in engellemediğini kontrol et.",
		"Lobiye katılınamadı: kapalı, dolu ya da kod yanlış.",
		"OYUNU BAŞLAT", "F6 - lobiden çık.", "Panoda: %s", "Panoda lobi kodu yok.",
		"ORTAĞA GİT" },
	{ "ODEJÍT DO KNIHOVNY", "HRA VÍCE HRÁČŮ",
		"Vytvořte lobby a pozvěte přítele ze Steamu, abyste hráli příběh spolu.<br>Přítel musí mít tento mód také.",
		"Vaše lobby je otevřené.<br>Pozvěte přítele: otevře se okno Steamu, vyberte v něm přítele.<br>Jakmile pozvánku přijme, spojíte se automaticky.",
		"Jste v lobby hráče %s.<br>Zavřete toto okno (Esc) a čekejte: až hostitel spustí hru, nabídka příběhu se otevře sama. Zvolte Pokračovat a přenese vás to k hostiteli.",
		"Připojeno k: %s.<br>Ve hře: F7 - k parťákovi, F5 - načíst jeho kapitolu.",
		"Připojování...", "VYTVOŘIT LOBBY", "POZVAT PŘÍTELE", "OPUSTIT LOBBY", "LOBBY",
		"ZMĚNIT POSTAVU", "Vy", "hrdina", "parťák",
		"PŘIPOJIT SE KÓDEM",
		"Kód lobby: %s<br>Je zkopírovaný - vložte ho příteli do chatu.",
		"Nejdřív zkopírujte kód lobby přítele (Ctrl+C) a pak zvolte Připojit se kódem.",
		"VYTVOŘIT LOBBY - jste hostitel a dostanete kód pro přítele.<br>PŘIPOJIT SE KÓDEM - nejdřív zkopírujte kód přítele (Ctrl+C).",
		"Toto okno se samo neaktualizuje: zavřete ho a za pár sekund znovu otevřete Hru více hráčů.",
		"Parťák se ještě připojuje. Hru můžete začít, až bude připojený.",
		"Lobby se nepodařilo vytvořit: žádné spojení se Steamem. Zkontrolujte, že je Steam online a VPN ho neblokuje.",
		"Do lobby se nepodařilo připojit: je zavřené, plné nebo je kód špatný.",
		"ZAČÍT HRU", "F6 - opustit lobby.", "Ve schránce: %s", "Ve schránce není kód lobby.",
		"K PARŤÁKOVI" },
	{ "KILÉPÉS A KÖNYVTÁRBA", "TÖBBJÁTÉKOS",
		"Hozz létre egy lobbit, és hívj meg egy Steam-barátot, hogy együtt játsszátok végig a történetet.<br>A barátodnak is kell ez a mod.",
		"A lobbid nyitva van.<br>Hívj meg egy barátot: megnyílik a Steam ablaka, ott válassz barátot.<br>Ha elfogadja, automatikusan csatlakoztok.",
		"%s lobbijában vagy.<br>Zárd be ezt az ablakot (Esc), és várj: amikor a házigazda elindítja a játékot, a történet menü magától megnyílik. Válaszd a Folytatást, és a házigazdához kerülsz.",
		"Csatlakozva: %s.<br>Játékban: F7 - a társadhoz, F5 - a társad fejezetének betöltése.",
		"Csatlakozás...", "LOBBI LÉTREHOZÁSA", "BARÁT MEGHÍVÁSA", "KILÉPÉS A LOBBIBÓL", "LOBBI",
		"KARAKTERVÁLTÁS", "Te", "főhős", "társ",
		"BELÉPÉS KÓDDAL",
		"Lobbikód: %s<br>Már ki van másolva - illeszd be a barátodnak a csetbe.",
		"Először másold ki a barátod lobbikódját (Ctrl+C), majd válaszd a Belépés kóddal lehetőséget.",
		"LOBBI LÉTREHOZÁSA - te vagy a házigazda, és kódot kapsz a barátodnak.<br>BELÉPÉS KÓDDAL - előbb másold ki a barátod kódját (Ctrl+C).",
		"Ez az ablak magától nem frissül: zárd be, és pár másodperc múlva nyisd meg újra a Többjátékos menüt.",
		"A társad még csatlakozik. Ha megérkezett, elindíthatod a játékot.",
		"Nem sikerült létrehozni a lobbit: nincs kapcsolat a Steammel. Ellenőrizd, hogy a Steam online van, és VPN nem blokkolja.",
		"Nem sikerült belépni a lobbiba: zárva van, megtelt vagy rossz a kód.",
		"JÁTÉK INDÍTÁSA", "F6 - kilépés a lobbiból.", "A vágólapon: %s", "A vágólapon nincs lobbikód.",
		"A TÁRSADHOZ" },
	{ "ΕΞΟΔΟΣ ΣΤΗ ΒΙΒΛΙΟΘΗΚΗ", "ΠΟΛΛΟΙ ΠΑΙΚΤΕΣ",
		"Δημιούργησε ένα λόμπι και προσκάλεσε έναν φίλο από το Steam για να παίξετε μαζί την ιστορία.<br>Ο φίλος σου χρειάζεται κι αυτός το mod.",
		"Το λόμπι σου είναι ανοιχτό.<br>Προσκάλεσε έναν φίλο: ανοίγει το παράθυρο του Steam, διάλεξε εκεί φίλο.<br>Μόλις αποδεχτεί, συνδέεστε αυτόματα.",
		"Είσαι στο λόμπι του/της %s.<br>Κλείσε αυτό το παράθυρο (Esc) και περίμενε: όταν ο οικοδεσπότης ξεκινήσει το παιχνίδι, το μενού ιστορίας ανοίγει μόνο του. Διάλεξε Συνέχεια και θα μεταφερθείς δίπλα του.",
		"Συνδέθηκες με: %s.<br>Στο παιχνίδι: F7 - στον συμπαίκτη, F5 - φόρτωση του κεφαλαίου του.",
		"Σύνδεση...", "ΔΗΜΙΟΥΡΓΙΑ ΛΟΜΠΙ", "ΠΡΟΣΚΛΗΣΗ ΦΙΛΟΥ", "ΑΠΟΧΩΡΗΣΗ", "ΛΟΜΠΙ",
		"ΑΛΛΑΓΗ ΧΑΡΑΚΤΗΡΑ", "Εσύ", "ήρωας", "σύντροφος",
		"ΣΥΝΔΕΣΗ ΜΕ ΚΩΔΙΚΟ",
		"Κωδικός λόμπι: %s<br>Έχει αντιγραφεί - στείλ' τον στον φίλο σου στη συνομιλία.",
		"Αντίγραψε πρώτα τον κωδικό λόμπι του φίλου σου (Ctrl+C) και μετά διάλεξε Σύνδεση με κωδικό.",
		"ΔΗΜΙΟΥΡΓΙΑ ΛΟΜΠΙ - είσαι ο οικοδεσπότης και παίρνεις κωδικό για τον φίλο σου.<br>ΣΥΝΔΕΣΗ ΜΕ ΚΩΔΙΚΟ - αντίγραψε πρώτα τον κωδικό του φίλου σου (Ctrl+C).",
		"Αυτό το παράθυρο δεν ενημερώνεται μόνο του: κλείσ' το και άνοιξε ξανά το Πολλοί παίκτες σε λίγα δευτερόλεπτα.",
		"Ο συμπαίκτης σου συνδέεται ακόμα. Μπορείς να ξεκινήσεις μόλις έρθει.",
		"Δεν δημιουργήθηκε το λόμπι: δεν υπάρχει σύνδεση με το Steam. Έλεγξε ότι το Steam είναι online και ότι δεν το μπλοκάρει VPN.",
		"Δεν ήταν δυνατή η σύνδεση στο λόμπι: είναι κλειστό, γεμάτο ή ο κωδικός είναι λάθος.",
		"ΕΝΑΡΞΗ", "F6 - αποχώρηση από το λόμπι.", "Στο πρόχειρο: %s", "Δεν υπάρχει κωδικός λόμπι στο πρόχειρο.",
		"ΣΤΟΝ ΣΥΜΠΑΙΚΤΗ" },
	{ "STOPPEN EN NAAR BIBLIOTHEEK", "MULTIPLAYER",
		"Maak een lobby en nodig een Steam-vriend uit om het verhaal samen te spelen.<br>Je vriend heeft deze mod ook nodig.",
		"Je lobby is open.<br>Nodig een vriend uit: het Steam-venster opent, kies daar een vriend.<br>Zodra die accepteert, maken jullie automatisch verbinding.",
		"Je zit in de lobby van %s.<br>Sluit dit venster (Esc) en wacht: als de host het spel start, opent het verhaalmenu vanzelf. Kies Doorgaan en je komt bij de host.",
		"Verbonden met %s.<br>In het spel: F7 - naar je partner, F5 - hoofdstuk van je partner laden.",
		"Verbinden...", "LOBBY MAKEN", "VRIEND UITNODIGEN", "LOBBY VERLATEN", "LOBBY",
		"PERSONAGE WISSELEN", "Jij", "held", "metgezel",
		"MET CODE DEELNEMEN",
		"Lobbycode: %s<br>Hij is gekopieerd - plak hem in een chat naar je vriend.",
		"Kopieer eerst de lobbycode van je vriend (Ctrl+C) en kies dan Met code deelnemen.",
		"LOBBY MAKEN - jij bent host en krijgt een code voor je vriend.<br>MET CODE DEELNEMEN - kopieer eerst de code van je vriend (Ctrl+C).",
		"Dit venster ververst niet vanzelf: sluit het en open Multiplayer over een paar seconden opnieuw.",
		"Je partner maakt nog verbinding. Je kunt beginnen zodra hij er is.",
		"Lobby maken mislukt: geen verbinding met Steam. Controleer of Steam online is en of een VPN het niet blokkeert.",
		"Deelnemen mislukt: de lobby is gesloten, vol of de code klopt niet.",
		"SPEL STARTEN", "F6 - lobby verlaten.", "Op het klembord: %s", "Er staat geen lobbycode op het klembord.",
		"NAAR PARTNER" },
	{ "AFSLUT TIL BIBLIOTEK", "MULTIPLAYER",
		"Opret en lobby, og inviter en Steam-ven til at spille historien sammen.<br>Din ven skal også have dette mod.",
		"Din lobby er åben.<br>Inviter en ven: Steam-vinduet åbner, vælg en ven der.<br>Når vennen accepterer, forbindes I automatisk.",
		"Du er i %s's lobby.<br>Luk dette vindue (Esc) og vent: når værten starter spillet, åbner historiemenuen af sig selv. Vælg Fortsæt, så kommer du hen til værten.",
		"Forbundet til %s.<br>I spillet: F7 - gå til din makker, F5 - indlæs din makkers kapitel.",
		"Forbinder...", "OPRET LOBBY", "INVITER VEN", "FORLAD LOBBY", "LOBBY",
		"SKIFT FIGUR", "Du", "helt", "makker",
		"DELTAG MED KODE",
		"Lobbykode: %s<br>Den er kopieret - indsæt den til din ven i en chat.",
		"Kopiér først din vens lobbykode (Ctrl+C), og vælg så Deltag med kode.",
		"OPRET LOBBY - du er vært og får en kode til din ven.<br>DELTAG MED KODE - kopiér først din vens kode (Ctrl+C).",
		"Dette vindue opdateres ikke af sig selv: luk det, og åbn Multiplayer igen om et par sekunder.",
		"Din makker forbinder stadig. Du kan starte spillet, når han er her.",
		"Lobbyen kunne ikke oprettes: ingen forbindelse til Steam. Tjek, at Steam er online, og at en VPN ikke blokerer.",
		"Kunne ikke deltage: lobbyen er lukket, fuld eller koden er forkert.",
		"START SPIL", "F6 - forlad lobbyen.", "I udklipsholderen: %s", "Der er ingen lobbykode i udklipsholderen.",
		"TIL MAKKEREN" },
	{ "POISTU KIRJASTOON", "MONINPELI",
		"Luo aula ja kutsu Steam-kaveri pelaamaan tarina yhdessä.<br>Kaverillasi pitää olla myös tämä modi.",
		"Aulasi on auki.<br>Kutsu kaveri: Steam-ikkuna aukeaa, valitse sieltä kaveri.<br>Kun hän hyväksyy, yhteys muodostuu automaattisesti.",
		"Olet pelaajan %s aulassa.<br>Sulje tämä ikkuna (Esc) ja odota: kun isäntä aloittaa pelin, tarinavalikko aukeaa itsestään. Valitse Jatka, niin pääset isännän luo.",
		"Yhdistetty: %s.<br>Pelissä: F7 - pelikaverin luo, F5 - lataa pelikaverin luku.",
		"Yhdistetään...", "LUO AULA", "KUTSU KAVERI", "POISTU AULASTA", "AULA",
		"VAIHDA HAHMOA", "Sinä", "sankari", "seuralainen",
		"LIITY KOODILLA",
		"Aulan koodi: %s<br>Se on kopioitu - liitä se kaverillesi chattiin.",
		"Kopioi ensin kaverisi aulan koodi (Ctrl+C) ja valitse sitten Liity koodilla.",
		"LUO AULA - olet isäntä ja saat koodin kaverillesi.<br>LIITY KOODILLA - kopioi ensin kaverisi koodi (Ctrl+C).",
		"Tämä ikkuna ei päivity itsestään: sulje se ja avaa Moninpeli uudelleen muutaman sekunnin päästä.",
		"Pelikaverisi yhdistää vielä. Voit aloittaa pelin, kun hän on paikalla.",
		"Aulaa ei voitu luoda: ei yhteyttä Steamiin. Tarkista, että Steam on verkossa eikä VPN estä sitä.",
		"Aulaan ei päästy: se on suljettu, täynnä tai koodi on väärä.",
		"ALOITA PELI", "F6 - poistu aulasta.", "Leikepöydällä: %s", "Leikepöydällä ei ole aulan koodia.",
		"PELIKAVERIN LUO" },
	{ "GÅ TIL BIBLIOTEKET", "FLERSPILLER",
		"Opprett en lobby og inviter en Steam-venn til å spille historien sammen.<br>Vennen din trenger også denne moden.",
		"Lobbyen din er åpen.<br>Inviter en venn: Steam-vinduet åpnes, velg en venn der.<br>Når vennen godtar, kobles dere til automatisk.",
		"Du er i lobbyen til %s.<br>Lukk dette vinduet (Esc) og vent: når verten starter spillet, åpnes historiemenyen av seg selv. Velg Fortsett, så kommer du til verten.",
		"Koblet til %s.<br>I spillet: F7 - gå til partneren, F5 - last inn partnerens kapittel.",
		"Kobler til...", "OPPRETT LOBBY", "INVITER VENN", "FORLAT LOBBYEN", "LOBBY",
		"BYTT KARAKTER", "Du", "helt", "følgesvenn",
		"BLI MED MED KODE",
		"Lobbykode: %s<br>Den er kopiert - lim den inn til vennen din i en chat.",
		"Kopier først lobbykoden til vennen din (Ctrl+C), og velg så Bli med med kode.",
		"OPPRETT LOBBY - du er vert og får en kode til vennen din.<br>BLI MED MED KODE - kopier først koden til vennen din (Ctrl+C).",
		"Dette vinduet oppdateres ikke av seg selv: lukk det og åpne Flerspiller igjen om noen sekunder.",
		"Partneren din kobler fortsatt til. Du kan starte når han er her.",
		"Kunne ikke opprette lobbyen: ingen forbindelse til Steam. Sjekk at Steam er pålogget og at en VPN ikke blokkerer.",
		"Kunne ikke bli med: lobbyen er stengt, full eller koden er feil.",
		"START SPILL", "F6 - forlat lobbyen.", "På utklippstavlen: %s", "Det er ingen lobbykode på utklippstavlen.",
		"TIL PARTNEREN" },
	{ "AVSLUTA TILL BIBLIOTEKET", "FLERSPELARLÄGE",
		"Skapa en lobby och bjud in en Steam-vän för att spela berättelsen tillsammans.<br>Din vän behöver också den här modden.",
		"Din lobby är öppen.<br>Bjud in en vän: Steam-fönstret öppnas, välj en vän där.<br>När vännen tackar ja ansluter ni automatiskt.",
		"Du är i %s lobby.<br>Stäng det här fönstret (Esc) och vänta: när värden startar spelet öppnas berättelsemenyn av sig själv. Välj Fortsätt så hamnar du hos värden.",
		"Ansluten till %s.<br>I spelet: F7 - gå till din partner, F5 - ladda din partners kapitel.",
		"Ansluter...", "SKAPA LOBBY", "BJUD IN VÄN", "LÄMNA LOBBYN", "LOBBY",
		"BYT KARAKTÄR", "Du", "hjälte", "följeslagare",
		"GÅ MED MED KOD",
		"Lobbykod: %s<br>Den är kopierad - klistra in den till din vän i en chatt.",
		"Kopiera först din väns lobbykod (Ctrl+C) och välj sedan Gå med med kod.",
		"SKAPA LOBBY - du är värd och får en kod till din vän.<br>GÅ MED MED KOD - kopiera först din väns kod (Ctrl+C).",
		"Det här fönstret uppdateras inte av sig självt: stäng det och öppna Flerspelarläge igen om några sekunder.",
		"Din partner ansluter fortfarande. Du kan starta spelet när den är här.",
		"Det gick inte att skapa lobbyn: ingen anslutning till Steam. Kontrollera att Steam är online och att en VPN inte blockerar.",
		"Det gick inte att gå med: lobbyn är stängd, full eller koden är fel.",
		"STARTA SPEL", "F6 - lämna lobbyn.", "I urklipp: %s", "Det finns ingen lobbykod i urklipp.",
		"TILL PARTNERN" },
	{ "VRATI SE U LIBRARY", "VIŠE IGRAČA",
		"Napravi predvorje i pozovi prijatelja sa Steama da zajedno igrate priču.<br>Prijatelj također treba imati ovaj mod.",
		"Tvoje predvorje je otvoreno.<br>Pozovi prijatelja: otvorit će se prozor Steama, ondje odaberi prijatelja.<br>Kad prihvati, povezat ćete se automatski.",
		"Nalaziš se u predvorju igrača %s.<br>Zatvori ovaj prozor (Esc) i pričekaj: kad domaćin pokrene igru, izbornik priče otvorit će se sam. Odaberi Nastavi i bit ćeš prebačen do domaćina.",
		"Povezano s: %s.<br>U igri: F7 - do partnera, F5 - učitaj partnerovo poglavlje.",
		"Povezivanje...", "NAPRAVI PREDVORJE", "POZOVI PRIJATELJA", "NAPUSTI PREDVORJE", "PREDVORJE",
		"PROMIJENI LIK", "Ti", "junak", "pratilac",
		"PRIDRUŽI SE KODOM",
		"Kod predvorja: %s<br>Već je kopiran - zalijepi ga prijatelju u chat.",
		"Najprije kopiraj prijateljev kod predvorja (Ctrl+C), a zatim odaberi Pridruži se kodom.",
		"NAPRAVI PREDVORJE - ti si domaćin i dobivaš kod za prijatelja.<br>PRIDRUŽI SE KODOM - najprije kopiraj prijateljev kod (Ctrl+C).",
		"Ovaj se prozor ne osvježava sam: zatvori ga i za nekoliko sekundi ponovno otvori Više igrača.",
		"Partner se još povezuje. Igru možeš pokrenuti kad stigne.",
		"Predvorje nije stvoreno: nema veze sa Steamom. Provjeri je li Steam na mreži i ne blokira li ga VPN.",
		"Ulazak nije uspio: predvorje je zatvoreno, puno ili je kod pogrešan.",
		"ZAPOČNI IGRU", "F6 - napusti predvorje.", "U međuspremniku: %s", "U međuspremniku nema koda predvorja.",
		"DO PARTNERA" },
	{ "ライブラリに戻る", "マルチプレイ",
		"ロビーを作成してSteamのフレンドを招待し、一緒にストーリーを遊びましょう。<br>フレンドにもこのMODが必要です。",
		"ロビーを作成しました。<br>フレンドを招待: Steamのウィンドウが開くので、そこでフレンドを選んでください。<br>招待が承諾されると自動で接続します。",
		"%s のロビーに参加しました。<br>この画面を閉じて（Esc）お待ちください。ホストがゲームを開始するとストーリーメニューが自動で開きます。「つづける」を選ぶとホストの所へ移動します。",
		"%s と接続しました。<br>ゲーム中: F7 - パートナーの所へ、F5 - パートナーのチャプターをロード。",
		"接続中...", "ロビーを作成", "フレンドを招待", "ロビーを退出", "ロビー",
		"キャラクター変更", "あなた", "主人公", "相棒",
		"コードで参加",
		"ロビーコード: %s<br>コピー済みです。チャットでフレンドに貼り付けてください。",
		"先にフレンドのロビーコードをコピー(Ctrl+C)してから「コードで参加」を選んでください。",
		"ロビーを作成 - あなたがホストになり、フレンド用のコードを受け取ります。<br>コードで参加 - 先にフレンドのコードをコピー(Ctrl+C)してください。",
		"このウィンドウは自動で更新されません。閉じて、数秒後にもう一度「マルチプレイ」を開いてください。",
		"パートナーはまだ接続中です。接続されたらゲームを開始できます。",
		"ロビーを作成できませんでした: Steamに接続できません。Steamがオンラインか、VPNがブロックしていないか確認してください。",
		"ロビーに参加できませんでした: 閉じている、満員、またはコードが違います。",
		"ゲーム開始", "F6 - ロビーを退出", "クリップボード: %s", "クリップボードにロビーコードがありません。",
		"パートナーの所へ" },
	{ "라이브러리로 나가기", "멀티플레이",
		"로비를 만들고 Steam 친구를 초대해 함께 스토리를 플레이하세요.<br>친구도 이 모드를 설치해야 합니다.",
		"로비가 열렸습니다.<br>친구 초대: Steam 창이 열리면 그곳에서 친구를 고르세요.<br>친구가 수락하면 자동으로 연결됩니다.",
		"%s 님의 로비에 참가했습니다.<br>이 창을 닫고(Esc) 기다리세요. 호스트가 게임을 시작하면 스토리 메뉴가 자동으로 열립니다. 이어하기를 선택하면 호스트에게 이동합니다.",
		"%s 님과 연결되었습니다.<br>게임 중: F7 - 파트너에게 이동, F5 - 파트너의 챕터 불러오기.",
		"연결 중...", "로비 만들기", "친구 초대", "로비 나가기", "로비",
		"캐릭터 변경", "나", "주인공", "동료",
		"코드로 참가",
		"로비 코드: %s<br>복사되었습니다. 채팅으로 친구에게 붙여 넣으세요.",
		"먼저 친구의 로비 코드를 복사(Ctrl+C)한 뒤 코드로 참가를 선택하세요.",
		"로비 만들기 - 내가 호스트가 되고 친구에게 줄 코드를 받습니다.<br>코드로 참가 - 먼저 친구의 코드를 복사(Ctrl+C)하세요.",
		"이 창은 자동으로 갱신되지 않습니다. 창을 닫고 몇 초 뒤에 멀티플레이를 다시 여세요.",
		"파트너가 아직 연결 중입니다. 연결되면 게임을 시작할 수 있습니다.",
		"로비를 만들 수 없습니다: Steam에 연결되지 않았습니다. Steam이 온라인인지, VPN이 막고 있지 않은지 확인하세요.",
		"로비에 참가할 수 없습니다: 닫혔거나 가득 찼거나 코드가 틀렸습니다.",
		"게임 시작", "F6 - 로비 나가기", "클립보드: %s", "클립보드에 로비 코드가 없습니다.",
		"파트너에게 이동" },
	{ "退出至游戏库", "多人游戏",
		"创建大厅并邀请一位 Steam 好友，一起体验剧情。<br>你的好友也需要安装此模组。",
		"大厅已创建。<br>邀请好友：Steam 窗口会打开，请在其中选择好友。<br>好友接受后会自动连接。",
		"你已加入 %s 的大厅。<br>关闭此窗口（Esc）并等待：房主开始游戏后，剧情菜单会自动打开。选择“继续”，即可传送到房主身边。",
		"已连接到 %s。<br>游戏中：F7 - 前往同伴处，F5 - 载入同伴的章节。",
		"正在连接...", "创建大厅", "邀请好友", "离开大厅", "大厅",
		"更换角色", "你", "主角", "同伴",
		"用代码加入",
		"大厅代码：%s<br>已复制，请在聊天中粘贴给好友。",
		"请先复制好友的大厅代码（Ctrl+C），再选择“用代码加入”。",
		"创建大厅 - 你是主机，并获得给好友的代码。<br>用代码加入 - 请先复制好友的代码（Ctrl+C）。",
		"此窗口不会自动更新：请关闭它，几秒后再打开“多人游戏”。",
		"同伴仍在连接中。连接后才能开始游戏。",
		"无法创建大厅：无法连接 Steam。请确认 Steam 在线，且未被 VPN 阻止。",
		"无法加入大厅：大厅已关闭、已满或代码错误。",
		"开始游戏", "F6 - 离开大厅", "剪贴板中：%s", "剪贴板中没有大厅代码。",
		"前往同伴处" },
	{ "返回收藏庫", "多人遊戲",
		"建立大廳並邀請一位 Steam 好友，一起體驗劇情。<br>你的好友也需要安裝此模組。",
		"大廳已建立。<br>邀請好友：Steam 視窗會開啟，請在其中選擇好友。<br>好友接受後會自動連線。",
		"你已加入 %s 的大廳。<br>關閉此視窗（Esc）並等待：房主開始遊戲後，劇情選單會自動開啟。選擇「繼續」，即可傳送到房主身邊。",
		"已連線至 %s。<br>遊戲中：F7 - 前往夥伴處，F5 - 載入夥伴的章節。",
		"正在連線...", "建立大廳", "邀請好友", "離開大廳", "大廳",
		"更換角色", "你", "主角", "同伴",
		"用代碼加入",
		"大廳代碼：%s<br>已複製，請在聊天中貼給好友。",
		"請先複製好友的大廳代碼（Ctrl+C），再選擇「用代碼加入」。",
		"建立大廳 - 你是主機，並取得給好友的代碼。<br>用代碼加入 - 請先複製好友的代碼（Ctrl+C）。",
		"此視窗不會自動更新：請關閉它，幾秒後再開啟「多人遊戲」。",
		"夥伴仍在連線中。連線後才能開始遊戲。",
		"無法建立大廳：無法連線至 Steam。請確認 Steam 在線，且未被 VPN 阻擋。",
		"無法加入大廳：大廳已關閉、已滿或代碼錯誤。",
		"開始遊戲", "F6 - 離開大廳", "剪貼簿中：%s", "剪貼簿中沒有大廳代碼。",
		"前往夥伴處" },
	{ "ออกไปยังคลังเกม", "ผู้เล่นหลายคน",
		"สร้างล็อบบี้แล้วเชิญเพื่อนใน Steam มาเล่นเนื้อเรื่องด้วยกัน<br>เพื่อนของคุณต้องติดตั้งม็อดนี้ด้วย",
		"ล็อบบี้ของคุณเปิดแล้ว<br>เชิญเพื่อน: หน้าต่าง Steam จะเปิดขึ้น ให้เลือกเพื่อนในนั้น<br>เมื่อเพื่อนตอบรับ จะเชื่อมต่อให้อัตโนมัติ",
		"คุณเข้าล็อบบี้ของ %s แล้ว<br>ปิดหน้าต่างนี้ (Esc) แล้วรอ: เมื่อโฮสต์เริ่มเกม เมนูเนื้อเรื่องจะเปิดเอง เลือก เล่นต่อ แล้วคุณจะถูกพาไปหาโฮสต์",
		"เชื่อมต่อกับ %s แล้ว<br>ในเกม: F7 - ไปหาคู่หู, F5 - โหลดบทของคู่หู",
		"กำลังเชื่อมต่อ...", "สร้างล็อบบี้", "เชิญเพื่อน", "ออกจากล็อบบี้", "ล็อบบี้",
		"เปลี่ยนตัวละคร", "คุณ", "ตัวเอก", "คู่หู",
		"เข้าร่วมด้วยรหัส",
		"รหัสล็อบบี้: %s<br>คัดลอกแล้ว วางให้เพื่อนในแชตได้เลย",
		"คัดลอกรหัสล็อบบี้ของเพื่อนก่อน (Ctrl+C) แล้วเลือกเข้าร่วมด้วยรหัส",
		"สร้างล็อบบี้ - คุณเป็นโฮสต์และได้รหัสไว้ให้เพื่อน<br>เข้าร่วมด้วยรหัส - คัดลอกรหัสของเพื่อนก่อน (Ctrl+C)",
		"หน้าต่างนี้ไม่อัปเดตเอง ปิดแล้วเปิดผู้เล่นหลายคนอีกครั้งในอีกไม่กี่วินาที",
		"คู่หูยังเชื่อมต่ออยู่ เริ่มเกมได้เมื่อเขาเข้ามาแล้ว",
		"สร้างล็อบบี้ไม่ได้: เชื่อมต่อ Steam ไม่ได้ ตรวจสอบว่า Steam ออนไลน์และ VPN ไม่ได้บล็อก",
		"เข้าร่วมล็อบบี้ไม่ได้: ล็อบบี้ปิด เต็ม หรือรหัสผิด",
		"เริ่มเกม", "F6 - ออกจากล็อบบี้", "ในคลิปบอร์ด: %s", "ไม่มีรหัสล็อบบี้ในคลิปบอร์ด",
		"ไปหาคู่หู" },
};

// Сообщения на экране по языкам (порядок как в kLangs), %s - ник партнёра.
const char* const kNotes[][7] = {
	{ "%s joined", "Partner disconnected", "%s died - restarting the checkpoint together", "Multiplayer: searching for your partner", "%s is READY!", "%s started - choose Continue", "Slow connection: %s" },  // en
	{ "%s присоединился", "Напарник отключился", "%s погиб - перезагружаем чекпоинт вместе", "Мультиплеер: поиск напарника", "%s ГОТОВ!", "%s начал игру - выберите «Продолжить»", "Медленная связь: %s" },  // ru
	{ "%s a rejoint la partie", "Partenaire déconnecté", "%s est mort - on recharge le point de contrôle ensemble", "Multijoueur : recherche de votre partenaire", "%s est PRÊT !", "%s a lancé la partie - choisissez Continuer", "Connexion lente : %s" },  // fr
	{ "%s ist beigetreten", "Partner getrennt", "%s ist gestorben - wir laden den Checkpoint gemeinsam neu", "Mehrspieler: Partner wird gesucht", "%s ist BEREIT!", "%s hat gestartet - wähle Fortsetzen", "Langsame Verbindung: %s" },  // de
	{ "%s si è unito", "Compagno disconnesso", "%s è morto - ricarichiamo il checkpoint insieme", "Multigiocatore: ricerca del compagno", "%s è PRONTO!", "%s ha iniziato - scegli Continua", "Connessione lenta: %s" },  // it
	{ "%s se ha unido", "Compañero desconectado", "%s ha muerto - recargamos el punto de control juntos", "Multijugador: buscando a tu compañero", "¡%s está LISTO!", "%s ha empezado - elige Continuar", "Conexión lenta: %s" },  // es
	{ "%s entrou", "Parceiro desconectado", "%s morreu - recarregando o checkpoint juntos", "Multijogador: procurando seu parceiro", "%s está PRONTO!", "%s começou - escolha Continuar", "Conexão lenta: %s" },  // pt
	{ "%s dołączył", "Partner rozłączony", "%s zginął - wczytujemy punkt kontrolny razem", "Tryb wieloosobowy: szukanie partnera", "%s jest GOTOWY!", "%s zaczął grę - wybierz Kontynuuj", "Wolne połączenie: %s" },  // pl
	{ "%s katıldı", "Ortağın bağlantısı koptu", "%s öldü - kontrol noktasını birlikte yeniden yüklüyoruz", "Çok oyunculu: ortağın aranıyor", "%s HAZIR!", "%s oyunu başlattı - Devam Et'i seç", "Yavaş bağlantı: %s" },  // tr
	{ "%s se připojil", "Parťák se odpojil", "%s zemřel - společně načítáme checkpoint", "Hra více hráčů: hledání parťáka", "%s je PŘIPRAVEN!", "%s spustil hru - zvolte Pokračovat", "Pomalé připojení: %s" },  // cs
	{ "%s csatlakozott", "A társad kilépett", "%s meghalt - együtt töltjük újra az ellenőrzőpontot", "Többjátékos: társ keresése", "%s KÉSZ!", "%s elindította a játékot - válaszd a Folytatást", "Lassú kapcsolat: %s" },  // hu
	{ "Ο/Η %s συνδέθηκε", "Ο συμπαίκτης αποσυνδέθηκε", "Ο/Η %s πέθανε - φορτώνουμε μαζί το σημείο ελέγχου", "Πολλοί παίκτες: αναζήτηση συμπαίκτη", "Ο/Η %s είναι ΕΤΟΙΜΟΣ!", "Ο/Η %s ξεκίνησε - διάλεξε Συνέχεια", "Αργή σύνδεση: %s" },  // el
	{ "%s doet mee", "Partner verbroken", "%s is dood - we laden het checkpoint samen opnieuw", "Multiplayer: partner zoeken", "%s is KLAAR!", "%s is begonnen - kies Doorgaan", "Trage verbinding: %s" },  // nl
	{ "%s er med", "Makker afbrudt", "%s døde - vi genindlæser checkpointet sammen", "Multiplayer: søger efter din makker", "%s er KLAR!", "%s er startet - vælg Fortsæt", "Langsom forbindelse: %s" },  // da
	{ "%s liittyi", "Pelikaveri katkaisi yhteyden", "%s kuoli - ladataan tallennuspiste yhdessä", "Moninpeli: etsitään pelikaveria", "%s on VALMIS!", "%s aloitti - valitse Jatka", "Hidas yhteys: %s" },  // fi
	{ "%s ble med", "Partneren koblet fra", "%s døde - vi laster inn sjekkpunktet sammen", "Flerspiller: leter etter partneren", "%s er KLAR!", "%s har startet - velg Fortsett", "Treg forbindelse: %s" },  // no
	{ "%s anslöt", "Partnern kopplade från", "%s dog - vi laddar om kontrollpunkten tillsammans", "Flerspelarläge: söker efter din partner", "%s är REDO!", "%s har startat - välj Fortsätt", "Långsam anslutning: %s" },  // sv
	{ "%s se pridružio", "Partner se odspojio", "%s je poginuo - zajedno učitavamo kontrolnu točku", "Više igrača: traženje partnera", "%s je SPREMAN!", "%s je pokrenuo igru - odaberi Nastavi", "Spora veza: %s" },  // hr
	{ "%s が参加しました", "パートナーが切断しました", "%s が死亡 - 一緒にチェックポイントから再開します", "マルチプレイ: パートナーを探しています", "%s の準備完了！", "%s が開始しました - 「つづける」を選択", "通信が遅い: %s" },  // ja
	{ "%s 님이 참가했습니다", "파트너 연결 끊김", "%s 님 사망 - 함께 체크포인트를 다시 불러옵니다", "멀티플레이: 파트너 찾는 중", "%s 님 준비 완료!", "%s 님이 시작했습니다 - 이어하기를 선택하세요", "연결 느림: %s" },  // ko
	{ "%s 已加入", "同伴已断开连接", "%s 阵亡 - 一起重新载入检查点", "多人游戏：正在寻找同伴", "%s 已准备！", "%s 已开始 - 请选择“继续”", "连接缓慢：%s" },  // zh-CN
	{ "%s 已加入", "夥伴已中斷連線", "%s 陣亡 - 一起重新載入檢查點", "多人遊戲：正在尋找夥伴", "%s 已準備！", "%s 已開始 - 請選擇「繼續」", "連線緩慢：%s" },  // zh-TW
	{ "%s เข้าร่วมแล้ว", "คู่หูตัดการเชื่อมต่อ", "%s ตาย - โหลดเช็กพอยต์ใหม่พร้อมกัน", "ผู้เล่นหลายคน: กำลังหาคู่หู", "%s พร้อมแล้ว!", "%s เริ่มเกมแล้ว - เลือก เล่นต่อ", "การเชื่อมต่อช้า: %s" },  // th
};
static_assert(sizeof(kNotes) / sizeof(kNotes[0]) == sizeof(kLangs) / sizeof(kLangs[0]));

const Lang& LangFor(const char* library) {
	if (library)
		for (const Lang& l : kLangs)
			if (!strcmp(l.library, library)) return l;
	return kLangs[0];
}

struct ReadyUi {
	const char* btnReady;
	const char* btnUnready;
	const char* btnWaiting;
	const char* readyTag;
	const char* notReadyTag;
	const char* hostTag;
	const char* guestTag;
	const char* waitNotReady;
	const char* waitHostOnly;
	const char* readyHintGuestOff;
	const char* readyHintGuestOn;
	const char* readyHintHostWait;
	const char* readyHintHostGo;
};

const ReadyUi kReadyRu = {
	"ГОТОВ К ИГРЕ",
	"НЕ ГОТОВ",
	"ЖДЁМ ДРУГА...",
	"ГОТОВ",
	"НЕ ГОТОВ",
	"хост",
	"гость",
	"Нельзя запустить игру: %s ещё не готов! Пусть друг откроет «Мультиплеер» и нажмёт «ГОТОВ К ИГРЕ».",
	"Игру запускает хост (%s). Нажмите «ГОТОВ К ИГРЕ» и дождитесь запуска — загрузит обоих сразу.",
	"Нажмите «ГОТОВ К ИГРЕ» и закройте окно (Esc). Когда хост нажмёт «Продолжить», у вас само откроется меню сюжета: выберите «Продолжить» - после загрузки вас перенесёт к хосту.",
	"Вы ГОТОВЫ! Закройте окно (Esc) и ждите: когда хост нажмёт «Продолжить», у вас само откроется меню сюжета. Выберите «Продолжить» - после загрузки вас перенесёт к хосту.",
	"Ждём готовности друга: %s должен нажать «ГОТОВ К ИГРЕ» в меню «Мультиплеер». Пока не готов — старт заблокирован.",
	"Оба готовы! Нажмите «НАЧАТЬ ИГРУ», затем «Продолжить» (или «Новая игра») — обоих закинет одновременно."
};

const ReadyUi kReadyEn = {
	"I AM READY",
	"NOT READY",
	"WAITING...",
	"READY",
	"NOT READY",
	"host",
	"guest",
	"Cannot start yet: %s is not ready! Ask your partner to open Multiplayer and press I AM READY.",
	"Only the host (%s) can start the game. Press I AM READY and wait — you will both load together.",
	"Press I AM READY and close this window (Esc). When the host presses Continue, your story menu opens by itself: choose Continue and after loading you will be taken to the host.",
	"You are READY! Close this window (Esc) and wait: when the host presses Continue, your story menu opens by itself. Choose Continue and after loading you will be taken to the host.",
	"Waiting for partner readiness: %s must press I AM READY in Multiplayer. Starting is locked until then.",
	"Both ready! Press START GAME, then Continue (or New Game) — you will both load at the same time."
};

const ReadyUi& ReadyUiFor(const Lang& l) {
	return !strcmp(l.library, kLangs[1].library) ? kReadyRu : kReadyEn;
}

// Сообщение на экране: очередь (любой поток) и показанный текст (живёт, пока его показывают).
SRWLOCK g_noteLock = SRWLOCK_INIT;
struct PendingNote { Note id; std::string name; };
std::vector<PendingNote> g_notes;
std::atomic<void*> g_textDb{ nullptr };  // база текстов игры (для языка сообщений)
char g_noteText[256] = "";

LobbyFn g_lobby = nullptr;
ActionFn g_action = nullptr;
CodeFn g_clipboardCode = nullptr;
std::string g_clipboardShown;  // код из буфера на момент открытия окна (поток меню)
safetyhook::InlineHook g_textHook, g_visibleHook;
safetyhook::MidHook g_pressHook, g_answerHook;

// Строки должны жить, пока игра их показывает: по два буфера на каждую, по очереди.
template <size_t N>
struct Text {
	char buf[2][N];
	int turn = 0;
	char* Next() { return buf[turn ^= 1]; }
};
Text<512> g_desc;
// Текст лобби и название пункта: всегда один и тот же буфер (см. LobbyText).
char g_bodyLive[2048], g_titleLive[256];

std::string Escaped(const std::string& name) {  // ник не должен стать разметкой
	std::string out;
	for (char c : name) out += c == '<' ? '(' : c == '>' ? ')' : c;
	return out;
}

// Название пункта: "MULTIPLAYER", "MULTIPLAYER · ЛОББИ", "MULTIPLAYER · ник".
const char* TitleText(const Lang& l) {
	const Lobby s = g_lobby ? g_lobby() : Lobby{};
	char next[256];
	if (!s.on && !s.lobby && !s.busy) snprintf(next, sizeof(next), "%s", l.title);
	else
		snprintf(next, sizeof(next), "%s · %s", l.title, s.connected ? Escaped(s.partner).c_str() : s.lobby ? l.tag : "...");
	memcpy(g_titleLive, next, strlen(next) + 1);
	return g_titleLive;
}

// Текст лобби и название пункта игра получает указателем. Отдаём всегда один и тот же буфер и
// раз в полсекунды переписываем его на месте (OnFrame): если игра перечитывает строку при
// отрисовке, открытое окно обновится само. Новый текст собираем рядом и копируем целиком.
const char* LobbyTextInto(const Lang& l, char* buf, size_t n);

const char* LobbyText(const Lang& l) {
	char next[sizeof(g_bodyLive)];
	LobbyTextInto(l, next, sizeof(next));
	memcpy(g_bodyLive, next, strlen(next) + 1);
	return g_bodyLive;
}

extern std::atomic<ULONGLONG> g_waitUntil;
extern std::atomic<int> g_waitReason;  // 0=connecting, 1=partner not ready, 2=guest waits host

const char* LobbyTextInto(const Lang& l, char* buf, size_t n) {
	const Lobby s = g_lobby ? g_lobby() : Lobby{};
	const ReadyUi& ru = ReadyUiFor(l);
	if (GetTickCount64() < g_waitUntil) {
		const int reason = g_waitReason.load();
		int k = 0;
		if (reason == 0 && !s.connected) k = snprintf(buf, n, "%s<br><br>", l.waitStart);
		else if (reason == 1 && s.connected && !s.partnerReady) k = snprintf(buf, n, ru.waitNotReady, Escaped(s.partner).c_str()), k = (k > 0 ? snprintf(buf + k, n - k, "<br><br>") + k : k);
		else if (reason == 2 && s.connected && !s.authority) k = snprintf(buf, n, ru.waitHostOnly, Escaped(s.partner).c_str()), k = (k > 0 ? snprintf(buf + k, n - k, "<br><br>") + k : k);
		if (k > 0 && static_cast<size_t>(k) < n) {
			buf += k;
			n -= k;
		}
	}
	if (s.connected) {
		// "Подключено: Вася... <br><br>Вы (хост): Sarah [ГОТОВ] · Вася (гость): Tess [НЕ ГОТОВ]".
		auto who = [&](const std::string& c) {
			std::string name = c == "auto" ? s.heroName : c == "companion" ? s.companionName : c;
			if (!name.empty()) name[0] = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
			if (c == "companion") return name.empty() ? std::string(l.companion) : name + " (" + l.companion + ")";
			return name.empty() ? std::string(l.hero) : name;
		};
		const char* hint = s.authority ? (s.partnerReady ? ru.readyHintHostGo : ru.readyHintHostWait)
			: (s.myReady ? ru.readyHintGuestOn : ru.readyHintGuestOff);
		int k = snprintf(buf, n, l.connected, Escaped(s.partner).c_str());
		if (s.ping >= 0 && k > 0 && static_cast<size_t>(k) < n) k += snprintf(buf + k, n - k, "<br>Ping: %d ms", s.ping);
		if (k > 0 && static_cast<size_t>(k) < n) {
			const char* myR = (s.authority || s.myReady) ? ru.readyTag : ru.notReadyTag;
			const char* prR = s.partnerReady ? ru.readyTag : ru.notReadyTag;
			snprintf(buf + k, n - k, "<br><br>%s (%s): %s [%s]  ·  %s (%s): %s [%s]<br><br>%s<br><br>%s",
				l.you, s.authority ? ru.hostTag : ru.guestTag, who(s.myCharacter).c_str(), myR,
				Escaped(s.partner).c_str(), s.authority ? ru.guestTag : ru.hostTag, who(s.partnerCharacter).c_str(), prR,
				hint, l.leaveHint);
		}
	}
	else if (s.lobby && s.host) {
		const int k = snprintf(buf, n, "%s", l.hostWait);
		if (!s.code.empty() && k > 0 && static_cast<size_t>(k) < n) {
			snprintf(buf + k, n - k, "<br><br>");
			snprintf(buf + strlen(buf), n - strlen(buf), l.codeLine, s.code.c_str());
		}
	}
	else if (s.lobby) snprintf(buf, n, l.joinWait, Escaped(s.owner).c_str());
	else if (s.busy || s.on) snprintf(buf, n, "%s", l.busy);
	else {
		// Первой строкой - что пошло не так в прошлый раз (лобби не создалось, не вошли, нет кода).
		const char* problem = s.failure == 1 ? l.createFailed : s.failure == 2 ? l.joinFailed : s.codeError ? l.noCode : nullptr;
		if (problem) snprintf(buf, n, "%s<br><br>%s<br><br>%s", problem, l.intro, l.help);
		else snprintf(buf, n, "%s<br><br>%s", l.intro, l.help);
		// Что сейчас в буфере обмена - видно, что вставится по "войти по коду".
		const size_t k = strlen(buf);
		if (g_clipboardShown.empty()) snprintf(buf + k, n - k, "<br><br>%s", l.noClipboard);
		else {
			snprintf(buf + k, n - k, "<br><br>");
			snprintf(buf + strlen(buf), n - strlen(buf), l.inClipboard, g_clipboardShown.c_str());
		}
	}
	// Окно игры показывает текст на момент открытия: во всех промежуточных состояниях - подсказка.
	if (!s.connected && ((s.lobby && s.host) || s.busy || (s.on && !s.lobby))) {
		const size_t k = strlen(buf);
		if (k + 8 < n) snprintf(buf + k, n - k, "<br><br>%s", l.refresh);
	}
	if (const size_t k = strlen(buf); s.version && k + 32 < n) snprintf(buf + k, n - k, "<br><br>tlou-together v%u", s.version);
	return buf;
}

// Описание под меню: первая строка текста лобби.
const char* DescText(const Lang& l) {
	const char* full = LobbyText(l);
	char* buf = g_desc.Next();
	const char* br = strstr(full, "<br>");
	snprintf(buf, sizeof(g_desc.buf[0]), "%.*s", static_cast<int>(br ? br - full : strlen(full)), full);
	return buf;
}

// Подписи кнопок нашего окна: игра берёт их по MENU_YES / MENU_NO, пока окно строится.
constexpr uint64_t kYesText = Sid("MENU_YES"), kNoText = Sid("MENU_NO");
std::atomic<ULONGLONG> g_labelsUntil{ 0 };
std::atomic<bool> g_boxInLobby{ false };  // какое окно открыто: в лобби (3 кнопки) или нет (2)
std::atomic<bool> g_boxConnected{ false };  // подключены: первая кнопка - старт/готов/к напарнику
std::atomic<bool> g_boxAuthority{ false };  // мы хост в момент открытия окна
std::atomic<bool> g_boxMyReady{ false };
std::atomic<bool> g_boxPartnerReady{ false };
std::atomic<bool> g_mainMenu{ true };  // окно открыто из главного меню (стек "main"), иначе из паузы
std::atomic<const Lang*> g_activeLang{ &kLangs[0] };

const char* TextLookup(void* db, uint64_t key, bool* found) {
	g_textDb.store(db, std::memory_order_relaxed);
	const bool labels = (key == kYesText || key == kNoText) && GetTickCount64() < g_labelsUntil;
	if (key == kNoteText) {
		if (found) *found = true;
		return g_noteText;
	}
	if (key == kLibraryText || key == kLobbyText || key == kLobbyText2 || key == kDescText || labels) {
		const Lang& l = LangFor(g_textHook.call<const char*>(db, kLibraryText, nullptr));
		g_activeLang = &l;
		const ReadyUi& ru = ReadyUiFor(l);
		if (found) *found = true;
		if (labels) {
			if (key == kYesText) {
				if (!g_boxConnected) return g_boxInLobby ? l.invite : l.create;
				if (!g_mainMenu) return l.goTo;
				if (!g_boxAuthority) return g_boxMyReady ? ru.btnUnready : ru.btnReady;
				return g_boxPartnerReady ? l.start : ru.btnWaiting;
			}
			return g_boxConnected ? l.swap : g_boxInLobby ? l.leave : l.joinCode;
		}
		return key == kLobbyText || key == kLobbyText2 ? LobbyText(l) : key == kDescText ? DescText(l) : TitleText(l);
	}
	return g_textHook.call<const char*>(db, key, found);
}

// Повторный показ лобби после действия (ScriptTick, основной поток). g_boxOpen - наше окно
// на экране (снимается любым ответом в нём).
std::atomic<bool> g_boxOpen{ false }, g_reopen{ false }, g_startGame{ false };
std::atomic<uintptr_t> g_menuObj{ 0 };  // объект главного меню (r15 обработчика), для "начать игру"
std::atomic<DWORD> g_mainThread{ 0 };  // основной поток (из OnFrame): меню трогаем только из него
std::atomic<ULONGLONG> g_reopenAfter{ 0 };
int g_textTurn = 0;  // какой из двух ключей текста лобби у следующего окна

void RequestReopen(ULONGLONG delayMs) {
	g_reopenAfter = GetTickCount64() + delayMs;
	g_reopen = true;
}

bool OpenLobby(bool mainMenu);

// Вызывается на каждый пункт меню каждый кадр: читаем прямо (указатель дала сама игра).
bool Visible(uintptr_t condition, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
	if (condition && *reinterpret_cast<const uint64_t*>(condition + 8) == kQuitLibrary) return false;
	return g_visibleHook.call<bool>(condition, a2, a3, a4);
}

// Окно игры с текстом лобби (вместо подтверждения выхода). mainMenu: стек "main" или "ingame".
// Вне лобби - "да/нет" (СОЗДАТЬ ЛОББИ / ОТМЕНА), в лобби - три кнопки
// (ПРИГЛАСИТЬ ДРУГА / ПОКИНУТЬ ЛОББИ / ОТМЕНА).
bool OpenLobby(bool mainMenu) {
	const Lobby s = g_lobby ? g_lobby() : Lobby{};
	const bool inLobby = s.lobby || s.busy || s.on;
	if (s.lobby && s.host && !s.code.empty()) clipboard::Set(s.code);  // код снова в буфере - вставить другу
	g_clipboardShown = !inLobby && g_clipboardCode ? g_clipboardCode() : std::string();
	g_boxInLobby = inLobby;
	g_boxConnected = s.connected;
	g_boxAuthority = s.authority;
	g_boxMyReady = s.myReady;
	g_boxPartnerReady = s.partnerReady;
	g_labelsUntil = GetTickCount64() + 1500;
	const uintptr_t base = engine::g_base;
	using StackFn = uintptr_t (*)(uintptr_t, uint64_t);
	using BoxFn = uintptr_t (*)(uintptr_t, const char*, uint64_t, uint64_t, uint64_t, int);
	const uintptr_t stack = reinterpret_cast<StackFn>(base + kPageStackRva)(0, mainMenu ? Sid("main") : Sid("ingame"));
	// Две кнопки: у окна на три кнопки "Отмена" присылала то же событие, что вторая кнопка
	// (выходила из лобби). Закрыть окно - "Назад" (Esc), подсказка внизу экрана.
	const char* page = "menu/message-box-yes-no";
	const uintptr_t box = stack ? reinterpret_cast<BoxFn>(base + kMessageBoxRva)(stack, page, (g_textTurn ^= 1) ? kLobbyText2 : kLobbyText, kLobbyBox, 0, 2) : 0;
	if (box) reinterpret_cast<void (*)(uintptr_t, uint64_t)>(base + kBoxTitleRva)(box, kLibraryText);
	if (box) {
		g_boxOpen = true;
		g_reopen = false;  // уже показано: отложенный показ после него открыл бы устаревшее окно
	}
	return box != 0;
}


// Готовность. В начале обработчика событий главного меню (0x1C831C7) и экрана кампании
// (0x1C826AA): блокируем запуск сюжета / "Продолжить" / "Новая игра", если напарник не подключён
// или не нажал "ГОТОВ К ИГРЕ", а также если пытается запустить гость. Когда оба готовы и хост
// нажимает "Продолжить" или "Новая игра", сразу шлём партнёру сигнал совместного запуска.
constexpr uintptr_t kMenuEventRva = 0x1C831C7;
constexpr uintptr_t kContinueEventRva = 0x1C826AA, kContinueDoneRva = 0x1C83147;
constexpr uint64_t kStartTlou = Sid("buttonCampaignTLOU"), kStartLeftBehind = Sid("buttonCampaignSPDLC1");
constexpr uint64_t kStartNewGame = Sid("buttonNewGame"), kStartContinue = Sid("buttonContinue");
safetyhook::MidHook g_startHook, g_continueHook;
std::atomic<ULONGLONG> g_waitUntil{ 0 };  // баннер с причиной блокировки запуска
std::atomic<int> g_waitReason{ 0 };
std::atomic<ULONGLONG> g_partnerLaunchUntil{ 0 };  // гость: хост начал игру - свой запуск сюжета разрешён

bool BlockOrLaunchStory(uint64_t btn, bool isContinueScreen) {
	if (btn != kStartTlou && btn != kStartLeftBehind && btn != kStartNewGame && btn != kStartContinue) return false;
	const Lobby s = g_lobby ? g_lobby() : Lobby{};
	if (!(s.lobby || s.busy || s.on)) return false;

	if (!s.connected) {
		g_waitReason = 0;
		g_waitUntil = GetTickCount64() + 30000;
		g_mainMenu = true;
		Log("menu: story launch blocked (partner not connected), showing readiness window=%d", OpenLobby(true) ? 1 : 0);
		return true;
	}
	if (!s.authority && GetTickCount64() < g_partnerLaunchUntil) {
		Log("menu: guest story start allowed (the host has started)");
		return false;
	}
	if (!s.authority) {
		g_waitReason = 2;
		g_waitUntil = GetTickCount64() + 30000;
		g_mainMenu = true;
		Log("menu: guest tried to start story manually, showing readiness window=%d", OpenLobby(true) ? 1 : 0);
		return true;
	}
	if (!s.partnerReady) {
		g_waitReason = 1;
		g_waitUntil = GetTickCount64() + 30000;
		g_mainMenu = true;
		Log("menu: partner not ready, story launch blocked, showing readiness window=%d", OpenLobby(true) ? 1 : 0);
		return true;
	}
	if ((isContinueScreen && btn == kStartContinue) || btn == kStartNewGame) {
		Log("menu: host pressed Continue/NewGame with partner ready -> triggering simultaneous start");
		if (g_action) g_action(HostLaunch);
	}
	return false;
}

void OnMenuEvent(safetyhook::Context& ctx) {
	if (ctx.rax == kStartTlou || ctx.rax == kStartLeftBehind) {
		if (!g_menuObj) g_menuObj = ctx.r15;
	}
	if (BlockOrLaunchStory(ctx.rax, false)) ctx.rip = engine::g_base + kPressDoneRva;
}

void OnContinueEvent(safetyhook::Context& ctx) {
	if (BlockOrLaunchStory(ctx.rax, true)) ctx.rip = engine::g_base + kContinueDoneRva;
}

void OnPress(safetyhook::Context& ctx) {
	// Ветка общая с "выйти на рабочий стол" (buttonQuitToDesktop прыгает сюда же): в rax ещё имя
	// нажатой кнопки - чужую отдаём игре.
	if (ctx.rax != Sid("buttonQuitToLibrary")) return;
	const uintptr_t menu = ctx.r15;
	using MainMenuFn = bool (*)(uintptr_t);
	const auto mainMenu = reinterpret_cast<MainMenuFn>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(menu) + kMainMenuVcall));
	g_mainMenu = mainMenu(menu);
	g_menuObj = menu;
	Log("menu: multiplayer pressed, lobby %s", OpenLobby(g_mainMenu) ? "opened" : "FAILED");
	ctx.rip = engine::g_base + kPressDoneRva;
}

// Ответ в окне: наше - обрабатываем сами, а игре подсовываем id, которого она не знает.
// Кнопки: messageBoxOk - первая, messageBoxCancel - вторая, messageBoxPop - "Назад".
void OnAnswer(safetyhook::Context& ctx) {
	if (ctx.rdi != kLobbyBox) return;
	g_boxOpen = false;  // любое событие нашего окна - оно закрылось
	const uint64_t button = *reinterpret_cast<const uint64_t*>(ctx.rsp + 0x30);
	Action a = None;
	if (button == kMessageBoxOk) {
		if (!g_boxConnected) a = g_boxInLobby ? Invite : CreateLobby;
		else if (!g_mainMenu) a = GoToPartner;
		else if (!g_boxAuthority) a = ToggleReady;
		else a = StartGame;
	} else if (button == kMessageBoxCancel) {
		a = g_boxConnected ? SwapRoles : g_boxInLobby ? Leave : JoinByCode;
	}
	if (a != None) {
		const Lobby cur = g_lobby ? g_lobby() : Lobby{};
		const bool canStart = a == StartGame && cur.connected && cur.authority && cur.partnerReady;
		Log("menu: lobby button %d", static_cast<int>(a));
		if (a == StartGame && !canStart) {
			g_waitReason = cur.connected ? 1 : 0;
			g_waitUntil = GetTickCount64() + 30000;
			RequestReopen(150);
		} else {
			if (canStart) g_startGame = true;
			if (g_action) g_action(a);
			if (a != Leave && !canStart && a != GoToPartner) RequestReopen(a == CreateLobby ? 2500 : (a == ToggleReady ? 120 : 1000));
		}
	}
	ctx.rdi = 1;
}

template <typename Hook, typename Fn>
bool Make(Hook& out, uintptr_t rva, Fn fn, const char* what) {
	auto h = Hook::create(reinterpret_cast<void*>(engine::g_base + rva), fn);
	if (h) out = std::move(*h);
	if (!h) Log("menu: %s hook FAILED", what);
	return static_cast<bool>(h);
}

bool g_installed = false;

}  // namespace

void OnFrame() {
	g_mainThread = GetCurrentThreadId();
	static ULONGLONG lastLive = 0;
	if (g_boxOpen.load(std::memory_order_relaxed) && GetTickCount64() - lastLive >= 300) {
		lastLive = GetTickCount64();
		LobbyText(*g_activeLang.load());
	}
	static bool registered = false;
	if (registered || !g_installed) return;
	registered = true;
	using AddFn = uintptr_t (*)(uintptr_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
	const uintptr_t src = reinterpret_cast<AddFn>(engine::g_base + kAddDataSourceRva)(engine::g_base + kDataSourcesRva,
		Sid("Menu2::DataSourceActionAvailableButton"), kQuitLibrary, kLibraryText, kDescText, 0);
	Log("menu: description %s", src ? "registered" : "FAILED");
}

// Отложенное открытие лобби (после "создать лобби", входа по коду, смены персонажа): здесь
// середина обновления игры на основном потоке - так открывают меню и её скрипты; из конца кадра
// (OnFrame) открытие роняло игру. Открытое окно игра не обновляет и закрыть его снаружи мы пока не
// умеем (menu2-pop-page его не закрывал), поэтому в промежуточных состояниях - подсказка.
// "Начать игру": обработчику меню (0x1C83174) - то же событие, что даёт THE LAST OF US
// (событие - структура, первым полем имя кнопки); он сам открывает меню сюжета.
constexpr uintptr_t kMenuEventsRva = 0x1C83174;

void ScriptTick() {
	// Сообщения на экране: по одному, раз в 4 с, на основном потоке (это скриптовая функция HUD).
	static ULONGLONG lastNote = 0;
	if (GetCurrentThreadId() == g_mainThread.load(std::memory_order_relaxed) && GetTickCount64() - lastNote >= 4000) {
		PendingNote note{};
		bool have = false;
		AcquireSRWLockExclusive(&g_noteLock);
		if (!g_notes.empty()) {
			note = g_notes.front();
			have = true;
			g_notes.erase(g_notes.begin());
		}
		ReleaseSRWLockExclusive(&g_noteLock);
		if (have) {
			// Язык - по строке QUIT TO LIBRARY в базе текстов игры (как у меню).
			const Lang* lang = g_activeLang.load();
			if (void* db = g_textDb.load(std::memory_order_relaxed)) lang = &LangFor(g_textHook.call<const char*>(db, kLibraryText, nullptr));
			const size_t li = static_cast<size_t>(lang - kLangs);
			const char* fmt = kNotes[li < sizeof(kNotes) / sizeof(kNotes[0]) ? li : 0][note.id];
			std::string next(255, '\0');
			next.resize(static_cast<size_t>((std::max)(0, snprintf(next.data(), next.size() + 1, fmt, Escaped(note.name).c_str()))));
			lastNote = GetTickCount64();
			snprintf(g_noteText, sizeof(g_noteText), "%s", next.c_str());
			Call(g_dc.hudMessage, { 0, kNoteText, F(4.0f), 2, Sid("none"), F(0.5f) });
			Log("note: %s", next.c_str());
		}
	}
	if (g_startGame.load(std::memory_order_relaxed) && !g_boxOpen.load(std::memory_order_relaxed) &&
		GetCurrentThreadId() == g_mainThread.load(std::memory_order_relaxed)) {
		bool want = true;
		if (const uintptr_t menu = g_menuObj; menu && g_startGame.compare_exchange_strong(want, false)) {
			alignas(16) uint64_t ev[0x40]{};
			ev[0] = Sid("buttonCampaignTLOU");
			reinterpret_cast<void (*)(uintptr_t, uint64_t*)>(engine::g_base + kMenuEventsRva)(menu, ev);
			Log("menu: start game - story menu requested");
		}
		return;
	}
	if (!g_reopen.load(std::memory_order_relaxed) || g_boxOpen.load(std::memory_order_relaxed)) return;
	if (GetCurrentThreadId() != g_mainThread.load(std::memory_order_relaxed)) return;
	bool want = true;
	if (GetTickCount64() >= g_reopenAfter && g_reopen.compare_exchange_strong(want, false))
		Log("menu: lobby reopened %s", OpenLobby(g_mainMenu) ? "ok" : "FAILED");
}

bool StartGameFromPartner() {
	g_partnerLaunchUntil = GetTickCount64() + 180000;  // 3 мин: открыть меню сюжета и нажать "Продолжить"
	if (!g_menuObj || !g_mainMenu) return false;
	g_reopen = false;
	g_startGame = true;
	return true;
}

void ShowLobbyAgain() {
	if (g_mainMenu) RequestReopen(300);
}

void Notify(Note id, const std::string& name) {
	AcquireSRWLockExclusive(&g_noteLock);
	if (g_notes.size() < 8) g_notes.push_back({ id, name });
	ReleaseSRWLockExclusive(&g_noteLock);
}

bool Install(LobbyFn lobby, ActionFn action, CodeFn clipboardCode) {
	g_lobby = lobby;
	g_action = action;
	g_clipboardCode = clipboardCode;
	// Порядок важен: сначала нажатие и ответ, видимость - последней. Если что-то не встало,
	// снимаем всё: иначе видимый пункт выходил бы в библиотеку.
	const bool ok = Make(g_pressHook, kPressRva, OnPress, "press") && Make(g_answerHook, kAnswerRva, OnAnswer, "answer") &&
		Make(g_startHook, kMenuEventRva, OnMenuEvent, "story start") &&
		Make(g_continueHook, kContinueEventRva, OnContinueEvent, "continue start") &&
		Make(g_textHook, kTextLookupRva, TextLookup, "text") && Make(g_visibleHook, kVisibleRva, Visible, "visibility");
	if (!ok) {
		g_visibleHook = {};
		g_continueHook = {};
		g_startHook = {};
		g_textHook = {};
		g_answerHook = {};
		g_pressHook = {};
	}
	g_installed = ok;
	Log("menu: %s", ok ? "multiplayer item on" : "multiplayer item off");
	return ok;
}

}  // namespace menu
