# Steam TV: diagnózis és célzott mérés, 2026. október 15.

Az alkalmazás hibrid: a Tizen webapp HTML/JavaScript felületet és C/C++-ból fordított WebAssembly streamingmagot használ. Az útvonal Steam/NVENC → UDP/IHS fogadóablak → teljes tömörített videókeret → `AppendPacketAsync()` → Samsung EMSS hardveres dekóder → HTML-videóelem. A videó nem JavaScriptben dekódolódik, nincs canvasos képkockamásolás. A hang Opus → WASM PCM-dekódolás → EMSS audio track; a kontroller külön HID útvonalon jut a Steamhez.

## A javítás előtti mérés

A helyi Steam-napló 11:05:28–11:07:26 közötti, 98 kliens mérési sorának összesítése: `steam-tv-meres-elotte.json`. A vizsgált beállítás 1920×1080/60, NVENC H.264, 19 906 kbit/s felső bitrátahatár, hardveres tévés dekóder, UltraLow mód.

| Jel | Eredmény | Mit bizonyít / mi a korlátja? |
|---|---|---|
| Videó beadási sebessége | 19–68 frame/s a jelentett ablakokban | Az átlag nem igazolja a 16,67 ms-os egyenletességet; indítás/alkalmazásváltás is van a mintában. |
| Jelentett megjelenítés | 19–75 frame/s | A korábbi számláló 250 ms-os memóriatükrözése torzíthatta az egy másodperces ablakokat. Nem panel-Hz mérés. |
| Videó beadási szakasz maximuma | 69 ms | A régi időmérés a konfigurációs zár visszavételére várást is tartalmazta. Nem tiszta SDK- vagy dekódolási idő. |
| Hangbeadási szakasz maximuma | 105,1 ms | Ugyanez a mérési korlát. Az elfogadott PCM önmagában nem igazolja a folyamatos hangkimenetet. |
| Hiányzó audio frame számláló | 19 → 36 | Hálózati hiányt és kliensoldali eldobást is számolhatott. Az első mérési sor már 19-et tartalmazott. |
| Audio dekóderhiba / visszautasítás | 0 / 0 | A hangakadás nem járt ezeknek a hibaszámlálóknak a növekedésével. |
| Steam játék-munkamenet hálózati statisztikája | átlag 14,55 ms, szórás 26,49 ms; SlowNetwork 3,50% | Felhasználói térben felvett vételi időbélyeg, ezért a TV socket-sora és ütemezése is benne lehet. Nem bizonyít kábelhibát. |
| Steam decode/display értékei | 0 | Nem mért nullák: a kliens nem adott tényleges dekódolásvégi és megjelenítési eseményeket. |

## Bizonyított fogadási hiba és javítása

A fogadóablak a `Discard()` hívást a `Poll()` előtt futtatta. Ha a 0. csomag után a 2. érkezett az 1. előtt, átugorhatta az üres 1. pozíciót; a később megérkező 1. ekkor már túl réginek számított. A korábbi 200/450 ms-os sender-időbélyeg határ ezt nem akadályozta meg. Az offline teszt reprodukálja ezt a sorrendhibát.

A javítás helyi monoton órával legfeljebb 20 ms esélyt ad a hiányzó pozíció pótlására. Ha a csomag 10 ms múlva megérkezik, 1 → 2 sorrendben továbbítjuk. Ha tényleg hiányzik, 20 ms után a feldolgozás továbbhalad. A folyamatos, sorrendhelyes útvonal nem kap fix várakozást; nem került új képkockasor vagy 16,67 ms-os pacer a médiába. Hiányos sorozatnál a javítás legfeljebb 20 ms helyi várakozást jelenthet a poll ütemezésén felül; a tényleges input lag hatását még tévén kell mérni.

A teszt lefedi az átrendeződést, a valódi veszteség utáni továbbhaladást további bejövő forgalom nélkül, az új hiány külön határidejét, a ring határát, a csomagazonosító és sender-időbélyeg körbefordulását. A meglévő árva fragmentum tesztek is megmaradtak.

## Az új mérés értelmezése

A natív `Idozites:` sorok külön mérik a teljes keret összeállítása utáni READY pontot, a callback és konfigurációs zár idejét, a tényleges SDK-hívás kezdetét/végét, az egymást követő callbackek és SDK-hívások intervallumát, valamint a visszatérés utáni zárvárást. A READY pont nem a nyers Ethernet-csomag érkezése: a protokollablakban és keret-összeállításban korábban eltöltött időt nem méri. A natív statisztikák átlagot, 1 ms-os hisztogramból számított p95 felső korlátot és pontos megfigyelt maximumot adnak, korlátozott számú sorban.

A PTS/DTS/duration és a frame-ID sorrend ellenőrzése elkülönül az időzítéstől. A jelenlegi DTS a PTS-ből származik, nem önálló host DTS; az egyenlőség nem bizonyítja a B-frame-ek hiányát. Az Annex-B H.264 első VCL slice-fejlécének vizsgálata P/B/I és ismeretlen eredményt jelent. Képenként legfeljebb 4 KiB bevezető adatot és 32 bájt fejlécet olvas; ez mintavételezett bitfolyam-ellenőrzés, nem teljes H.264 kép-sorrend dekódoló. Az ismeretlen adatból nem következtetünk B-frame-mentességre. A callback és append számlálók az ablakhatáron eltérhetnek, ha egy SDK-hívás közben történik a statisztikai ablak váltása.

