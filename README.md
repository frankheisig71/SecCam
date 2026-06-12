# GooUuuu CAM

Mini-ESP-IDF-Projekt fuer ein ESP32-S3-basiertes Kameramodul mit folgenden Zielen:

- alle 60 Sekunden ein JPEG aufnehmen
- immer nur das letzte Bild im RAM halten
- das Bild bei Bedarf per Browser ueber WLAN ausliefern
- zusaetzlich per GPIO oder HTTP eine Sofortaufnahme ausloesen
- im Idle mit 80 MHz laufen und nur waehrend der Aufnahme auf 240 MHz hochtakten
- mit einem kleinen S3-tauglichen Modell abschaetzen, ob wahrscheinlich eine Person im Bild ist

Die Firmware ist fuer PlatformIO mit ESP-IDF aufgebaut.

## Funktionsueberblick

- WLAN-Betrieb wahlweise als eigener Access Point oder als Station in einem bestehenden WLAN
- HTTP-Oberflaeche fuer Status, Bildanzeige und Sofortaufnahme
- periodische Aufnahme im Sekundenbereich
- externer Hardware-Trigger ueber GPIO 21 auf steigender Flanke
- gemeinsame Cooldown-Zeit fuer alle Triggerquellen
- probabilistische Personenerkennung ueber Espressif `pedestrian_detect`
- letztes JPEG ausschliesslich im RAM, kein Filesystem und kein Live-Stream

## Projektstruktur

- `src/main.cpp`: zentrale Orchestrierung, Triggerlogik, Power-Management, Startreihenfolge
- `src/app_camera.cpp`: Kamerainit, Aufnahme und RAM-Pufferverwaltung
- `src/app_wifi_ap.cpp`: SoftAP-Modus
- `src/app_wifi_sta.cpp`: Station-Modus mit Reconnect und IP-Logging
- `src/app_http_server.cpp`: Browser-UI und HTTP-Endpunkte
- `src/app_person_detect.cpp`: Personenerkennung und Statusverwaltung
- `include/app_config.h`: zentrale Defines fuer WLAN, Trigger, CPU-Frequenz und Kamerapins
- `HTTP_API.md`: Endpunkt-Dokumentation

## Voraussetzungen

- ESP32-S3-Board, hier aktuell auf `esp32-s3-devkitc-1-n16r8v` konfiguriert
- ESP32-kompatible Kamera, aktuell mit plausiblen OV2640-Pinannahmen hinterlegt
- PlatformIO mit ESP-IDF-Unterstuetzung
- Internetzugang beim ersten Build, damit PlatformIO und der ESP-IDF Component Manager Abhaengigkeiten laden koennen
- ausreichend Flash/PSRAM fuer Kamera plus Modellkomponente

## Build und Flash

Typischer Ablauf:

```bash
pio run
pio run -t upload
pio device monitor
```

Wichtige Hinweise:

- PlatformIO richtet Toolchain, Framework und Plattform beim ersten Build automatisch ein.
- Die Komponenten sind ueber `src/idf_component.yml` und die eingecheckte `dependencies.lock` auf feste Versionen festgelegt.
- Die Projektbasis ist ueber `platform = espressif32 @ 6.13.0` reproduzierbar gepinnt; das bringt fuer ESP-IDF die getestete 5.5.3-Paketlinie mit.
- Die Kamera-Komponente wird ueber `src/idf_component.yml` automatisch geladen.
- Das `pedestrian_detect`-Modell wird ebenfalls ueber den Component Manager eingebunden.
- `src/idf_component.yml` verlangt mindestens ESP-IDF 5.5.3, damit aeltere, inkompatible Setups frueh scheitern.
- `sdkconfig.defaults` aktiviert das noetige ESP-IDF Power Management.

## Konfiguration

Die wichtigsten Schalter liegen in `include/app_config.h`.

### WLAN-Modus

```cpp
#define APP_WIFI_MODE_AP 1
#define APP_WIFI_MODE_STA 2
#define APP_WIFI_MODE APP_WIFI_MODE_AP
```

- `APP_WIFI_MODE_AP`: das Geraet oeffnet ein eigenes WLAN
- `APP_WIFI_MODE_STA`: das Geraet verbindet sich mit einem vorhandenen WLAN

### Access Point

```cpp
#define APP_WIFI_AP_SSID "GooUuuu-CAM"
#define APP_WIFI_AP_PASSWORD "goouuuu123"
```

Im AP-Modus ist die Weboberflaeche normalerweise unter `http://192.168.4.1/` erreichbar.

### Station-Modus

```cpp
#define APP_WIFI_STA_SSID "DummyExistingWifi"
#define APP_WIFI_STA_PASSWORD "DummyExistingPassword"
```

Im STA-Modus wird die per DHCP erhaltene IP-Adresse beim Verbinden ins Log geschrieben.

### Trigger und Capture-Timing

