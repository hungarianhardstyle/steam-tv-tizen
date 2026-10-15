# Steam Remote Play kliens Samsung Tizenre – aktuális állapot

## Elfogadott működés

- Samsung UE65U8072FUXXH (2025) tévén futó, sideloadolt alkalmazás.
- PC: Windows 11 + GeForce RTX 5070 Ti. A PC-n a használathoz kizárólag a Steam kell, engedélyezett Remote Playjel. Nincs saját PC-s küldő, Node.js, Sunshine vagy böngészős képernyőmegosztás.
- A TV kliens Steam-párosítást végez, kéri a Big Picture munkamenetet, megjeleníti a videót és hangot, és továbbítja a tévéhez csatlakoztatott kontroller bemenetét. Xbox vagy DualSense kontroller.
- A kész és ellenőrzött csomagot a tévére is telepíteni kell. A PC-n megtalálható: `C:\Program Files\Apps2Samsung\Apps2Samsung.exe`.

## Ellenőrzött technikai alap

Az IHSlib forrása letöltve a munkamappa `work/ihslib` könyvtárába. A README felderítést, hitelesítést és Steam Remote Play streaminget ír le. A forrásban külön discovery, authorization, streaming és session modulok vannak; a protokoll tartalmaz Big Picture interfészértéket. Ez még nem bizonyítja a jelenlegi Windows Steam és e tévé együttműködését.

A Samsung dokumentáció szerint Tizen WASM alkalmazásból TCP és UDP socketek használhatók worker/pthread szálon. Ez fontos, mert egy szokásos böngészős WebRTC oldal nem tud közvetlenül a Steam saját streaming protokolljához kapcsolódni.

Az IHSlib POSIX UDP implementációja `fcntl()`-t használ a blokkolás állítására. A Samsung játé streaming portolási leírása ezt hiányzó hívásként említi, ezért a socketkezelés portolását ellenőrizni kell. A projekt további alapfüggőségei a protobuf-c és egy kriptográfiai backend; a Tizen socket- és hardveres médiaillesztés még nincs megépítve.

A Locomo egy meglévő, IHSlibre épülő Steam kliens ARM64 Linuxra. Hasznos referencia, de Linux/SDL/V4L2 függőségei miatt közvetlenül nem telepíthető Tizen WGT-ként.

## Hátralévő munka

1. Samsung Emscripten SDK és Tizen build/certificate eszközök felkutatása, illetve beszerzése. Az alapellenőrzés nem talált Tizen CLI-t a PATH-ban vagy SDK-t a gyakori telepítési helyeken.
2. Első tévés próba: WASM socket-hozzáférés, Steam felderítés és PIN-párosítás. Ez legyen az első bizonyított rész, ne egy tetszetős, de nem kapcsolódó felület.
3. A Steam streaming munkamenet és Big Picture kérés ellenőrzése.
4. Samsung hardveres videódekóder, hang, kontroller és mentett párosítás illesztése.
5. Saját TV-re aláírt WGT, telepítés Apps2Samsunggal, majd valódi kép/hang/bemeneti késleltetésmérés.

Még nincs működő, közvetlen Steamhez csatlakozó Tizen build, nincs aláírt csomag és nem történt TV-telepítés.

## Helyi ellenőrzés eredménye

- TV: `192.168.1.6`, a Samsung REST API azonosította `UE65U8072FUXXH`, `25_KSUE_UB`, Tizen modellként; `developerMode = 1`.
- PC: `192.168.1.9`, Ethernet 5.
- A TV TCP 26101 fejlesztői portja nyitott. Az Apps2Samsung aktuális naplója szerint a tévé a fejlesztői adatkapcsolatot bontja; a Developer Mode Host PC IP ellenőrzése még szükséges. A nyitott port nem bizonyít sikeres telepítési jogosultságot.
- A hivatalos Samsung Emscripten 1.39.4.7 Windows SDK letöltve és a munkamappában kibontva, helyi `--embedded` konfigurációval aktiválva. Az `emcc --version` sikeres.
- A fordításhoz szükséges IHSlib, protobuf-c és mbedTLS források a munkamappában vannak. Ez fejlesztői eszköztár; a végleges PC-s használathoz nem kell.
- Az IHSlib protokollmag és a függőségek Samsung Emscriptennel sikeresen lefordultak egy összevont LLVM bitcode objektummá (`work/ihs-core.bc`, `work/build-ihs-core.ps1`). Ehhez a POSIX UDP implementációban explicit `sys/time.h` include kellett. Ez **nem futtatható WASM modul**, nem végleges alkalmazás, és nem bizonyít tévés működést. A socketen végzett `fcntl(F_SETFL)` hívást a Samsung SDK futás közben abortálja, tehát a socket backend módosítása továbbra is szükséges a protokoll futtatása előtt.
- A Docker parancs elérhető, de a Docker daemon nem fut; a jelenlegi fordítás közvetlenül a Samsung Windows SDK-val történik.

Az új Samsung menüben a Developer Mode beállítóablaka: Home → Apps → lejjebb App Settings → 12345 a távirányítóval vagy képernyős számbillentyűzettel. Host PC IP: `192.168.1.9`, majd OK és TV újraindítás. Hivatalos útmutató: https://developer.samsung.com/smarttv/develop/getting-started/using-sdk/tv-device.html

## Első valódi tévés próba

A Host PC IP beállítása után az SDB kézfogás sikerült. A tévé Tizen 9.0-t és ARMv7 platformot jelentett.

Elkészült és a tévére feltelepült a külön `Steam TV Probe` diagnosztikai alkalmazás (`SteamTV001.Probe`). A tévé megerősítette az `install completed` eredményt, és az app debug módban elindult. Az Apps2Samsung beépített Jellyfin tanúsítványát a tévé érvénytelen lánc miatt visszautasította; a már létező, felhasználói `Jelly2Sams - Public` profiljával aláírt csomagot elfogadta. A profilok jelszava és privát kulcsa nem került a kimeneti fájlokba.

A Samsung WASM socket útvonala működik: a tévés app felfedezte a PC-n futó Steamet (`192.168.1.9`). A Steam saját `remote_connections.txt` naplója megerősítette a TV-ről érkező authorization kéréseket és az InProgress válaszokat. A párosítás még nem sikerült, 7-es hibakód érkezett.

Javítások a próbaappban és a portolt könyvtárban:

- A Tizenen nem támogatott `fcntl(F_SETFL)` helyett a könyvtár meglévő blokkoló socketmódját használjuk; nem blokkoló mód kérése sikertelen. A vizsgált hívók blokkoló módot kérnek.
- Az SDL thread-belépő ténylegesen `int(void*)` szignatúrát kapott, és a timer worker `void(void*)` szignatúrával fut; a korábbi függvénypointer-cast WASM-on nem biztonságos.
- A távirányító iránygombjai, OK és Vissza kezelve, felfedezés után fókusz a párosítási gombon.
- Folyamatban levő párosításnál a gomb letiltva marad. Ismételt OK nem generálhat új, az előző kérésnek ellentmondó PIN-t.
- Identitás és PIN `crypto.getRandomValues` segítségével készül; az identitás helyben megmarad. Az identitásmegőrzés, hibás helyi tároló helyreállítása, PIN, távirányító és ismételt párosítás blokkolása Node assert-teszttel ellenőrizve.

A frissített próbaapp telepítése sikerült. Még nincs video/audio dekóderillesztés, kontroller-visszaküldés és működő Big Picture streaming munkamenet. A következő lépés a PIN-javítás utáni valódi Steam-párosítás.

## A tartós 7-es hiba vizsgálata

A PIN-rögzítés javítása után is a Steam küldte vissza a 7-es AuthorizationFailed választ. A TV-n lefuttatott RSA önellenőrzés sikeres (`0`, 256 bájt), de ez önmagában nem bizonyítja a teljes protokoll helyességét.

A `kxn/ihslib` újabb forrásában megtaláltuk a PIN-nel maszkolt X25519 kulcscserét és az AuthorizationConfirmed lépést, amelyek az eredetileg használt `mariotaku/ihslib` verzióból hiányoznak. A próbaapp fordítása átállt erre a változatra, megőrizve a Tizen socket/thread illesztést. A sikeres párosításkor kapott új titkos kulcs helyben megmarad, az eszközazonosító változtatása nélkül. Ennek saját assert-ellenőrzése sikeres.

