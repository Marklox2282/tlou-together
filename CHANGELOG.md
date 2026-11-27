# Changelog / Список изменений

The mod updates itself: a new version downloads when the game starts and is used from the next start. Both players need the same version.
Мод обновляется сам: новая версия скачивается при запуске игры и включается со следующего запуска. У обоих игроков должна быть одна версия.

## v36

- No more T-pose: when there is no NPC of your sex on the level, your partner's copy keeps its own look (for example Marlene) but moves properly, crouches and stands. Where such an NPC exists, it looks like your character as before.

---

- Больше нет Т-позы: если на уровне нет NPC вашего пола, копия напарника остаётся в своём облике (например, Марлин), но нормально двигается, приседает и стоит. Где такой NPC есть, напарник выглядит как ваш персонаж, как и раньше.

## v35

- The guest no longer has to press READY: once connected in the main menu, they are ready.
- The guest's story menu opens when the host gets into the game, whatever button the host used.
- After loading, the guest is taken to the host's chapter by itself (no F5 needed).
- Fixed: "%s" instead of the partner's name in the lobby; no false "slow connection" right after connecting.
- Fixed: the partner stood in a T-pose (since v31).
- When your look is put on another character's body (no NPC of your sex on the level), the copy now uses animations for your look: standing, crouching (experimental).

---

- Гостю больше не нужно нажимать «ГОТОВ»: подключился в главном меню — значит, готов.
- У гостя меню сюжета открывается, когда хост оказался в игре, какой бы кнопкой он ни начал.
- После загрузки гостя само переносит в главу хоста (F5 не нужен).
- Исправлено: «%s» вместо ника напарника в лобби; нет ложной «медленной связи» сразу после подключения.
- Исправлено: напарник стоял в Т-позе (с v31).
- Если облик надет на чужое тело (на уровне нет NPC вашего пола), копия теперь берёт анимации под ваш облик: стойка, присед (экспериментально).

## v34

- Fixed: the game crashed on CREATE LOBBY (v33).

---

- Исправлено: игра падала при нажатии «СОЗДАТЬ ЛОББИ» (v33).

## v33

- Ping to your partner in the lobby; an on-screen warning when the connection is slow.
- On-screen messages in all 23 game languages.
- The partner's copy no longer adds extra damage: its bullets are for show, the partner sends the real damage.
- Thrown bricks, bottles, Molotovs, nail and smoke bombs fly in your partner's game too.
- When your partner heals, their copy plays a healing gesture.

---

- Пинг до напарника в лобби; предупреждение на экране, если связь медленная.
- Сообщения на экране на всех 23 языках игры.
- Копия напарника больше не наносит лишний урон: её пули — для вида, настоящий урон присылает напарник.
- Брошенные кирпичи, бутылки, коктейли Молотова, гвоздевые и дымовые бомбы летят и у напарника.
- Когда напарник лечится, его копия делает жест лечения.

## v32

- The guest starts through their own story menu: when the host presses Continue, the guest's story menu opens by itself; after loading, the guest is taken to the host.
- The partner's copy raises its weapon when the partner aims again.
- Experimental animation copying is off by default (`anim_sync=on` in `tlou-together.ini` to try it).
- The copy steps aside for cutscenes; partner's flashlight; aim angle up and down.

---

- Гость начинает через своё меню сюжета: когда хост нажимает «Продолжить», у гостя меню открывается само; после загрузки гостя переносит к хосту.
- Копия снова поднимает оружие, когда напарник целится.
- Экспериментальный перенос анимаций по умолчанию выключен (`anim_sync=on` в `tlou-together.ini`, чтобы попробовать).
- Копия не мешает катсценам; фонарик напарника; наклон прицела вверх и вниз.

## v31

- READY button in the lobby: the host can start once the partner is ready.
- Experimental copying of the partner's animations; smoother movement.

---

- Кнопка «ГОТОВ К ИГРЕ» в лобби: хост может начать, когда напарник готов.
- Экспериментальный перенос анимаций напарника; более плавное движение.

## v30

- The guest starts together with the host; clearer lobby text for the guest.
- The host's lobby opens again when the partner joins.
- The partner's copy keeps up better: runs and sprints sooner, catches up off-screen.

---

- Гость начинает вместе с хостом; понятный текст лобби для гостя.
- У хоста лобби открывается снова, когда напарник подключился.
- Копия напарника меньше отстаёт: раньше переходит на бег и спринт, вне кадра догоняет сразу.

## v29

- The partner's copy holds the same weapon as the partner and fires when they fire.

---

- Копия напарника держит то же оружие, что и напарник, и стреляет вместе с ним.

## v21–v28

- On-screen messages: partner joined, left, died (v28).
- Reconnect after a dropped connection (v27).
- Shared difficulty: the guest plays on the host's difficulty (v26).
- Partner outlined through walls when more than 12 m away (v25).
- Enemies notice and attack your partner's character (v24).
- The partner crouches and aims with the player (v23).
- Shared death: when one player dies, both restart the checkpoint (v22).
- The guest leaves when the host leaves; GO TO PARTNER in the pause menu (v21).

---

- Сообщения на экране: напарник подключился, отключился, погиб (v28).
- Переподключение после обрыва связи (v27).
- Общая сложность: гость играет на сложности хоста (v26).
- Контур напарника сквозь стены, если он дальше 12 м (v25).
- Враги замечают и атакуют персонажа напарника (v24).
- Напарник приседает и целится вместе с игроком (v23).
- Общая смерть: если один погиб, оба начинают с чекпоинта (v22).
- Гость выходит, когда выходит хост; «К НАПАРНИКУ» в меню паузы (v21).

## v18–v20

- START GAME button, mod version and the lobby code shown in the lobby; the code has a secret part (v20).
- Project renamed to tlou-together (v19).
- The partner stands on the exact spot and facing of the player (v18).

---

- Кнопка «НАЧАТЬ ИГРУ», версия мода и код лобби в лобби; в коде есть секретная часть (v20).
- Проект переименован в tlou-together (v19).
- Напарник встаёт точно на место и с поворотом игрока (v18).

## v7–v17

- MULTIPLAYER item in the game's own menu, Steam lobby, invite, join by code.
- The partner appears as the chapter's companion or a chosen character with their look.
- Shared enemy damage and kills; F5 loads the partner's chapter, F7 teleports to them; shared story checkpoints.
- Signed self-update.

---

- Пункт MULTIPLAYER в меню самой игры, лобби Steam, приглашение, вход по коду.
- Напарник появляется как спутник главы или выбранный персонаж со своей внешностью.
- Общий урон и смерти врагов; F5 загружает главу напарника, F7 переносит к нему; общие чекпоинты сюжета.
- Подписанное самообновление.
