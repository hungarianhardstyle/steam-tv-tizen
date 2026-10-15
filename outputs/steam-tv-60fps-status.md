# SteamTV stabil 60 FPS cél – 2026. október 14.

A cél aktív: 1080p/60 továbbítás és megjelenítés, folyamatos hang, akadásmentes kontroller; csak Steam a PC-n. A teljesítéshez hardveres visszaigazolás kell, a sikeres fordítás önmagában nem elég.

## Régi Samsung Steam Link

A [2017. június 12-i bejelentés](https://steamcommunity.com/app/353380/discussions/0/1353742967821054749/) a 2016–2017-es Samsung UHD tévéken már a béta idején 1080p/60 FPS lejátszást jelölt meg. Jó minőségű, legalább 5 GHz-es hálózatot kért, a vezetékes kapcsolatot kifejezetten ajánlotta. A [Samsung 2017. augusztusi közleménye](https://news.samsung.com/global/samsung-electronics-brings-steam-link-game-app-for-smart-tv-users) a 2017-es modellek 4K streamtámogatását is megerősíti; ez a közlemény nem ad konkrét 4K FPS-garanciát. Ezek támogatott képességek, nem minden hálózatra és játékra szóló akadásmentességi garanciák.

A jelenlegi Samsung app megszűnésénél a [Steam támogatás](https://help.steampowered.com/de/faqs/view/175C-DDCE-F0CF-B3BD) a Samsung által már nem támogatott pluginra hivatkozik. A régi, Samsung által karbantartott kliens működése nem bizonyítja automatikusan a mostani WASM/EMSS megvalósítás teljesítményét. A fenti források nem közölnek részletes TV-CPU/RAM követelményt vagy a régi kliens belső hangpuffer-méretét; ezekre nincs bizonyított specifikáció.

## Legutóbbi bizonyíték és javítás

A felhasználó szerint a PC és tévé is Ethernet-kábelen csatlakozik. A 21:52-es naplóban a kép/hang PTS már együtt haladt, de 9775 audio callbackből 8959 jutott appendig. A számlálók nem különítették el a még zárt tracket az időbélyeg miatti eldobástól. Egyedi frame ID-k is rendelkezhetnek azonos vagy kissé hátráló timestampbel; ez önmagában nem jogosítja fel a klienst a hangminták eldobására.

A friss javítás a valódi duplikált/régi frame ID-ket dobja el a dekódolás előtt. Folyamatos ID-sorozatnál minden PCM-minta megmarad és a minták időtartama lépteti a hangot; valódi ID-kimaradáskor a hangóra előre igazodik a közös senderórához. Nincs mesterséges pótló PCM-csomagáradat.

A stream közbeni csomagonkénti Control/HID trace és Debug naplózás kikerült a diagnosztikai gyűrűből. A kontrollcsomagok számlálása, figyelmeztetések és hibák megmaradtak. A képernyőre naplót tükröző szinkron böngészőhívás a stream alatt legfeljebb 2/s. A tényleges videó-FPS mérése a böngésző `presentedFrames` metaadatának különbségét használja; a kihagyott callbackek többé nem látszanak hamis képkockavesztésnek.

A natív tesztek ellenőrzik a frame ID duplikációját és körbefordulását, száz eltérő/ismétlődő timestampű egyedi hangcsomag összes mintájának megőrzését, valamint az ID-kimaradás időbeli rését. A JS teszt ellenőrzi a kihagyott videócallbackek és az új streamben visszaálló számláló helyes mérését. A diagnosztikai teszt igazolja, hogy a folyamatos packet trace kimarad, míg a figyelmeztetés és a médiaállapot megmarad.

A cél még nincs hardveren igazolva. A hangcsomag-vesztést, a valódi kijelzett FPS-t és a kontroller késését a frissített kliens következő kézi futásából kell értékelni. A meglévő körülbelül 9 órás egybefüggő streamkorlát változatlan.

A teljes build, a natív WASM tesztek és a JS tesztek sikeresek. A tévé az új csomagot `install completed` válasszal telepítette. Nem indítottam el. Az aláírt csomag másolata `outputs/SteamTV.wgt`. Az app elindítása nélküli 12 ICMP-próba 0–3 ms választ és 0% veszteséget mutatott; ez nem stream alatti UDP-terhelésmérés.