```cpp
#define APP_CAPTURE_INTERVAL_MS (60 * 1000)
#define APP_CAPTURE_TRIGGER_GPIO GPIO_NUM_21
#define APP_CAPTURE_TRIGGER_COOLDOWN_MS (5 * 1000)
#define APP_CAPTURE_TRIGGER_POLL_MS 50
```

- alle 60 Sekunden wird automatisch ein neues Bild aufgenommen
- GPIO 21 loest bei LOW-zu-HIGH ebenfalls eine Aufnahme aus
- nach jeder Aufnahme gilt eine gemeinsame Sperrzeit von 5 Sekunden
- Polling ist bewusst einfach gehalten, weil die Triggerung hier im Sekundenfenster liegt

### CPU-Takt fuer Stromsparen

```cpp
#define APP_CPU_FREQ_IDLE_MHZ 80
#define APP_CPU_FREQ_ACTIVE_MHZ 240
```

Die Firmware laeuft im Idle mit niedrigerem Takt. Fuer die Dauer einer Aufnahme wird ein CPU-Frequenz-Lock gesetzt, danach faellt das System wieder auf den Idle-Bereich zurueck.

### Personenerkennung

```cpp
#define APP_PERSON_DETECT_ENABLED 1
#define APP_PERSON_DETECT_SCORE_THRESHOLD 0.70f
```

- aktiviert eine probabilistische `Person ja/nein`-Aussage fuer jedes neu aufgenommene Bild
- `APP_PERSON_DETECT_SCORE_THRESHOLD` bestimmt, ab welcher Modellwahrscheinlichkeit `person_present=true` gesetzt wird
- die Erkennung ist bewusst als Wahrscheinlichkeitsaussage gedacht, nicht als harte, sichere Klassifikation

### Kameraannahmen

Die Pinbelegung und Kameraeinstellungen sind aktuell bewusst als plausible Annahmen hinterlegt. Vor echtem Einsatz sollten insbesondere diese Defines gegen die reale Hardware geprueft werden:

- Daten- und Sync-Pins `CAM_PIN_*`
- `APP_CAMERA_FRAME_SIZE_PSRAM`
- `APP_CAMERA_FRAME_SIZE_NO_PSRAM`
- `APP_CAMERA_JPEG_QUALITY`

## Laufzeitverhalten

Beim Start passiert Folgendes:

1. NVS wird initialisiert.
2. Die Kamera wird initialisiert.
3. Das Power Management wird fuer 80/240 MHz eingerichtet.
4. Direkt beim Start wird ein erstes Bild aufgenommen.
   Dabei wird zugleich die Personenerkennung auf diesem Bild ausgefuehrt.
5. WLAN wird im gewaehlten Modus gestartet.
6. Der HTTP-Server wird gestartet.
7. Eine Supervisor-Task ueberwacht Zeittrigger und GPIO-Trigger.

Alle Triggerquellen laufen ueber denselben zentralen Capture-Pfad in `main.cpp`.
Dadurch gelten Mutex, Cooldown und CPU-Taktumschaltung fuer Start, Timer, GPIO und HTTP identisch.
Der Capture-Pfad fuehrt zuerst die Personenerkennung auf dem RGB565-Frame aus und speichert danach ein JPEG fuer den Browser im RAM.

## HTTP-Schnittstelle

Kurzuebersicht:

- `GET /`: Browser-Oberflaeche
- `GET /status`: Status, Bildmetadaten und Personenerkennungs-Ergebnis
- `GET /image.jpg`: letztes JPEG aus dem RAM
- `POST /capture`: Sofortaufnahme anfordern

Details und Beispielantworten stehen in `HTTP_API.md`.

## Architekturhinweise

- Das Projekt speichert bewusst nur genau ein Bild im RAM.
- Es gibt absichtlich keinen MJPEG-Stream und keine Dateiablage.
- Die Capture-Logik ist zentralisiert, damit Triggerquellen nicht auseinanderlaufen.
- Die HTTP-Schicht bleibt duenn und delegiert Sofortaufnahmen ueber Callback an die Hauptlogik.
- Fuer die KI wird direkt auf dem aufgenommenen RGB565-Frame inferiert; das Browser-Bild wird anschliessend separat als JPEG erzeugt.
- Die Personenerkennung ist auf `Person wahrscheinlich vorhanden ja/nein` reduziert, um den ESP32-S3 nicht mit unnoetig breiten Klassifikationsaufgaben zu ueberladen.

## Offene Annahmen

Vor echtem Hardwareeinsatz sollten diese Punkte geprueft werden:

- passt die Kamerapinbelegung zur realen Platine
- passt das Board-Profil in `platformio.ini`
- ist GPIO 21 wirklich frei und elektrisch passend beschaltet
- sind SSID und Passwort fuer den Zielbetrieb korrekt gesetzt
- ist die gewaehlte JPEG-Aufloesung fuer den verfuegbaren RAM sinnvoll
