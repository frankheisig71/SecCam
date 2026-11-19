Target: esp32-s3-cam-capture

Kurzbeschreibung:
Dieses Target baut eine Firmware, die auf Bildaufnahme und Datensammlung ausgerichtet ist. Die Personenerkennung ist deaktiviert, dafür ist der Dataset-Collector aktiviert, um Bilder für Trainingsdaten zu sammeln.

Wichtige Build-Flags (aus platformio.ini):
- `-DAPP_CAPTURE=1`
- `-DAPP_PERSON_DETECT_ENABLED=0`
- `-DAPP_DATASET_COLLECTOR_ENABLED=1`
- `-DAPP_CAMERA_FRAME_SIZE_PSRAM=FRAMESIZE_VGA` (bei PSRAM)

Anwendung / Nutzung:
- Build: `pio run -e esp32-s3-cam-capture`
- Upload: `pio run -e esp32-s3-cam-capture -t upload`

Upload-Taktung:
- Im normalen Idle-Betrieb werden Bilder in einem festen Intervall von 15 Minuten an den Collector gesendet.
- Bei Bewegung (PIR-Trigger) startet ein kurzer Burst mit mehreren Aufnahmen; die Einzelbilder werden im Bereich von etwa 1 Sekunde Abstand hochgeladen, bis der Burst abgeschlossen ist.
- Die eigentliche Übertragung erfolgt als HTTP-POST an die in `APP_DATASET_COLLECTOR_UPLOAD_URL` konfigurierte URL (`http://192.168.178.149:8080/api/v1/captures`).

Hinweis:
Eignet sich, um Bilddaten für Trainings- oder Debug-Zwecke zu sammeln. Einstellungen für PSRAM und Partitionen sind in `platformio.ini` konfiguriert.
