# Képhiány javítása – 2026. október 14.

## Újabb javítás a 20:07–20:08-as próbák alapján

A friss naplóban a videó-submit első belépése a közös `configMutex` megszerzésére várt. A hang első `AppendPacketAsync` hívása visszatért, majd a hang állapota a hívás utáni 4-es jelölőn maradt. A főciklus és a hálózati vétel tovább futott; képkocka nem jutott a lejátszóhoz.

Az append utáni maximumszámítás 64 bites `compare_exchange_weak` ciklust használt. A mellékelt fastcomp SDK `library_pthread_asmjs.c` implementációja hibás: a régi értéket adja vissza sikerjelző helyett, nem frissíti az `expected` változót, és a fordító által generált hívásétól eltérő paraméterlistát használ. A külön `work/test-sdk-atomic.cpp` helyi WASM teszt igazolta, hogy a sikeres csere hamis eredményt, a sikertelen csere pedig igaz eredményt ad, miközben az `expected` változatlan marad.

A már `configMutex` által védett 64 bites hangszámlálók és időértékek egyszerű mezők lettek; a maximumot `std::max` számolja. A főciklus számára publikált 32 bites atomikus számlálók megmaradtak. Az 5-ös hangállapot jelzi a sikeres append utáni út befejezését. Független subagent minden érintett olvasást és írást ellenőrzött: ugyanaz a zár védi őket.

Az SDK hibája bizonyított. Hogy ez okozta-e a konkrét tévés megállást, azt a javítás utáni futásnak kell megerősítenie; a korábbi állapotjelölők nem azonosítják az append utáni egyetlen pontos gépi utasítást.

A teljes Tizen/WASM build, az SDK-hibát reprodukáló helyi WASM teszt és a JS tesztek sikeresek. A tévé az új csomag telepítését `install completed` válasszal megerősítette. Az appot nem indítottam el. A friss aláírt csomag: `outputs/SteamTV.wgt`.

## Korábbi, 17:48-as változat

A 17:31-es Steam-naplóban a stream hitelesítése, indítása, StartAudioData/StartVideoData üzenete és UDP-vétele megtörtént. Az app főciklusa futott, de nem jelentett megjelenített képet. A nulla médiaszámlálók nem bizonyították a submit callbackek elmaradását: ezek korábban csak visszatért média-hívás után jutottak el a watchdoghoz.

## Változások

- A kép és a hang platformra beadása `AppendPacketAsync()`-ot használ. A közös médiazár és a trackek élettartamvédelme megmaradt, a beadás nem használja a szinkron platformvárakozást. A Samsung SDK clear packeteknél funkcionálisan egyenértékű, szinkron validációjú hívásként dokumentálja ezt.
- A főciklus a médiazár megszerzése nélkül közli az atomikus submit-belépés, zár utáni állapot, append-indítás és append-visszatérés adatait. Egy első appendben elakadt worker ezért már nem marad láthatatlan a diagnosztikában.
- A Samsung `ActiveDecodingMode` enumjának értelmezése javítva: `kHardware` a hardveres és `kSoftware` a szoftveres mód. A korábbi számalapú fordított értelmezés téves hardverkorlátokra utalt. A korábbi 2K-ra vonatkozó ilyen állításokat nem tekintjük bizonyítottnak.
- A csomagablak utolsó orphan fragmentjének eldobásakor megmarad az üres állapot a gyűrű körbefordulásánál. Korábban az üres ablak teljesnek látszhatott. A mostani képhiánnyal való kapcsolata nem igazolt; külön regressziós teszt fedi.

## Ellenőrzések és telepítés

- Teljes Tizen/WASM build: sikeres.
- JS tesztek: sikeresek, a jelenlegi párosítási és kontrollerbeállításokat lefedik.
- Natív WASM tesztek: sikeresek, az új csomagablak-körbefordulási teszttel együtt.
- Külön, a Samsung SDK enumját használó hardver/szoftver leképezési teszt: sikeres.
- Két subagent read-only ellenőrzése nem talált bizonyított új blokkoló hibát a javításban.
- A tévé a csomag telepítését `install completed` válasszal megerősítette. Az appot nem indítottam el. A telepített csomag másolata `SteamTV.wgt`.

Az aszinkron API natív backendjének működését és a kép tényleges megjelenését új tévés futással még nem ellenőriztük. A frissítés az azonos appcsomagot használja; a mentett párosítás szándékosan megmarad. A kézhiány pontos futásidejű gyökérokát a korábbi napló nem bizonyította, ezért a javítást nem nevezzük hardveren igazolt megoldásnak.