Az új kulcscserét tartalmazó WASM-build lefordult és a tévére feltelepült (`install completed`). A Steam-párosítás eredménye még nincs igazolva; következőként ezt kell valódi PIN-jóváhagyással tesztelni. Forrás: https://github.com/kxn/ihslib

## A folytatás: helyi ellenőrzés és friss telepítés

A tévés kép elmaradása utáni helyi ellenőrzőlánc rendbe került:

- A `work/test-native.ps1` és a build-szkriptek elhaltak, mert a **Windows PowerShell 5.1** a
  natív programok stderr-üzenetét (`emcc` figyelmeztetések, Node `pthread_sigmask` értesítés)
  `$ErrorActionPreference = 'Stop'` mellett végzetes hibának veszi. A `build-ihs-core.ps1`,
  `build-opus.ps1`, `build-probe.ps1` és `test-native.ps1` mostantól kizárólag a tényleges
  exitkódra bukik el, a stderr nem állítja meg a folyamatot.
- A `test-native.c`-ből hiányzó `<unistd.h>` miatt az `usleep` implicit deklarációja buildhiba
  volt — ezért a natív teszt el sem indult. Pótolva.
- A natív teszt kiterjed a korábban ellenőrizetlen kézfogási útra: ConnectACK → Handshaking,
  ismételt (replay) ConnectACK nem indítja újra a kézfogást, ServerHandshake → Authenticating
  az átvett MTU-val (1400), végül a sikertelen hitelesítés lebontása. **Minden állítás átment.**
- `node work/steam-tv/test.mjs`: párosítás-helyreállítás, újrapróbálás, első képkocka
  megszakítása, távirányító és gamepad-leképezés — átment.
- A probe a jelenlegi forrásból újraépült (`ihs-core.bc`, `opus.bc`, `steam-tv/ihs.js`,
  `ihs.wasm`), majd a `dotnet run --project work/tv-device -- install-probe` a friss, aláírt
  csomagot feltelepítette a tévére (`install completed`). **Az appot nem indítottuk el.**

### Új diagnosztika a következő tévés méréshez

Az audit nyitva hagyta, hogy a ConnectACK után hol áll meg a kézfogás, mert erről nem készült
napló. Ez pótolva — kizárólag naplózással, a viselkedés változatlan:

- `ch_discovery.c` / `OnConnectACK`: jelzi az elfogadott ConnectACK-ot, és külön azt is, ha a
  csomagot cél-connection vagy állapot miatt eldobja.
- `ch_control.c` / `OnServerHandshake`: jelzi a megérkezést, az átvett MTU-t, valamint a
  stale-ként eldobott ServerHandshake-et.
- `ch_control.c` / `IHS_SessionChannelControlHandshake`: jelzi, ha a kézfogás már fut, tehát
  nem indul újra.
- Az app naplógyűrűje 32 → 200 sor, és minden naplósor a konzolra is kimegy, így az
  `inspect-tv.mjs --console` közvetlenül olvassa.

A tévén a `Send control message: ClientHandshake` (Debug) és a fenti új sorok együtt már
megmondják, hogy a kliens elküldte-e a kézfogást, és hogy a szerver válasza megérkezett-e.
Tévés képátvitel továbbra sincs igazolva.

## Beállítható felbontás és képfrissítés, 4K-ig

Az appban a főoldali panelen két új, távirányítóval vezérelhető gomb van, és a választás
megmarad az újraindítás után is:

- **Képfrissítés:** 30 / 60 FPS váltógomb.
- **Felbontás:** 1024×576 → 1280×720 → 1600×900 → 1920×1080 → 2560×1440 → 3840×2160
  (körbeforgó gomb, mert a távirányítóval ez a legbiztonságosabb).

A beállítás a `localStorage`-ban marad (`steam-tv-fps`, `steam-tv-resolution`), és stream közben
zárolt. A C oldal a `TVLink.settings()` híváson keresztül olvassa ki.

### Hogyan jut el a hostig

- **Indításkor** (`IHS_StreamingRequest.maxResolution`) – ez az egyetlen felbontás-kapcsoló ezen
  a protokollüzeneten; fps és bitráta itt nem kérhető.
- **`configuring` callback** – `maxWidth`, `maxHeight`, `maxFps`, `maxBitrateKbps`, `enableHevc`.
  A bitráta a felbontásból és az fps-ből számol (1080p60 ≈ a korábbi 20 Mbps, 4K60 ≈ 80 Mbps).
- **1080p felett** a kliens HEVC-et kér (`enableHevc`), mert ott a H.264 már rossz illeszkedés.
- A **tényleges** felbontás és kodek csak a videó `start` callbackjében derül ki – a host a
  `setCaptureSize` üzenettel felül is bírálhatja, és azt a kliens nem tudja elutasítani.

### Médiacsatorna

- A kodekstringet a tévé platformja bírálja el, ezért a kliens a tényleges kodekból és a
  felbontás/fps arányból számol H.264 profilszintet (`avc1.64001F` … `avc1.640034`) vagy HEVC
  szintet (`hvc1.2.4.L150.B0` / `L153.B0`). Ezt a létra a tévén is ellenőrzi magát
  (`TVMediaSelfCheck`).
- **Új:** a `ValidateVideoConfig` segítségével a kliens **előre megkérdezi a tévét**, elfogadja-e
  a kért kodekot, felbontást és fps-t; ha nem, érthető hibaüzenettel áll meg a néma elakadás helyett.
- **Új:** a késleltetési mód a tévé képességéhez igazodik (`has_ultra_low_latency`), korábban
  feltétel nélkül ultra-low volt.
- **Javítás:** az ihslib egyetlen kész frame-re 2 MB-ot engedett. Egy 4K kulcskocka ezt
  átlépheti, és a könyvtár ilyenkor eldobja a függőben levő adatot, majd **bontja a teljes
  munkamenetet**. A korlát 8 MB-ra nőtt (`ch_data_video.c`).
- **Javítás:** a kliens eddig egyetlen frame-visszajelzést sem küldött a hostnak
  (`IHS_SessionReportVideoFrameComplete`); a host adaptív bitrátája e nélkül nem kap statisztikát.

### Kontroller

- A tévéhez csatlakoztatott Xbox/DualSense pad eddig kizárólag `mapping === 'standard'` esetén
  került be. A Tizen ezt Bluetooth-padnál nem mindig jelenti, ezért mostantól a szabványos
  elrendezésű (4 tengely / 16 gomb) pad is elfogadott, a nem kontroller eszköz továbbra is kimarad.
- Stream közben a távirányító gombjai (köztük az OK) a játékhoz mennek, nem az app gombjaihoz –
  ezt a JS-teszt külön ellenőrzi.

### Tesztek

- `node work/steam-tv/test.mjs`: a beállítások körbeforgatása és mentése, a stream közbeni zár,
  az újraindítás utáni párosítás- és beállításmegőrzés, valamint a kontroller-leképezés (nem
  standard mappinggel is) átment.
- `work/test-native.ps1`: a kézfogási lánc változatlanul átmegy.

A tévés működés továbbra sincs igazolva: a kézfogás a ConnectACK után a tévén még nem bizonyított,
ezért a most telepített build részletes naplója alapján kell a valódi elakadási pontot megkeresni.

## A 22:32-es build indulási elakadása — megtalált hiba

A tévére telepített, beállításokkal bővített build **el sem indult**: a képernyőn az induló
`WASM modul betöltése…` szöveg maradt, a `crypto` sor üresen, a PC keresése el sem kezdődött, és a
Steam naplójában 22:02 óta **nem jelent meg új bejegyzés** (sem felderítés, sem párosítás).

A hiba a saját új kódomban volt: a HEVC profilszint-küszöb. A 4K60 = 3840×2160×60 =
497 664 000 minta/másodperc, ami **nem** nagyobb az 5.1 szint 534 773 760-as határánál, ezért a
függvény `L150`-et adott vissza a helyes `L153` helyett. Az ezt ellenőrző `assert` pedig — ez a
lényeg — **a Tizen proxied main-szálában némán megöli az appot**: nincs hibaüzenet, a böngésző
szála tovább fut, a képernyő pedig az induló állapoton marad. Ez pontosan a látott kép.

