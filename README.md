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
- probabilistische Personenerkennung ueber Edge-Impulse-Modell bzw. optionales ESP32-Modell
- letztes JPEG ausschliesslich im RAM, kein Filesystem und kein Live-Stream

## Projektstruktur

- `src/main.cpp`: zentrale Orchestrierung, Supervisor-/Triggerlogik, Guardzeiten, Burst- und Idle-Capture-Flow, Power-Management
- `src/app_camera.cpp`: Kamerainit, Aufnahme, JPEG-Erzeugung und RAM-Pufferverwaltung
- `src/app_capture_uploader.cpp`: Queue-basierter Upload-Pfad, Analyse-Queue, Upload-Worker, Dateinamen und Retry-Logik
- `include/app_capture_uploader.h`: API fuer Analyse- und Upload-Queue
- `src/app_wifi_ap.cpp`: SoftAP-Modus
- `src/app_wifi_sta.cpp`: Station-Modus mit Reconnect und IP-Logging
- `src/app_http_server.cpp`: Browser-UI und HTTP-Endpunkte
- `src/app_person_detect.cpp`: Modellauswertung, Statusverwaltung, Edge-Impulse-/Person-Scoring und Motion-Referenz
- `include/app_config.h`: zentrale Defines fuer WLAN, Trigger, CPU-Frequenz, Dataset-Collector und Kamerapins
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

Eine kompakte Uebersicht, welche Optionen pro Target wirklich abweichen, steht in [SDKCONFIG_OVERVIEW.md](SDKCONFIG_OVERVIEW.md).

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

Fuer die Objektiv-Einstellung gibt es zusaetzlich das Target `esp32-s3-cam-setup`.
Dort laeuft der AP unter `http://192.168.125.1/`, aktualisiert das Bild alle 500 ms und bietet einen IR-Schalter.

### Build-Targets

- `esp32-s3-cam-edge-impulse`: Analyse-Variante mit Edge-Impulse-Modell
- `esp32-s3-cam-capture`: JPEG-Upload-Variante fuer Trainingsdaten
- `esp32-s3-cam-setup`: AP-Setup fuer Objektivjustage

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

## Bewegungs-, Analyse- und Upload-Ablauf (aktuelle Logik)

Der Triggerpfad ist bewusst als einfache Zustandsmaschine aufgebaut und nicht als GPIO-Interrupt-Handler. In `capture_supervisor_task()` in `src/main.cpp` wird der PIR-Pin mit einer kurzen Poll-Periode (`APP_CAPTURE_TRIGGER_POLL_MS`) abgefragt. Wenn der GPIO-Wert auf HIGH steht und die Guard-/Cooldown-Fenster passen, startet der Motion-/Dataset-Pfad.

Die aktuelle Reihenfolge ist grob:

1. Supervisor erkennt Bewegung am PIR-GPIO und prueft Guard-/Cooldown-Fenster.
2. `perform_motion_dataset_batch_capture()` bzw. `perform_dataset_capture_and_upload()` startet den passenden Capture-Pfad.
3. `main.cpp` nimmt mehrere Bilder in kurzer Folge auf und sammelt sie in einem lokalen Puffer.
4. Das eigentliche Modell-Scoring laeuft nicht mehr inline im Supervisor-Task, sondern wird asynchron ueber eine separate Analyse-Queue abgearbeitet.
5. `app_capture_uploader_enqueue_analysis_image()` kopiert das aktuelle JPEG in einen Queue-Eintrag und stellt es in `app_capture_uploader.cpp` in die `g_analysis_queue`.
6. `analysis_task()` nimmt die Bilder aus der Analyse-Queue, ruft `app_person_detect_analyze_jpeg_image(...)` auf und bestimmt `person_present` sowie `score`.
7. Danach wird nur noch das bereits klassifizierte Bild in die Upload-Queue weitergereicht.
8. Ein separater Upload-Task verarbeitet die Upload-Queue und schickt das JPEG an den Collector mit Retry-/Backoff-Mechanik.
9. Beim Setzen des Dateinamens wird das Klassifikationsergebnis bevorzugt, also z. B. `CAM_person_0.67.jpg` oder `CAM_no_person_0.12.jpg`.

Diese Trennung ist wichtig, damit die asynchronen, rechenintensiven Schritte nicht mehr im Trigger-/Supervisor-Task laufen. Der Supervisor bleibt damit reaktionsschnell und der Watchdog verschiebt die schwere Arbeit in die dedizierte Analyse-Queue.

Wichtig: Das System speichert nicht "alle Bilder als Datei"; der typische Ablauf ist vielmehr: Bewegung erkannt -> kurzer Burst -> Bilder im RAM puffern -> Analyse in eigener Queue -> nur das klassifizierte Bild wird nach Upload weitergereicht. Das macht den Ablauf fuer den ESP32-S3 robust und reduziert RAM/Flash deutlich.

Der Idle-/Dataset-Pfad laeuft parallel dazu. Wenn kein PIR aktiv ist, kann in festen Intervallen ebenfalls ein Bild aufgenommen und via Analyse-/Upload-Queue verarbeitet werden (`APP_DATASET_COLLECTOR_IDLE_INTERVAL_MS`). Dadurch ist die App nicht rein "PIR-only"; sie kann in beiden Modi bei Bedarf Bilder erzeugen.

## HTTP-Schnittstelle

Kurzuebersicht:

- `GET /`: Browser-Oberflaeche
- `GET /status`: Status, Bildmetadaten und Personenerkennungs-Ergebnis
- `GET /image.jpg`: letztes JPEG aus dem RAM
- `POST /capture`: Sofortaufnahme anfordern

Details und Beispielantworten stehen in `HTTP_API.md`.

## Architekturhinweise

- `main.cpp` bleibt der orchestrierende Einstiegspunkt: Trigger, Cooldown, Guardzeiten, Datensammelpfad und Ablaufsteuerung liegen dort zusammen.
- `app_camera.cpp` ist die reine Kamera-/Puffer-Schicht: Init, Capture, JPEG-Conversion und RAM-Verwaltung.
- `app_person_detect.cpp` kapselt die Modell- und Statuslogik: Edge-Impulse-/RGB-Auswertung, Score-Berechnung, Motion-Reference und Statushaltung.
- `app_capture_uploader.cpp` kapselt den I/O-/Transport-Pfad: Analyse-Queue, Upload-Queue, HTTP-POST, Dateinamen und Retry/Backoff.
- Es gibt absichtlich keinen MJPEG-Stream und keine Dateiablage.
- Die HTTP-Schicht bleibt duenn und delegiert Sofortaufnahmen ueber Callback an die Hauptlogik.
- Die schwere Bildanalyse wurde bewusst aus dem Supervisor-Task herausgezogen, damit Trigger-Polling und Camera-Flow stabil bleiben.
- Die Personenerkennung ist auf `Person wahrscheinlich vorhanden ja/nein` reduziert, um den ESP32-S3 nicht mit unnoetig breiten Klassifikationsaufgaben zu ueberladen.

## Offene Annahmen

Vor echtem Hardwareeinsatz sollten diese Punkte geprueft werden:

- passt die Kamerapinbelegung zur realen Platine
- passt das Board-Profil in `platformio.ini`
- ist GPIO 21 wirklich frei und elektrisch passend beschaltet
- sind SSID und Passwort fuer den Zielbetrieb korrekt gesetzt
- ist die gewaehlte JPEG-Aufloesung fuer den verfuegbaren RAM sinnvoll
