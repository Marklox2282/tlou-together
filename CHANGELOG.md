# Changelog / Список изменений

The mod updates itself: a new version downloads when the game starts and is used from the next start. Both players need the same version.
Мод обновляется сам: новая версия скачивается при запуске игры и включается со следующего запуска. У обоих игроков должна быть одна версия.

## 0.40.0

- Versions are now numbered like 0.40.0 (MAJOR.MINOR.PATCH); this one follows v39.
- Tess and other companions: when the guest's companion wanders more than 6 m away from where the host's is and you can't see it, it is moved there.
- More stable: animation copying while climbing is off by default (`climb_anims=on` in `tlou-together.ini` to try it).
- The partner's copy raises its weapon and fires again without that option.
- Experimental (with `climb_anims=on`): melee and grabs/chokes are copied onto the partner's copy too.

---

- Версии теперь вида 0.40.0 (MAJOR.MINOR.PATCH); эта идёт после v39.
- Тесс и другие спутники: если у гостя спутник ушёл дальше 6 м от того места, где он у хоста, и его не видно, его переносит туда.
- Стабильнее: перенос анимаций при лазанье по умолчанию выключен (`climb_anims=on` в `tlou-together.ini`, чтобы попробовать).
- Копия напарника снова поднимает оружие и стреляет и без этой настройки.
- Экспериментально (с `climb_anims=on`): рукопашка и захваты/удушения тоже копируются на копию напарника.

## v39

- Fixed a crash when the partner joined: the mod could take the chapter's real companion (Tess) as the partner's copy, and the level's scripts fought over her.
- Fixed an error when closing the game (the mod crashed while shutting down its network connection).

---

- Исправлено падение при входе напарника: мод мог взять настоящую спутницу главы (Тесс) как копию напарника, и скрипты уровня тянули её в другую сторону.
- Исправлена ошибка при закрытии игры (мод падал, отключая сеть на выходе).

## v38

- Much less desync: the partner's copy stays within half a metre of the partner's real position and jumps to them at once when more than 8 m away.
- The copy no longer disappears or freezes because of a falsely detected cutscene.
- F7 takes you to your partner's real position, not to where their copy is.

---

- Намного меньше рассинхрона: копия напарника держится в полуметре от его настоящей позиции и сразу переносится к нему, если отстала больше чем на 8 м.
- Копия больше не пропадает и не замирает из-за ложно определённой катсцены.
- F7 переносит к настоящей позиции напарника, а не туда, где стоит его копия.

## v37

- Ladders, jumps and climbs: while your partner climbs, their copy follows them exactly and copies their animation (`climb_anims=off` in `tlou-together.ini` turns the animation copy off).
- The copy raises its weapon and aims where your partner aims; if aiming would put that copy into a T-pose, aiming is switched off for it.
- Carried planks and ladders: where one player puts one down, it moves there for the other too.
- Crouching works again, no T-pose from aiming overlays, the partner's flashlight shows on the copy, the copy hides during cutscenes.
- The guest is taken into the host's chapter straight from the main menu.
- Cutscenes and checkpoints follow the partner faster (6 s pause instead of 20 s), without endless swapping when both reach a checkpoint at once.
- Less desync: positions by arrival time again (as before v31), the copy is made from a real NPC spawner.

---

- Лестницы, прыжки и подъёмы: пока напарник лезет, его копия идёт точно за ним и повторяет его анимацию (`climb_anims=off` в `tlou-together.ini` выключает перенос анимации).
- Копия поднимает оружие и целится туда же, куда напарник; если прицел даёт этой копии Т-позу, прицел у неё выключается.
- Переносные доски и лестницы: куда один игрок поставил, туда же она переносится и у другого.
- Снова работает присед, нет Т-позы от прицела, фонарик напарника виден на копии, на катсценах копия прячется.
- Гостя закидывает в главу хоста прямо из главного меню.
- Катсцены и чекпоинты напарника подхватываются быстрее (пауза 6 с вместо 20 с) и без бесконечного обмена, если оба дошли до чекпоинта одновременно.
- Меньше рассинхрона: позиции по времени прихода (как до v31), копия создаётся из настоящего NPC.

## v36

- No more T-pose: when there is no NPC of your sex on the level, your partner's copy keeps its own look (for example Marlene) but moves properly, crouches and stands. Where such an NPC exists, it looks like your character as before.
- The partner's copy no longer falls far behind when running: it is pulled to the partner's real position.

---

- Больше нет Т-позы: если на уровне нет NPC вашего пола, копия напарника остаётся в своём облике (например, Марлин), но нормально двигается, приседает и стоит. Где такой NPC есть, напарник выглядит как ваш персонаж, как и раньше.
- Копия напарника больше не отстаёт далеко на бегу: её подтягивает к настоящей позиции напарника.

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
