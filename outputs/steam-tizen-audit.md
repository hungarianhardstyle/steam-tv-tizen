# Steam TV audit – 2026. október 13.

Az auditot a fő agent és két független subagent végezte. A felhasználó kérésére az audit alatt nem kapcsolódtunk a tévéhez, nem telepítettünk és nem indítottunk új tévés próbát. A forráskódot, a Samsung SDK helyi fejlécdokumentációját és a korábban összegyűjtött naplókat vizsgáltuk.

## Mi bizonyított?

- A Steam elfogadta a párosítást, a tévé mentette az eszközazonosítót és a titkos kulcsot. Ezek az eddigi appfrissítések után is megmaradtak.
- A Steam elfogadta a Big Picture streamkérést.
- A tévé elküldött egy 21 bájtos UDP Connect csomagot a PC 27031-es portjára, és kapott egy 21 bájtos választ.
- A válasz ellenőrzése és fejlécfeldolgozása sikerült: `type=2` (ConnectACK), a célazonosító egyezett, `ipMatch=0` az IP-címek egyezését jelenti.
- A stream konfigurálási callbackje nem futott le, a videó indulása nem igazolt. A jelenlegi elakadás a médialejátszó indulása előtt van.

**A pontos gyökérok a meglévő naplóból nem bizonyítható.** A `phase=9` csak a SessionConnect visszatérését jelzi; nem mutatja a belső kézfogási állapotot. A napló Info szintre szűr, ezért elrejti a ClientHandshake küldését és az ismeretlen csatorna diagnosztikáját. Nem tudjuk megkülönböztetni, hogy a ConnectACK csatornakiosztása, a következő üzenet küldése vagy a szerverkézfogás feldolgozása akad-e el. Nem találtunk bizonyított WASM callback-aláírási hibát ezen az útvonalon. A hibát nem lehet az eddigi bizonyíték alapján hálózati vagy Opus-hibának kijelenteni.

A fázisjelző maga is hibás: a Connect után írt 9 felülírhat egy gyors callback által írt 10/11-et. Az előrehaladást kizárólag a callbackeknek kell jelölniük. A jelenlegi konfigurációs naplók hiánya külön bizonyíték, de a fázisszám önmagában nem elég.

## Biztos hibák és hiányok

| Prioritás | Forrás | Hiba és következmény | Szükséges javítás |
|---|---|---|---|
| P1 | `work/steam-tv/probe.c:67`, `:159`; `app.js:81` | Sikertelen indítás után a C `session_requested` és a JS `starting` beragad. A gomb tiltva marad, az új indítás nem történik meg. | Hiba esetén mindkét állapot feloldása, biztonságos munkamenet-takarítás. |
| P1 | `work/steam-tv/probe.c:60–65`, `:137` | Nincs teljes kapcsolódási határidő. Elmaradó kézfogásnál az app korlátlanul „indítást” mutathat. | Határidő és a tényleges kapcsolódási fázist megnevező hiba. |
| P1 | `work/steam-tv/app.js:38–45`, `:27–34` | Sérült vagy hiányzó identitásnál új kulcs keletkezik, de a párosított jelző megmarad. A párosítás gombja rejtve marad az érvénytelen kapcsolat mellett. | Identitás újragenerálásakor a párosított jelző érvénytelenítése; a meglévő érvényes kulcs megőrzése. |
| P1 | `work/steam-tv/media.cpp:80`, `:158` | A csatorna stop/start vagy új konfiguráció nincs kezelve. A `DataStop` üres, meglévő player mellett a Prepare azonnal sikerrel visszatér, miközben a régi dekóderkonfiguráció marad. | A média-életciklus kezelése egy Steam-munkameneten belül is. |
| P1 | `work/steam-tv/app.js:94`; `probe.c:164–166` | Csak távirányító/billentyű események továbbítása létezik. A tévéhez csatlakoztatott Xbox/PS5 kontrollerhez nincs gamepad-beolvasás és Steam HID integráció. | A kontroller útvonal teljes implementálása. Ez jelenleg hiányzó funkció. |
| P2 | `work/steam-tv/media.cpp:80`, `:111` | A kép indulása a hangcsatorna és hangdekóder indulásától függ. Hanghiba esetén nincs kép sem. | A hanghiba külön kezelése; működő videó önálló indításának támogatása. |
| P2 | `work/steam-tv/media.cpp:114`; `app.js:15` | A lejátszás késznek jelzése a track megnyílására épül, nem az első igazolt videóképre. | A kész állapotot tényleges első képkockához kötni. |

## Ellenőrizetlen médiakockázatok

- `media.cpp:109–110`: az aszinkron Open után azonnal Play következik. A helyi működő Samsung-lejátszó referencia megvárja az OpenPending/Open állapotot. Ez indítási verseny lehet, de a jelenlegi elakadás ennél korábban van.
- `media.cpp:100`: a közvetlen hardveres Opus út ezen a tévén nincs igazolva. A helyi referencia Opusból PCM-et készít, és azt adja a Samsung-lejátszónak. Az Opus támogatás nem kezelhető kész tényként.

## Ellenőrzések

- `node work/steam-tv/test.mjs`: a helyi identitás/párosítás/távirányító és automatikus indítás egységtesztje átment. Ez nem képátviteli teszt.
- `node work/audit-ui.mjs`: a sérült identitás melletti hamis párosított állapot és a hiba utáni tiltott újrapróbálás tévé nélkül reprodukálható.
- A subagentek külön vizsgálták a szálkezelést, callback-aláírásokat, csomagfeldolgozást, a Samsung médialejátszó indítását és életciklusát.

## Következő javítási sorrend

1. A kapcsolódási állapot és határidő rendbetétele; a ConnectACK → ClientHandshake → ServerHandshake út helyi tesztelése. A jelenlegi naplóhiányok miatt nem állítunk bizonyítatlan gyökérokot.
2. Hiba utáni újrapróbálás és párosítás-helyreállítás.
3. Médiaforrás állapotvezérelt indítása, hangút és csatorna-életciklus javítása.
4. Kontrollerintegráció.

A tévén jelenleg fejlesztési prototípus van, nem kész Steam Link pótlás. Az audit alatt nem módosítottuk az appot és nem végeztünk új tévés tesztet; a hang és kép valódi működése továbbra sincs igazolva.
