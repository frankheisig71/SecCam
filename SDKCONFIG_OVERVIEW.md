# SDKConfig Overview

Die Dateien `sdkconfig.*` sind von ESP-IDF generierte Vollkonfigurationen. Sie sind absichtlich umfangreich, weil sie nicht nur die von uns gesetzten Optionen enthalten, sondern auch alle aufgeloesten Abhaengigkeiten und SoC-/Board-Defaults.

## Schichten

| Ebene | Datei | Inhalt |
| --- | --- | --- |
| Gemeinsame Basis | `sdkconfig.defaults` | Werte, die fuer alle Targets gelten sollen |
| Target-Overrides | `platformio.ini` | build flags und pro Env gesetzte Schalter |
| Generiertes Ergebnis | `sdkconfig.*` | komplette, von ESP-IDF erzeugte Endkonfiguration |

## Wichtige Unterschiede

### `esp32-s3-cam-edge-impulse`

- Personenerkennung mit Edge Impulse Backend
- Startet ohne Capture beim Boot
- ansonsten naehert es sich dem Standard-CAM-Betrieb

### `esp32-s3-cam-capture`

- STA statt AP
- HTTP-Server aus
- Personenerkennung aus
- Dataset-Collector an
- Kamera-Frames fuer Upload auf VGA reduziert

### `esp32-s3-cam-setup`

- AP-Modus
- Setup-UI aktiv
- HTTP-Server aktiv
- Personenerkennung aus
- Dataset-Collector aus
- Kamerabild alle 500 ms aktualisieren
- AP-IP auf `192.168.125.1`
- IR-LED per Button schaltbar

## Wo die Unterschiede stehen

- Gemeinsame, stabile Default-Werte: `sdkconfig.defaults`
- Ziel-spezifische Schalter: `platformio.ini`
- Das grosse `sdkconfig.*` ist nur das generierte Resultat und ist nicht der beste Ort, um Unterschiede zu suchen.