A `Render:` sorok a `requestVideoFrameCallback` adataiból számolnak, a 250 ms-os tükrözéstől függetlenül. Jelentik a kumulatív `presentedFrames` alapú FPS-t, az egymást követő megfigyelt kompozitor-prezentációk intervallumának p50/p95/max értékét és célintervallumhoz viszonyított RMS eltérését. Az összevont callbackek külön számlálót kapnak; ezek nem jelentenek bizonyított frame dropot. A `dropAPI` kizárólag a videóelem által közölt eldobásszám, támogatás hiányában `NA`.

A `decode` a böngésző opcionális `processingDuration` mezője, nem az append hívás ideje. A `JS-late` a callback késése az `expectedDisplayTime` értékhez képest; ez nem a kijelző renderelési késése. A `media` a callbackhez tartozó média-idő. Hiányzó adatot nem helyettesítünk nullával.

A menüben az alapból kikapcsolt **Diagnosztika** megjeleníti a prezentációs összegzést és a legutóbb tükrözött natív médiastatisztikát. A részletes natív időzítés a Steam-naplóban olvasható. Képkockánkénti naplózás nincs.

## Támogatás, kijelző és referenciák

A kliens `EmssVersionInfo::Create().has_ultra_low_latency` alapján választ UltraLow módot; nem a modellévből következtet. Az aktív hardveres dekóder és a használt kodek naplózott. Az új csomag az EMSS és kapcsolódó képességjelzőket is kiírja. A cél továbbra is 1080p/60; ebben a javításban nincs felbontás-, bitráta- vagy kijelzőmód-váltás.

A Samsung magyar termékoldalának frissítési adata 50 Hz, a HDMI támogatásnál pedig 4K 60 Hz szerepel. Ebből nem állapítható meg a belső app aktuális megjelenítési üteme. A véges, 120 intervallumos rAF-mérés csak a böngésző kompozitorának becslése, nem közvetlen panel- vagy VSYNC-visszaigazolás; a napló ezért `raf~…Hz panel=NA` formát használ. Nem kérünk nem dokumentált frissítésimód-váltást.

A Samsung által hivatkozott Moonlight WASM forrás EMSS trackeket és HTMLMediaElement kimenetet használ. Opcionális pacingje a tömörített frame beadása előtt aktívan vár, szintetikus PTS-sel. Ezt nem emeltük át. Az Android Moonlight Balanced módja ezzel szemben a dekóder kimeneti puffereit engedi ki Choreographer-időbélyeggel, legfeljebb egy extra képpel. A mostani EMSS MediaElement kimenet nem ad ilyen dekódolt-frame kiengedési API-t. A H.264 P-frame-ek önkényes eldobása a dekódolás előtt sérthetné a referencialáncot.

Ezért nem állítjuk, hogy egy rAF vagy késleltetett append Android Balanced módot valósítana meg. A Samsung textúrakimenete képességellenőrzéshez és új GL megjelenítőhöz kötött; ezt a működő renderer mérés nélküli átírása helyett most kihagytuk.

Források: [Samsung EMSS működési módok](https://developer.samsung.com/smarttv/develop/extension-libraries/webassembly/tizen-wasm-player/tizen-wasm-player-features.html), [Samsung által hivatkozott Moonlight WASM forrás](https://github.com/SamsungDForum/moonlight-chrome/blob/samsung_wasm/wasm/wasmplayer.cpp), [Android Moonlight Balanced implementáció](https://github.com/moonlight-stream/moonlight-android/blob/master/app/src/main/java/com/limelight/binding/video/MediaCodecDecoderRenderer.java), [rVFC mezők meghatározása](https://wicg.github.io/video-rvfc/), [a pontos TV Samsung-adatlapja](https://www.samsung.com/hu/tvs/uhd-4k-tv/u8000f-65-inch-crystal-uhd-4k-smart-tv-ue65u8072fuxxh/).

## Ellenőrzés és visszaállítás

A natív WASM tesztek sikeresek: csomagsorrend/határidő/körbefordulás, párhuzamos audio-video beadás és lezárás, időhisztogramok és H.264 első slice vizsgálat. A JS tesztek ellenőrzik a 60 FPS prezentációs mintát, összevont callbackeket, számláló-nullázódást, hiányzó mérési mezőket, véges 50 Hz-es rAF mintát és a diagnosztika kapcsolását. A teljes build sikeres. A tévé a telepítést `install completed` válasszal visszaigazolta; az appot nem indítottuk el.

Aktuális aláírt csomag: `outputs/SteamTV.wgt`. A közvetlenül előző telepített csomag másolata visszaállításhoz: `work/SteamTV-before-reorder-measurement.wgt`. A médiaátadás ütemezése a csomagsorrend-javításon túl nem változott; a mérési kód nem simítja ki előre a mérendő időközöket.

A javítás utáni valódi érkezési/append/prezentációs és input lag eredményekhez kézi tévés futás kell. Az alkalmazást nem indítjuk el automatikusan. E nélkül sem a jitter kisimításának előnyét, sem a hozzáadott input lagot, sem a stabil 60 FPS-t nem állítjuk igazoltnak.
