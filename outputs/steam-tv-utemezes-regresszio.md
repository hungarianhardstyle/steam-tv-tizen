# Ütemezési regresszió visszavonása – 2026. október 15.

A 10:09-es kézi próba alapján a 100 ms-os, submit callbackekbe tett ütemezés regressziót okozott: körülbelül 100 helyett 10 audio frame/s és 50 helyett 5–7 append/s jelent meg, majd a videó 362 beadott képkockánál megállt. A főciklus és a gamepad mintavétel tovább futott; a sikeres helyi tesztek nem igazolták a teljes valós idejű út teljesítményét.

A kódaudit megállapította, hogy a médiazár feloldása ellenére az IHS videóoldali `stateMutex` a callback teljes ideje alatt fogva maradt. Az adatworkerek csak a callback visszatérése után szabadítják fel az aktuális keretet és pollolják a következőt. A protokoll fogadóablaka minden poll előtt eldobja a túl régi csomagokat; ez nem alkalmas lejátszási puffernek. A 10 packet/s pontos mechanizmusát a régi számlálók nem bizonyítják, de a blokkoló callback és a hibás pufferelési hely konkrét hiba.

Az ütemezési várakozás, annak órája és kizárólagos helpertesztjei kikerültek. A submit callbackek ismét közvetlenül dolgoznak, többlet 100 ms-os várakozás nélkül. A fölösleges PCM-másolat is megszűnt. A leállítás külön szála, a visszalépést kezelő felület, a kontroller állapotának atomikus átadása, a közös senderóra és a 20 ms-os PCM összevonás megmaradt. A leállítás kezdetén külön atomikus jelző tiltja az új média-beadásokat; a lezárás továbbra is csak a régi workerek bevárása után történik.

A folyamatos lejátszás későbbi puffereléséhez a callbacknek saját memóriába másolva, sorba helyezve azonnal vissza kell térnie. Külön renderer kezelheti az órát; ezt a jelenlegi javítás nem állítja késznek. A cél továbbra is stabil 60 FPS és folyamatos hang, nem pusztán a regresszió visszavonása.

A natív és JS tesztek sikeresek, köztük a kontroller, PCM mintasorrend/összevonás és aszinkron cleanup regresszió. A hardveres kép/hang helyreállítása még nincs az új csomaggal igazolva; az appot nem indítjuk el automatikusan.

A végső teljes build sikeres. A tévé a friss csomag telepítését `install completed` válasszal megerősítette. Nem indítottam el az appot. Az aláírt csomag másolata `outputs/SteamTV.wgt`.