Javítások:

- A HEVC létra a szint saját határához igazodik (`L150` ≤ 267 386 880, `L153` ≤ 534 773 760,
  `L156` felette), így 4K60 helyesen `hvc1.2.4.L153.B0`.
- A kodeklétra ellenőrzése **nem `assert` többé**, hanem naplózott jelentés: egy eltérés
  megjelenik a naplóban, de nem tudja megölni az appot induláskor.
- Az indulás lépései **láthatók a tévé képernyőjén** (`Indulás: identitás betöltése…`,
  `Identitás és média rendben…`, `Steam indítása…`), és a `crypto` sor kiírja az
  **eszközazonosítót**. Így egy fotó is megmondja, hol áll meg.

### A párosítás megjegyzése

A Steam `remote_connections.txt` tanúsága szerint ma este **három különböző eszközazonosító**
párosodott (…393991, …76606, …1581), mindegyik új PIN-jóváhagyással. A párosítás megjegyzése tehát
nem volt elég robusztus. Az azonosító és a titkos kulcs mostantól **mentett másodpéldányban** is
tárolódik: ha az elsődleges bejegyzés megsérül, a kliens a mentésből állítja helyre ahelyett, hogy
új eszközazonosítót generálna és új PIN-t kérne. Új azonosító csak akkor készül, ha mindkét
másolat használhatatlan.

Az eszközazonosító a tévén kiírt érték és a Steam naplójában szereplő `device ID` összevetésével
ellenőrizhető: ha megegyezik, a párosítás megmaradt.

## A kézfogás láthatóvá tétele

Az audit nyitva hagyta, hogy a ConnectACK után hol áll meg a kézfogás — kiderült, hogy a napló
pont ott volt vak: a `session.c` a **kimenő** csomagokat csak a Connect esetén naplózta, a
**bejövőket** pedig csak addig, amíg a kapcsolat `Connecting` állapotban volt. A ConnectACK
utáni teljes szakasz (ClientHandshake, ServerHandshake, hitelesítés) így naplózatlan maradt.

Javítás:

- `IHS_SessionSendPacket` mostantól **minden** csomagot naplóz, amíg a munkamenet nem érte el a
  `Connected` állapotot (típus, csatorna, csomagazonosító, src/dst connection id, méret, siker).
- `SessionRecvCallback` ugyanezen szakasz **minden** bejövő csomagját naplózza.
- Az elutasított csomag naplója eddig csak annyit írt, hogy valami nem egyezik; mostantól kiírja
  a típust, a csatornát, a src/dst és a saját connection id-ket, az állapotot, valamint hogy az
  IP és a port egyezett-e — így azonnal látszik, melyik feltétel bukott.
- Ellenőrizve a forrásban: a `ClientHandshake` **nincs titkosítva** (`IsMessageEncrypted`), tehát
  a Steam elé küldött első üzenet a kulcs előtt is értelmezhető.
- A **legfrissebb naplósor a tévé képernyőjén is megjelenik** (`#trace` sor), így egy fotó is
  megmutatja, hol áll meg a folyamat — nem kell inspector vagy olvasható eszköznapló.

A tévé `sdb` shell-je nem ad vissza kimenetet (`ls`, `dlogutil` üresen tér vissza), ezért a
rendszer-napló olvasása nem járható út; a képernyőre írt nyomvonal és a bővített napló viszont
igen.

## A vételi út helyi bizonyítéka

A natív teszt eddig a `ServerHandshake`-et **közvetlenül a csatorna-handlerbe** táplálta, ami nem
tudja elkapni azt a csomagot, amely már a vétel során elvész. Az új teszt egy **valódi
ServerHandshake csomagot** vezet át a teljes úton: csomagértelmezés → peer- és
connection-id-ellenőrzés → a megbízható csatorna ablaka → vezérlő-dispatcher. Az eredmény: a
munkamenet helyesen vált `Authenticating` állapotba az átvett MTU-val (1400).

Ezzel helyben bizonyított a teljes állapotgép és a vételi út:

| Lépés | Állapot |
|---|---|
| ConnectACK → Handshaking + ClientHandshake a sorban | bizonyított |
| Ismételt (replay) ConnectACK nem indít újra | bizonyított |
| ServerHandshake **teljes vételi úton** → Authenticating | bizonyított |
| ServerHandshake közvetlen dispatcher-úton → Authenticating | bizonyított |
| Sikertelen hitelesítés → munkamenet lebontása | bizonyított |
| Kodeklétra (H.264 és HEVC, 1024×576 … 3840×2160) | bizonyított |
| Gamepad/HID másodpercenkénti jelentés, hotplug, Opus PCM | bizonyított |

Ami ezek után **már csak a tévén** dőlhet el: hogy a ClientHandshake datagram ténylegesen kimegy-e
a tévé UDP socketén, és hogy a Steam válaszol-e rá. Erre szolgál a most telepített naplózás és a
képernyőn látható nyomvonal.

A képernyőn a **legutolsó három** naplósor látszik (`#trace`), mert a kézfogás egy sorozat: egy
fotó így egyszerre mutatja a ConnectACK elfogadását, a ClientHandshake elküldését és az esetleges
választ.

## 4K keretméret: a szakadás kockázatának megszüntetése

Az ihslib egyetlen összeállított képkockára **8 MB** plafont tartott, és a
`NeedEscape` (emulációvédelem) miatt egy keret akár **másfélszeres** helyet is igényelhet — így a
hasznos tartalom valójában ~5 MB volt. A plafon átlépése nem elszigetelt hiba: a könyvtár
eldobja a függőben levő adatot és **bontja a teljes munkamenetet**.

- A plafon **16 MB**, de a puffer **igény szerint nő** (`IHS_BufferEnsureCapacity`, és a
  `clear(..., false)` megtartja a kapacitást a keretek között), ezért a tétlen költség továbbra is
  a kezdeti 128 KB — nem foglalunk előre 16 MB-ot a 128 MB-os WASM-heapból.
- A `media.cpp` túl nagy keretre eddig **végzetes hibát** adott, ami szintén a munkamenet
  lebontását jelentette. Mostantól `ReportLost`: a stream a következő kulcskockára szinkronizál,
  egyetlen rossz keret nem viszi magával a kapcsolatot.

### A sorba állított ClientHandshake bájtjai

A `struct IHS_QueueItem` a `session.c`-ben privát módon definiálva van (`packet` + `reliable`), így
a natív teszt befejezheti a típust és **közvetlenül megvizsgálhatja a sorba állított csomagot** —
ezt a küldő szál csak `IHS_SessionConnect` után indítja, ezért a vizsgálat determinisztikus.

A ConnectACK feldolgozása után a sorban lévő csomagról a teszt igazolja:

- `type = Reliable`, `channel = Control`, `reliable = true`;
- `srcConnectionId = 189` (a sajátunk), `dstConnectionId = 7` (a ConnectACK-ból vett host-azonosító);
- a törzs első bájtja `k_EStreamControlClientHandshake`, azaz **nyílt szöveges** kézfogás.

Ezzel a kliens oldali kézfogás **bájt szinten** igazolt a ConnectACK-tól a ServerHandshake
feldolgozásáig. Ami ezek után kizárólag a tévén dőlhet el: hogy ez a datagram ténylegesen kimegy-e
a tévé UDP socketén, és hogy a Steam válaszol-e rá.

## A gyökérok: a küldési sor elakadt a kézfogásnál

A tévén látható nyomvonal (`#trace`) döntötte el a kérdést. A sorrend:

1. `Session: Send packet: type=1, channel=0, id=0, fragment=0, src=59, dst=0, size=21, sent=1`
   — a Connect **kiment**.
2. `Discovery: ConnectACK accepted, host connection id 128` — a Steam válaszolt, és a kliens
   elfogadta.
3. `Control: Send control message: k_EStreamControlClientHandshake, id=0` — a ClientHandshake
   **sorba került**.

A hozzá tartozó `Send packet` sor viszont **soha nem jelent meg**. Mivel a `sendQueue` a
`SessionSendPacket` egyetlen hívója a küldő szál (`session.c`), és a Connect bizonyítottan
kiment, a szál a Connect után **megállt**: a ClientHandshake a sorban maradt, ezért a Steam soha
nem kapott kézfogást, ezért nem indult kép. Ez magyarázza a teljes eddigi jelenséget — a
párosítás azért működött, mert az a kliens saját szálán fut, nem a munkamenet küldő szálán.

