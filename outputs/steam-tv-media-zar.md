# Egymást feltartó hang- és képbeadás – 2026. október 15.

A felhasználó minden automatikus beállítás mellett már csak enyhe hang- és képakadást jelez. A 10:56–10:59 közötti futás tényleges konfigurációja 1920×1080, 60 FPS, NVENC H.264, hardveres tévés dekódolás, 19 906 kbit/s felső bitrátahatár. Az automatikus bitrátát a kliens a felbontásból és képfrissítésből számolja; nem a tévé 4K paneljére állítja a streamet.

A mérési sorokban nulla a hiányzó audio frame, a dekóderhiba és a visszautasítás. A hang platformnak történő beadása viszont néha 26,3 ms-ig tartott. A kódban ugyanaz a globális zár védte a hang és a kép beadását, ezért ennyi ideig a másik médiaút is állhatott. Ez konkrét blokkolási lehetőség; az összes maradék akadás okát önmagában nem bizonyítja.

A két track most külön beadási zárat használ. A konfigurációs zár csak a platformhívás idejére szabadul fel; az aktuális Player erős referenciája életben tartja a csomaghoz tartozó objektumot. Lezárás előtt mindkét beadást megvárjuk. A platformhívás visszatérésekor a track-zár felszabadítása megelőzi a konfigurációs zár visszavételét, így a lezárás nem okoz zársorrendi holtpontot. Elavult Player vagy megkezdett leállítás esetén nem frissítjük a lejátszó állapotát.

A tényleges közös append helperrel futó WASM regressziós teszt blokkolt hangbeadás mellett befejezi a videó beadását, ellenőrzi a lezárás várakozását, majd a szálak befejeződését és az objektum élettartamát. A natív tesztcsomag és a JS felületi tesztek sikeresek.

A host 10:58:08–10:58:13 között 7–10 FPS körüli videót is küldött; a Steam ugyanitt a játék 47–153 ms-os képkockaidejét jelölte lassulási okként. Ezt a kliens nem tudja valódi 60 különálló képkockává alakítani. Az enyhe, folyamatos recegés és ez az átmeneti játékoldali megtorpanás nem azonosított ugyanazon hibaként.

Nem változtattuk a felbontást, FPS-t, bitrátát, kodeket vagy a késleltetési módot. Nem került új időzítési várakozás a fogadó callbackekbe. A tévés egyenletesség és késleltetés kézi ellenőrzése még hátravan.

A teljes build sikeres, a tévé a telepítést `install completed` válasszal megerősítette. Az appot nem indítottuk el. Az aktuális csomag `outputs/SteamTV.wgt`; a korábbi csomag visszaállításhoz `work/SteamTV-before-independent-appends.wgt` néven megmaradt.
