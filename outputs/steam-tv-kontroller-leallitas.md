# Kontroller és stream-leállítás javítása – 2026. október 14.

A 22:21–22:23-as kézi próba naplójában a kép többnyire 57–61 megjelenített FPS körül haladt, a hang és kép PTS együtt járt. A felhasználó még akadozást, időnkénti kontrollerlecsatlakozást és a Big Picture bezárásakor lefagyó TV-appot jelzett. A napló 22:23:06-kor `HID: Device id=3 poll failed (-1), closing` sort tartalmaz.

## Javítások

- A JS kontrolleríró korábban előbb lenullázta a közös 160 bájtos WASM állapotot, majd feltöltötte. A C olvasó közben ezt részleges/hiányzó eszközállapotnak láthatta, mivel a C mutex nem védi a JS írást. Most az állapot külön ideiglenes pufferben készül, a közös állapot átadását 32 bites atomikus odd/even sorszám védi. A HID csak a teljes, kétszer ellenőrzött snapshotot olvassa. A valódi, publikált lecsatlakozás továbbra is működik.
- A videószál `VideoStop` callbackje nem zárja be közvetlenül a Samsung lejátszót. A bezárást a meglévő főciklusra halasztja, így a session worker-join nem függ annak böngészős platformhívásaitól. A főciklus `TVMediaStop` útja végzi a tényleges lezárást.
- A küldőszál már 2 ms-os pollingot használ, a `sendQueueCond`-hoz nincs várakozó. A két felesleges jelzést eltávolítottuk; a korábban ezen a tévén beragadó condition-hívásnak nincs feladata ezen az úton.
- Két folyamatos 10 ms-os stereo PCM csomagból egy 20 ms-os beadás készül. A 100/s platformhívás 50/s-ra csökken, legfeljebb 10 ms többletkéséssel. Időrés vagy eltérő csomaghossz előtt a függő csomag külön kerül beadásra. Nincs mesterséges pótló hang. Stream-leállításkor az utolsó pár nélküli 10 ms kimaradhat.

## Ellenőrzés

A natív WASM regresszió ellenőrzi, hogy egy félkész JS snapshot nem távolítja el az élő kontrollert, míg a teljesen átadott lecsatlakozás igen. A JS teszt ellenőrzi a publikálás kész, páros sorszámát. A PCM teszt igazolja a mintasorrendet, a 3840 bájtos / 20 ms-os csomagot, az időrések külön kezelését és a 100→50 összevonást. A meglévő session-queue és offline session-destroy tesztek továbbra is átmentek.

A javított leállítás tényleges böngésző/platform működését, a kontroller tartós kapcsolatát és az akadozás megszűnését hardveren még nem igazoltuk. Az appot nem indítjuk el automatikusan.

A teljes build és a JS/natív tesztek sikeresek. A tévé `install completed` válasszal megerősítette az új csomag telepítését. Az appot nem indítottam el. Az aláírt csomag másolata `outputs/SteamTV.wgt`.