A küldő szál `SDL_CondWait`-tel várakozott, **ami ezen a platformon nem tud timeoutolni**: ha a
jelzés elveszik (vagy a szál a várakozás előtt/után akad el), a sor örökre áll.

Javítások:

- **Időkorlátos várakozás:** új `IHS_CondWaitTimeout` (SDL `SDL_CondWaitTimeout`, POSIX
  `pthread_cond_timedwait`), a küldő szál 5 ms-onként újrapróbál. A jelzés így legfeljebb
  késleltet, nem akadály.
- **Biztos háló:** a küldési sor ürítése kikerült egy közös `SessionDrainSendQueue` függvénybe,
  amelyet a küldő szál **és a vételi út is** meghív. A vételi szál bizonyítottan él (hiszen
  megérkezett a ConnectACK), így a kézfogás akkor is kimegy, ha a küldő szál meghalt vagy elakadt.
  A sor zárja a küldés idejére felszabadul, ezért egy blokkoló küldés sem akaszthatja meg a sort.
- **Napló mondja meg, melyik szál küldött:** `Sending queued packet via send worker` vagy
  `via receive path` — a következő tévés futás így egyértelműen megmutatja, melyik mechanizmus
  hozta el a kézfogást.

A javított csomag telepítve. Ha a kép ezután sem indul, a napló már pontosan megmondja, hova
jutott a kézfogás.

### A képernyőn látható diagnosztika csiszolása

Az első fotó tanulsága: a kézfogás 5–8 különálló esemény, a 3 soros ablak viszont a friss indítás
első sorait mutatta (a Connect előtt). Ezért:

- a nyomvonal ablak **8 sor**, 13 px betűvel, hogy egyetlen fotó elég legyen;
- az **ismétlődő sorokat összevonja** (a retransmission-ök nem tolják ki a lényeget);
- a státuszsor a **fázisszámot** is mutatja (`TVStage()`), ami tömör és nem görög el: 5 = Steam
  indítása, 10 = videóbeállítás, 11 = stream kapcsolódott, 12 = első kép megvan.

## A valódi gyökérok: a könyvtári szálak nem tértek vissza a naplózásból

Három egymást követő tévés fotó együtt árulta el. Az elsőn a küldő szál a **Connect elküldése
után** állt meg (`Send packet: type=1, … sent=1` volt az utolsó sora). A következő kettőn viszont
**már a szál legelső naplósora után** (`Session: Send worker started`) — vagyis a hiba pontosan
egy sorral korábbra került, amikor én egy új naplósort tettem a szál elejére.

Mindkét esetben ugyanaz történik: **a szál az első `MAIN_THREAD_EM_ASM` (JS-proxy) hívása után nem
folytatódik**. A JS lefut (a sor megjelenik a képernyőn), a C szál viszont nem ébred vissza. Ez
pontosan megmagyarázza a teljes eddigi jelenséget: a küldő szál az első naplózásnál megállt, ezért
a sorba állított ClientHandshake soha nem ment ki, ezért nem indult kép.

Javítás:

- **A könyvtári szálak többé nem hívnak JavaScriptet.** A `diagnostic_log` csak egy
  gyűrűpufferbe ír (atomikus számlálóval), és a **fő ciklus** írja ki a képernyőre — az a szál,
  amelyik bizonyítottan működik (`event()`, státuszfrissítés). Ezzel egyetlen könyvtári szál sem
  tud megállni a naplózáson, és nem tart zárva zárat sem.
- A **időkorlátos várakozás** megmaradt a küldő szálban (5 ms), mert a jelzés elvesztése ellen is
  véd.
- A **vételi út ürítése visszavonva**: az egy spekulatív biztos háló volt, és nem akartam, hogy a
  diagnózist egy saját változtatásom zavarja.

## Indítás gombnyomásra

Kérésre a **Big Picture nem indul el magától**: a kliens a párosítás után csak **fókuszba teszi**
a „Big Picture indítása" gombot, és megvárja a nyomást. Így van idő felbontást és képfrissítést
állítani. (A `host()` callback már nem hívja meg a start gombot.)

A státuszsor fázisjelzője is **élő** lett: minden naplósor frissíti a `[fázis N]` utótagot, nem
csak egyszer, a szöveg kiírásakor.

## Áttörés: a munkamenet összeáll (7. kör)

A tévé **debug módban** indítva az inspectoron át olvasható, ezért többé nem kell fotózni. Ezzel a
teljes napló látható, és a stream nagyot lépett:

- párosítás megmaradt (ugyanaz az eszközazonosító, PIN nélkül) ✓
- streamkérés elfogadva → UDP Connect → ConnectACK → kézfogás → hitelesítés → tárgyalás ✓
- **`Steam stream kapcsolódott` (fázis 11)**, keepalive-ok mennek, **hangcsatorna elindul** ✓
- a host oldalán: `Video Streaming: enabled`, `Created encoder NVENC for codec 4`,
  `Capture resolution set to 1920x1080`, `Dropped N video frames` — a PC kódol ✓

Két valódi hiba javítva:

1. **A küldő szál megfagyása.** A `SessionSendWorker` `SDL_CondWait`-tel várt, ami ezen a platformon
   nem ébredt vissza, ezért a ClientHandshake soha nem ment ki. Helyette 5 ms-os pollozás. Ez volt a
   „nincs kép" eredeti gyökéroka. (Az `IHS_CondWaitTimeout` külön nem vált be.)
2. **Hiányzó fps a streamkérésben.** A `CMsgRemoteDeviceStreamingRequest` `maximum_framerate_*`
   mezőit az ihslib sosem állította, ezért a host `-nan(ind) FPS`-t számolt. Pótolva, alapértelmezés
   60/1; a host most `Maximum capture: 1920x1080 60.00 FPS`-t ír.

Emellett: a `ClientSystemInfo` helyes **VDF-blokk** (a host eddig KeyValues-hibát írt),
`IHS_SessionSetClientCapabilities` (form factor, dekóder, bitráta-plafon) és a
`k_EStreamControlVideoDecoderInfo` elküldése a kapcsolódás után — a host naplózza is:
`Client video decoder set to Samsung Tizen WASM hardware decoder`.

## A hátralévő akadály

A host **soha nem küldi el a `StartVideoData`-t**, ezért nincs videócsatorna és nincs kép. A host
viszont `Streaming` állapotban van, a kódoló fut. Kizártam (bizonyítékkal):

- **csomagvesztés/titkosítás**: a „hiányzó" 11–14 azonosítók a `SetIcon` **töredékei** (0–4.
  fragmentum), a napló a fej azonosítóját írja — nincs veszteség;
- **fps** (javítva), **rendszerinfó** (most érvényes VDF), **dekóder-infó** (a host regisztrálta),
  **kliens-képességek**, **felbontás-egyeztetés** (2560×1440 is próbálva), **desktop vs. Big
  Picture mód**.

Következő ötlet: a host HID-eszköz-felderítését kiváltani (`IHS_SessionHIDNotifyDeviceChange` a
kapcsolódás után), illetve összevetni egy **ismerten működő klienssel** (pl. Steam Link telefonon):
ha a host oda sem küld videót, a hiba a host oldalán van; ha igen, nálunk hiányzik egy lépés.

## 8. kör: további kizárások a videóindítás körül

A host `Streaming` állapotban van, a kódoló fut (`Dropped N video frames` — nincs fogyasztó), de
**sem `SetTargetFramerate`-et, sem `StartVideoData`-t nem küld**. Az alábbiakat próbáltam ki, mind
hatástalan:

- **referencia-kliens összevetése**: az ihslib saját mintapéldánya input-callbackeket regisztrál
  (`setCursor`, `cursorImage`, `activity`), ezek viszont csak értesítések — nem kapuk. A Locomo
  (működő ihslib-alapú kliens) forrása a streamkérésnél szintén nem állít fps-t.
- **`k_EStreamControlVideoDecoderInfo`** a kapcsolódás után → a host naplózza is
  (`Client video decoder set to …`), a `console_log.txt` szerint megkapta.
- **`k_EStreamControlSetStreamingClientConfig`** a kliens felől → a host **feldolgozza** (a
  részleges üzenetre visszaállította a bitrátát 15000-re, ezért a végleges változat minden mezőt
  ismétel), de videót nem indít.
- **HID-eszközértesítés** a kapcsolódás után (`IHS_SessionHIDNotifyDeviceChange`).
- **felbontás-egyeztetés** (2560×1440, a host capture-méretével egyezően), **desktop vs. Big
  Picture** mód, **VDF rendszerinfó** (a `ClientSystemInfo` KeyValues-hiba megszűnt).

Kizárva bizonyítékkal: csomagvesztés (a „hiányzó" azonosítók a `SetIcon` töredékei), titkosítás,
fps (javítva), dekóder-infó, kliens-képességek, MTU-felderítés (lefut, 1468).

Natív Windows-fordító nincs a gépen, ezért a „ugyanaz a kliens natívan" összehasonlítás nem
járható. A döntő kísérlet így egy **ismerten működő klienssel** való összevetés (pl. Steam Link
telefonon): ha a host oda sem küld videót, a hiba a host oldalán van.

## 9. kör: a videóindítás tényleges kiváltója — a host saját naplója döntött

Nem tippeltem: a PC-n lévő **`C:\Program Files (x86)\Steam\logs\streaming_log.txt`** tartalmaz egy
működő klienst (Steam Link Android, `SEI Robotics SmartBox`, `Samsung SM-S938B`) **és** a mi tévés
próbáinkat is, ezért a kettő közvetlenül összevethető. A naplóban a hozzánk kapcsolódó kliens sorai
`CLIENT: ` előtaggal jelennek meg (a kliens a saját naplóját a **stats csatornán** küldi a hostnak),
a host saját sorai pedig `>>> ` előtaggal.

Amit a napló **bizonyít** (nem következtetés):

| Mérés | Eredmény |
|---|---|
| `StartVideoData` előfordulás | 48, ebből 38 `StopVideoData` után, 10 a munkamenet első videóindítása |
| Minden `StartVideoData` előtt 1 sorral | a kliens `... hardware decoding` sora, azaz **friss `VideoDecoderInfo`** |
| `>>> Client video decoder set to …` sorok | munkamenetenként **pontosan 1** (7/7, 1/1, 2/2), miközben a kliens 28/29/12 alkalommal küldte |
| Működő munkamenet sorrendje | `SetCaptureSize` → `Capture method set to … shared …` → **94–136 ms** → `Client video decoder set to` → `StartVideoData` (+34 µs) |
| A mi munkameneteink sorrendje | `Streaming started` → **+68 ms** `Client video decoder set to` → **+47 ms** `Switching video stream` → **+173 ms** `Capture method set to …` |

Vagyis a host a `VideoDecoderInfo` üzenetre válaszol `StartVideoData`-val, de **csak akkor, ha a
capture/encoder pipeline már áll**. A valódi kliens azért ér oda jókor, mert a saját hardveres
dekódere ~400 ms alatt épül fel (a `SetCaptureSize` után ~405 ms-mal jelentkezik). A mi kliensünk
68 ms-mal a stream indulása után jelentkezett — **173 ms-mal a pipeline előtt** —, a host eltárolta a
nevet (ezért látszik a SessionStats `DecoderDescriptionID` mezőjében), naplózta egyszer, és soha nem
indított videót. A napló szerint ugyanez a név ismételt küldése nem vált ki újabb naplósort, ezért a
korábbi 500 ms-os ismétlés hatástalan volt.

### Változtatások

- **A dekóder-bejelentés késleltetve.** A `VideoDecoderInfo` már nem a kapcsolódás pillanatában megy
  ki, hanem amikor a host jelzi, hogy elindította a saját streamjét (`SetCaptureSize` →
  `TVHostStreamStarted()`), plusz 450 ms — ez a valódi kliens ~405 ms-os késleltetését követi. Az
  ismétlések 500 ms-onként mennek, és a név minden próbánál változik (`… (retry N)`), így akkor is
  újrapróbál a host, ha csak a megváltozott névre indít videót. Biztonsági háló: ha a host 2500 ms-on
  belül nem jelez capture-méretet, a bejelentés akkor is kimegy.
- **A kliens naplója mostantól a PC-n olvasható.** Új `IHS_SessionSendLogMessage()` (stats csatorna,
  `k_EStreamStatsLogMessage` + `CLogMsg`), és a probe minden diagnosztikai sorát átküldi a hostnak
  (`TV: …` előtaggal, 20 ms-onként, munkamenetenként 600 sorig). A host ezeket a
  `streaming_log.txt`-be írja `CLIENT: ` előtaggal — pontosan úgy, ahogy a hivatalos kliensek teszik.
  Ez a **debug mód és a fotózás nélkül** olvasható napló. Az első négy sor `TV[type=N]:` előtaggal
  megy ki, hogy kiderüljön, a `CLogMsg.type` melyik értékére ír a host.
- A csomag a tévére feltelepítve (`install completed`), a WGT minden fájlja bájtra egyezik a
  forrással. **Az appot nem indítottam el.**

### Ami a következő tévés futásból látszani fog

A `streaming_log.txt`-ben a `CLIENT: TV: …` sorok megmutatják a kliens saját naplóját: hány
`VideoDecoderInfo` ment ki és mikor, megérkezett-e a `StartVideoData`, és mi az utolsó lépés a
videócsatorna megnyitása előtt. Ezzel a kérdés egy futásból eldől.

## 10. kör: a 01:00-es tévés futás — két elmélet megdőlt, új mérés

Az új build naplója már a PC-n olvasható (`CLIENT: TV: …` sorok a `streaming_log.txt`-ben), ezért a
futás minden részlete megvan. A 2026-10-14 01:00:54-i munkamenet (10223–10433. sor):

- A kliens **56 `VideoDecoderInfo`** üzenetet küldött, és a host **mindegyiket naplózta**
  (`>>> Client video decoder set to …`). Ezért a „host csak a névváltozásra indít videót” elmélet
  **megdőlt**.
- Az első bejelentés a host `Capture method set to … shared NV12` sora után **789 ms-mal** ment ki,
  tehát a pipeline már állt. A „túl korai bejelentés” elmélet is **megdőlt**.
- A host `StartVideoData`-t ettől függetlenül **nem küldött**, viszont `Dropped N video frames`
  sorokat igen. Ez a sor a **teljes** naplóban kizárólag a mi kliensünknél fordul elő (2026-10-13/14),
  egyetlen működő munkamenetben sem: a host kódol, de nincs fogyasztója.
- A `SteamUI.dll`-ben megkerestem és **visszafejtettem** a `>>> Client video decoder set to %s`
  előállító függvényt (RVA `0x188060`): az csak összehasonlítja a tárolt névvel, naplóz, eltárolja a
  nevet és a szálszámot, majd visszatér — **videót nem indít**. A működő naplókban látott
  „bejelentés → StartVideoData” együttállás tehát véletlen egybeesés volt, nem ok-okozat.

Ami viszont gyanús maradt: a host az első csomagáradat (id ≤ 17) után **semmit nem látszik küldeni**
a kliensnek, pedig a saját naplója szerint többször hívott `SynchronizeClientState()`-et (kurzor).
Ha a host megbízható küldési ablaka elakadt, akkor a `StartVideoData` soha nem tud kijutni.

### Az új build mérése

- **Vezérlőcsatorna-életjel:** 2 másodpercenként naplózza, hány vezérlőcsomag érkezett a hosttól
  (`Vezérlőcsatorna: N csomag az elmúlt 2 s-ban.`). Ez egyértelműen megmondja, hogy a host
  elhallgat-e.
- **Veszteségmentes naplózás:** minden naplósor közvetlenül a stats csatornán megy a hosthoz
  (a gyűrűpuffer csak a tévé kijelzőjére dolgozik), ezért nem vesznek el a lényeges sorok.
- A dekóder-bejelentés száma 8-ra korlátozva, a szöveg állandó (mint a valódi klienseknél).
- A `quality` most **Beautiful** (a host naplója szerint minden működő munkamenet ezt használta,
  a miénk volt az egyetlen `Fast`), a `supported_colorspaces` és a `decoder_limit` pedig kivéve:
  az előbbi miatt a host `BT.709 full range`-et választott, működő munkamenetben `BT.601 limited`
  szerepel.

A csomag feltelepítve (`install completed`), a natív és JS tesztek zöldek. Az appot nem indítottam el.

## 11. kör: a 10:48-as futás és a kezelőfelület javításai

A 10:48:46-i munkamenet (10443–10512. sor) **4 másodperc után a kliens saját cancel kérésével**
zárult (`remote_connections.txt`: kérés 10:48:38 → hét másodpercig `StreamingInProgress` → siker
10:48:45 → cancel 10:48:50). Addig a host naplója **azonos** egy működő munkamenetével:

- `Window title set to Steam Nagy Kép mód` (ez most jelent meg, a korábbi futásokból hiányzott),
- `>>> Capture method set to Desktop DWM shared NT NV12 + NVENC H264`,
- `>>> Capture colorspace set to BT.601 limited range` — a színteret már nem hirdetjük, ezért a host
  ugyanazt választja, mint a működő munkamenetekben,
- a kliens első `VideoDecoderInfo`-ja 1,6 ms-mal a pipeline elkészülte **után** ment ki,
- `StartVideoData` négy másodperc alatt nem érkezett, `Dropped video frames` viszont **nem is volt**
  (frissen indított Steam mellett).

Négy másodperc túl rövid ahhoz, hogy a videóindításról bármit kijelentsünk; a következő futásnak
**30 másodpercig érintetlenül** kell futnia.

### Javított kezelőfelület (a jelzett hibák)

1. **A fókusz elrablása megszűnt.** A `host()` minden felderítési jelentésnél a „Big Picture
   indítása" gombra ugrott, ezért lehetetlen volt a felbontás/FPS gombokat használni. Mostantól
   **csak az első** felderítéskor áll a fókusz a start gombra; a JS-teszt ellenőrzi.
2. **A kontroller vezérli az app felületét.** Eddig csak a távirányító `keydown` eseményei
   mozgatták a fókuszt, ezért a tévéhez csatlakoztatott pad nem tudott semmit állítani. Új
   `pollGamepadUi()`: D-pad, bal analóg kar, A (=OK) és B (=Vissza) ugyanazt a fókuszgyűrűt és
   ugyanazokat a gombokat vezérli; stream közben **nem** nyúl a padhoz, mert ott a játék kapja.
3. **A „streaming próba" felirat eltűnt** (a fejléc most „Steam TV"), és a `StreamingInProgress`
   válaszra érthető állapotszöveg jelenik meg („A PC még az előző munkamenetet zárja le;
   újrapróbálom…") a félrevezető párosítási szöveg helyett.
4. **Minden naplósor eljut a hosthoz** (`push_diagnostic` az egyetlen útvonal, újrabelépés elleni
   védelemmel), így a 2 másodperces életjel-sor is látszik majd a PC-n.

A csomag feltelepítve (`install completed`), natív és JS teszt zöld. Az appot nem indítottam el.

## 12. kör: MEGTALÁLT GYÖKÉROK — a vételi szál meghalt az első médiahívásban

A 10:56:26-i, **28 másodperces** munkamenet (10517. sortól) hozta el a döntő mérést. A kliens saját
életjel-naplója:

```
[574.093193] Él: Vezérlőcsatorna: 29 csomag az elmúlt 2 s-ban.
[576.101385] Él: Vezérlőcsatorna: 0 csomag az elmúlt 2 s-ban.
[578.158919] Él: Vezérlőcsatorna: 0 csomag …
   … a munkamenet hátralévő 24 másodpercében minden ablak 0
```

A host közben **küldött**: `SynchronizeClientState(): setting cursor to 65539` (576.33) és `65567`
(578.16). Egy működő munkamenetben ezek **mindig** megjelennek a kliens naplójában
(`Got control packet k_EStreamControlSetCursor`, 1:1 megfelelés a Sep 16-i futásban). Nálunk
egyetlen sem érkezett meg.

### Az ok

A médiaréteg `start` visszahívásai (audió és videó) **a munkamenet socket-vételi szálán** futnak, és
a `Prepare()` ezt hívta:

```c
MAIN_THREAD_EM_ASM({ TVLink.awaitPicture(); });
```

Ezen a tévén viszont a **könyvtári szálból indított `MAIN_THREAD_EM_ASM` soha nem tér vissza** (ezt
már a kézfogásnál megmértük). A vételi szál tehát az első médiahíváskor elhalt: az utolsó klienssorok
pontosan ott vannak (`Activity: SetActivity` 572.439179, majd egy újraküldött SetIcon-töredék
572.439201), utána 24 másodperc csend. Mivel a vételi szál küldi az ACK-okat is, a host megbízható
küldési ablaka megtelt, ezért a `StartVideoData` **soha nem tudott kijutni**. Ugyanez a hibaosztály
a `Status()` is, amelyet az audió adatút (`AudioSubmit`, `OnAppendError`) hív — egyetlen hibás
hangcsomag is megölte volna a vételi szálat.

### Javítás

- `Status()` → `TVStatus(text)`: csak gyűrűpufferbe ír (a fő ciklus írja ki a képernyőre és küldi a
  hostnak), JavaScriptet nem hív.
- `Prepare()` → `TVRequestAwaitPicture()`: atomi jelző; a `TVLink.awaitPicture()` hívást a **fő
  ciklus** végzi a böngésző szálon.
- **D-pad irány javítva:** eddig mind a négy irány előrefelé lépett a fókuszgyűrűben (ezért ment
  „mindkét irányban csak egyfelé"); most jobb/le előre, bal/fel hátra. JS-teszt ellenőrzi.

A csomag feltelepítve (`install completed`), natív és JS teszt zöld. Az appot nem indítottam el.

## 13. kör: a 11:06-os futás és a második blokkoló hívás

A 11:06:15-i, 28 másodperces futás ugyanazt mutatta:

```
[1162.43] Él: Vezérlőcsatorna: 50 csomag az elmúlt 2 s-ban.
[1164.41] Él: Vezérlőcsatorna: 0 csomag az elmúlt 2 s-ban.   ← és utána 24 másodpercig 0
```

A host közben `SynchronizeClientState(): setting cursor to 65539`-et naplózott (1164.68) — a mi
kliensünk nem kapta meg. Az első javítás (Status/awaitPicture) tehát nem volt elég, mert **két másik
blokkoló pont** maradt ugyanazon a szálon:

1. **`Prepare()` a vételi szálon.** Az audió/videó `start` visszahívás a vételi szálon fut, és a
   `Prepare()` akár **5 másodpercig** vár a téve videódekóderére (`player->Wait(videoListener.open)`)
   — ez pontosan a mért elakadás hossza. Mostantól a `start` visszahívások csak jelzőt állítanak
   (`TVRequestMediaPrepare()`), magát a pipeline-t a **fő ciklus** nyitja meg (`TVMediaPrepare()`).
2. **Az ACK csak a üzenetkezelők után ment ki.** A vételi szál az ACK-ot a dispatcher után küldte,
   így egy lassú kezelő alatt a host megbízható ablaka megtelt. Mostantól minden megbízható csomag
   **azonnal**, a kezelők előtt nyugtázódik (a watermark-ACK változatlanul megmarad).

Emellett a D-pad irány is javítva (jobb/le előre, bal/fel hátra), és a fókusz többé nem ugrik a start
gombra. A csomag feltelepítve, a tesztek zöldek.

## 14. kör: a telefon működik → a hiba nálunk van; a gyökérök az adatcsatorna condvarja

A felhasználó telefonján (Steam Link app) **ugyanazon a hoston, ugyanabban a percben működött a stream**
(host napló 11:32:59, `Samsung SM-S938B Android 17`): a `StartVideoData` a capture-kész után **174 ms-mal**
megérkezett. Vagyis a host jó, a hiba a saját kliensünkben van.

A mérésekből összeállt a lánc:

- A kliens **minden munkamenetben ~2 másodperc után elhallgat** (11:31: 26 csomag az első 2 s-ban, utána
  **0** a következő 28 másodpercben), miközben a host **tíz további üzenetet** próbált küldeni
  (`SynchronizeClientState(): setting cursor …` 2691.8–2715.1), és a küldésünk/fő ciklusunk él.
- A `crc=1` jelző mindkét irányban ugyanaz, tehát az ACK-formátum nem hibás (a host ACK-jai és a mieink
  is CRC-vel, 4 bájtos törzzsel mennek).
- Az elhallgatás **pontosan az audió adatcsatorna indulásakor** kezdődik (`Channel: Adding channel 3`,
  2687.5159) — vagyis amikor az első adatcsatorna-csomag megérkezik.

### A gyökérök

`ch_data.c`: az adatcsatorna munkaszála `IHS_CondWait`-tel várt a `windowLock`-on, a **vételi szál**
pedig minden megérkező adatcsomagnál `IHS_CondSignal`-t hívott. Ezen a tévén a **SDL/pthread
feltételváltozók nem működnek** (ugyanez a hiba fagyasztotta le korábban a küldő szálat, amit 5 ms-os
pollozásra kellett cserélni): a várakozó szál egy elromlott futex-waitben parkol, és a jelzés rá
**megakasztja a hívót** — azt a szálat, amelyik a UDP socketet birtokolja. Ezért áll meg a vétel
örökre, miközben a küldés (más szálakon) és a fő ciklus tovább él.

### Javítás

- A `DataThreadWorker` már **2 ms-onként polloz** a condvar helyett (a `windowLock`-ot a várakozás
  idejére elengedi).
- A vételi út (`IHS_SessionChannelDataReceived`) **nem hív `IHS_CondSignal`-t**, és a
  `DataThreadInterrupt` sem (a munkás úgyis 2 ms-on belül észreveszi a megszakítást).
- Korábbról: `BaseWorker` nem lép ki néma hiba esetén; a média-előkészítés a fő cikluson fut;
  minden megbízható csomag azonnal nyugtázódik; `TVLink`-hívás könyvtári szálból sehol.

### Doksi-kutatás (Samsung + Emscripten) fő pontjai

- A Samsung **nem tiltja** a JavaScript hívást worker-szálból; a saját referencia-portja
  (`SamsungDForum/moonlight-chrome`, `samsung_wasm` ág) `MAIN_THREAD_EM_ASM`-et használ könyvtári
  szálból. Ugyanakkor az Emscripten 1.39.4 `em_asm.h`-ja kimondja: a `MAIN_THREAD_EM_ASM`
  „eltaltatja a hívó szálat, amíg a fő böngészőszál ki nem szolgálja" — és 1.39.4-ben **minden
  syscall (köztük a `printf`/`stdout` írás) szinkron proxy** a fő szál felé, ami parkoltatja a hívót.
- A Samsung API-referencia szerint `select()` helyett **`poll()`** ajánlott, nem-blokkoló socketet
  pedig `SOCK_NONBLOCK`-kal a `socket()` hívásnál kell kérni (`fcntl(F_SETFL)` nem megy).
- Tizen 9.0 specifikációban a `SharedArrayBuffer`/`Atomics` nincs dokumentálva, holott a pthreads
  ezekre épül — ez magyarázza a feltételváltozók megbízhatatlanságát.
- A Samsung doksija szerint az app **háttérbe kerülésekor a JavaScript végrehajtás szünetel** — ez
  nálunk nem áll fenn, de érdemes figyelni (`visibilitychange`).

## 15. kör: „megáll a kép" — a blokkoló `sendto` a munkamenet-zár alatt fagyasztja az egész klienst

A 15:42:08–15:44:32 közötti futás (**előző napló, 7935–8291. sor**) még rendben ment: 1080p60,
60 kocka/s bemenet, 38–52 megjelenített kocka, 2 perc 24 másodpercig. Utána **minden további
munkamenet 0,5–1 másodperc után teljesen elhallgat** (15:45:16, 15:45:44, 15:46:11, 15:48:25,
15:49:47, 16:05:57).

A bizonyítékok (mind a host `streaming_log.txt`-jéből):

- A 16:05:57-es futás: `Data: Starting Video channel` 19144.251, `Videó-dekóder mód: szoftveres
  (1920x1080, 0 PPS)` 19144.783, `Videó: 0 kocka/s … readyState 0, paused 0` 19144.783,
  `Coming keyframe` 19144.920 — ezután **14 másodpercig egyetlen kliens-sor sem**, pedig a fázis
  ekkor 11 (< 12), tehát a naplózást csak az 5 ms-os rés korlátozza. A host közben végig kódolt
  (`Dropped 1 video frames` 16:06:04, `Encoding complete` 16:06:14).
- A 2 másodperces liveness sor (`Él: Vezérlőcsatorna: …`) a 13:53-as és 14:03-as futásban
  rendszeresen megjelent, a 15:42-esben még egyszer, a **bukott munkamenetekben egyszer sem**:
  nem csak a napló, hanem a **fő ciklus** is megállt.
- Az első videó-append a jó futásban 1–6 ms (`dekódolás átlag 2.0 ms`), a bukottakban 13–62 ms —
  vagyis a megosztott médiapipeline már az első kockánál lassult.

### A gyökérök

`IHS_BaseSend` a **`base->lock` alatt hív blokkoló `sendto`-t**, az `IHS_BaseLog` pedig ugyanezt a
zárat kéri. Ha a socket küldési puffere megtelik, a `sendto` várakozik, a zár a küldő szálnál marad,
és ezzel **a vételi munkás, a videó- és hang-adatcsatorna munkaszála, valamint minden naplózó szál
ugyanazon a záron áll** — a kliens elhallgat és a kép megfagy, miközben a host tovább küld. Ez
egyúttal a korábbi „a vételi út 2 másodperc után meghal" jelenségek közös magyarázata is.

A puffert a kliens saját maga töltötte fel: a fázis 11 alatt (első kép előtt) a napló-továbbítás
**korlátlan** volt az 5 ms-os résen kívül, így egy mozgalmas kézfogás **100+ naplócsomagot küldött
másodpercenként** felfelé, miközben a tévé Wi-Fi-je lefelé 20–30 Mbit/s videót vett.

Rontó tényező: a 15:42-ben bevezetett **audió PLC** minden kihagyott id-re akár 8 darab szintetikus
20 ms-os Opus-csomagot küldött a közös médiapipeline-ba, amitől a hang idővonala előreszaladt, és az
első videó-append 13–62 ms-ra lassult. A bukott buildekben ez a változtatás benne volt, az utolsó jó
buildben nem.

### Javítások (2026-10-14 16:2x)

- `ihs_udp_posix.c`: a küldés **`MSG_DONTWAIT`**-tel megy (ha a platform elutasítja a flaget:
  visszaesés flag nélkülire), `SO_SNDTIMEO` 50 ms és 1 MB `SO_SNDBUF` biztosíték; a sikertelen
  küldéseket számoljuk (`IHS_UDPSocketSendFailuresGlobal` → `IHS_BaseFailedSendCount`), és a 2 s-os
  liveness sor kiírja (`eldobva +N`).
- `base.c`: az `IHS_BaseLog` **nem vesz zárat** (a log-függvény a szálak indulása előtt egyszer
  állítódik be), így egy lassú küldés nem némítja el az összes szálat.
- `ch_data_audio.c`: a **PLC szintézis törölve** — csak a ténylegesen megérkezett csomagok mennek a
  dekóderbe, a kihagyott id-k számlálódnak és naplózódnak.
- `probe.c`: a napló-továbbítás **minden fázisban** korlátozott (11 ms bizonyíték-soroknak, 40 ms
  egyébnek), a JS-tükrözés a fő ciklusból 40 ms-ra fogva; `DIAGNOSTIC_LINE` 192 → 320 (a
  statisztika-sor eddig némán csonkult); a megosztott `TVVideoClockState` C→JS mezőkkel bővült
  (főciklus-kurzus, videó/hang számlálók, eldobott küldések), és a JS `TVLink.watchdog()` 2
  másodpercenként jelenti ezeket a host felé (`TVLogBuffer`/`TVLogFlush`) — akkor is, amikor a C
  szálak már nem tudnak írni.
- `media.cpp`: `TVMediaFailed()` `try_to_lock`-kal dolgozik (a fő ciklus sosem vár a média-zárra),
  a hang hibák **számlálódnak** a csomagonkénti naplózás helyett, a NULL/üres hangpuffer nem megy a
  dekóderbe, és az 1 Hz-es statisztika sor tartalmazza a hang számlálóit
  (`hang N csomag/s, M hiba, K vissza, PTS X s`).
- `app.js`: a watchdog újraindítja a lejátszást, ha a video-elem stream közben `paused`; ~6 s
  mozdulatlan főciklus után kiírja a képernyőre, hogy lefagyott, és a **Vissza gomb ilyenkor kilép**
  az appból (a megfagyott fő ciklus a stop-kérést már nem olvassa).

A JS-tesztek (`test.mjs`) a watchdogra is tartalmaznak állításokat, a natív tesztek változatlanul
zöldek. Ami a következő futásból kiderül: ha a kép megint megáll, a watchdog sora megmondja, hogy a
főciklus állt-e meg, mennyi volt a hang hibák/elutasítások száma, és hány felső irányú csomagot
dobott el a kernel.

## 16. kör: a kép jó, a hang akad — a videó-idővonal hibája és a bemeneti út

A 16:29-es futás (a 15. kör javításaival) **stabil képet** adott: `readyState 4, paused 0`,
51–68 kocka/s be, 0.5–3 ms dekódolás, 30–58 megjelenített kocka/s, ~95 másodpercig. A hang viszont
nagyon akadozott, és a kontroller is.

### Amit a napló bizonyít (auditok + saját mérés)

- **Az audió-út a kliens oldalán hibátlan.** A `hang …` farok minden sorában `0 hiba, 0 vissza`; a
  9858 hozzáfűzött csomag 98.58 s audiót ad, azaz **pontosan 10.00 ms/frame (480 minta, 48 kHz)**,
  100.02 csomag/s, `audioPts`/falióra = **0.9993** — sem alul-, sem túlcsordulás, a tévé sem utasítja
  vissza. (A korábbi „183 → 9858 csomag/s" ijesztő szám csupán a **kumulatív** számláló volt `/s`
  címkével; a következő build ezt másodperces értékként írja.)
- **A videó-idővonal viszont lassabban járt, mint a valós idő.** A host 60 fps cél helyett átlag
  **56.7 kocka/s**-ot adott, a kliens pedig kockánként fix `1/60`-dal léptette a PTS-t → a
  kép-idővonal **0.95×**, az audió **1.00×** ütemben haladt, és a kettő 92 s alatt **5.8 s-ra**
  szakadt szét (a napló `óra`/`audioPts` értékei: 92.8 s vs 98.6 s).
- Ez azért döntő, mert az EMSS `kUltraLow`/`kLow` módban **az alkalmazás felel a stream
  szinkronizációért**, és az elem óráját a **beadott PTS-ekből** számolja (SDK: `emss_modes.h`). Az
  elszakadó idővonalat a platform folyamatos audió-korrekcióval (kihagyással) követi — ez hallatszik
  akadozásként, miközben minden audió-csomag hibátlanul dekódolódik.
- **A kontroller-út:** a `pollGamepads` szignatúra-kapuja nem tartalmazta az **analóg ravasz** értékét
  és a 18. gombot, így azok befagytak (csak a press-küszöbnél ugrottak); a tengelyek 1%-ra voltak
  kvantálva. Két független 8 ms-os óra (böngésző-mintavevő + könyvtári HID-poll) és az 5 ms-os
  send-worker poll ~20 ms elkerülhető késleltetést adott.
- **A telemetria csapdája:** a stream közbeni naplózásnál egyetlen `host_log_last_ms` volt a
  30 s-os „szívverés" és a mérések 900 ms-os kapuja mögött, és mivel a `Média: Videó` sor ~1/s jött,
  a 30 s-os kapu **soha nem teljesült**: sem az `Él:` csatorna-jelentés, sem a `HID:` sorok, sem a JS
  watchdog `Őr:` sorai nem érkeztek meg. Ezért nem lehetett eldönteni, torlódik-e a bemeneti sor.

### Javítások (2026-10-14 16:5x)

- **Videó-idővonal:** a PTS mostantól a host **kocka-időbélyegeiből** számol
  (`IHS_SessionVideoFrameTimestamp`, 1/65536 s egységek), nem kockánkénti `1/60`-ból; az első kocka a
  hang aktuális pozíciójára indul, hogy a két sáv ne legyen eltolva. Érvénytelen időbélyeg esetén
  visszaesik a régi viselkedésre. A statisztika-sor új mezője (`kép-PTS`) ezt a következő futásban
  ellenőrizhetővé teszi.
- **Audió:** a hangút **kikerült a videó zára alól** (a videó szinkron dekódhívásai 1–3 ms-ra,
  néha 20 ms+-ra tartották a zárat 60×/s, és a hang 100×/s ugyanerre a zárra várt); két 10 ms-os PCM
  frame-enként egy platform-hívás (100 → 50 hívás/s), a `pcmAccum` a stream végén eldobva; a
  szinkron `AppendPacket` marad (az `AppendPacketAsync` a fejléc ajánlása ellenére sem bizonyított ezen
  a tévén). Új mérés: `írás X ms (max Y)` + `N írás/s`.
- **Kontroller:** a szignatúra tartalmazza a ravasz analóg értékét és a 18. gombot, a tengelyek
  1/1000 pontossággal; a böngésző-mintavevő 4 ms, a könyvtári HID-poll 4 ms, a send-worker poll 2 ms;
  a watchdog sorban megjelenik a mintavevő tényleges üteme (`pad Ndb/max M ms`).
- **Telemetria:** külön kapu a méréseknek (900 ms) és a szívverésnek (30 s), a `Média: Videó`, `Él:`,
  `HID:` és `Őr:` sorok mérésnek számítanak; az `Él:` sor mostantól a HID-számlálókat is hozza
  (`feladva/elküldve/nyugtázva/függőben/repülésben`), így a bemeneti sor torlódása mérhető.
- **Ikon:** új 512×512 `icon.png` (kontroller + „STEAM TV" felirat), az app neve `Steam TV`.

### Utólag visszavonva (2026-10-14 17:2x): a videó-idővonal-átírás nem maradt bent

Négy futás összevetése eldöntötte: **a 16:29-es futás** (idővonal-átírás nélkül) 92 másodpercig
100.02 audió-frame/s-ot adott, 0 hibával és 0 visszautasítással; **a 16:57-es, 17:20-as és 17:23-as
futás** (mind az átírt idővonallal) elvesztette a hangot — a 16:57-esben 46 frame után megállt a
hangút (a kép jó maradt), a 17:23-asban már a videó is lefagyott egy kocka után, mert a hang a
`configMutex` alatt akadt meg, és magával vitte a videót is. Ezért az idővonal-átírás (host
kocka-időbélyegek + a hanghoz igazított kezdés) **visszaállítva** a régi `videoPts += 1/fps`-re; az
`IHS_SessionVideoFrameTimestamp` a könyvtárban maradt egy későbbi, önálló kísérlethez. A build így a
16:29-es, bizonyítottan működő média-úttal fut, a diagnosztika (kép-PTS, hang-számlálók, `Őr:`/`Él:`/
`HID:` sorok), a bemeneti javítások és az ikon megmaradtak.

### Ami a következő futásból kiderülHa a hang még mindig akad: a `kép-PTS` és az `óra` együtt 1.0×-ot kell mutasson (ha nem, az
időbélyeg-egység más, és a visszaesés miatt a régi viselkedés él). A hang oldalán az `írás` átlag/max
mutatja, hogy a platform-hívás belefér-e a 20 ms-os ütembe. A kontrollernél az `Él:` sor
`Bemenet: … függőben, repülésben` számlálói döntik el, hogy a megbízható csatorna torlódik-e
(rubber-band), és a `pad Ndb/max M ms` a böngésző-szál késését.

## Források

- IHSlib: https://github.com/mariotaku/ihslib
- Locomo: https://github.com/beebono/locomo
- Samsung socket API: https://developer.samsung.com/smarttv/develop/extension-libraries/webassembly/tizen-sockets-extension/tizen-sockets-extension.html
- Samsung WASM streaming portolás: https://developer.samsung.com/smarttv/develop/extension-libraries/webassembly/game-streaming-with-wasm.html
- Valve Remote Play: https://store.steampowered.com/remoteplay
