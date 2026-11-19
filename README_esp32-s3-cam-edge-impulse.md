Target: esp32-s3-cam-edge-impulse

Kurzbeschreibung:
Firmware mit aktivierter Bildaufnahme und Edge-Impulse-Personenerkennungs-Backend. Dieses Target baut eine Version, die ein auf dem Gerät laufendes Edge-Impulse-Inferenzmodell zur Personenerkennung verwendet.

Wichtige Build-Flags (aus platformio.ini):
- `-DAPP_CAPTURE=1`

Anwendung / Nutzung:
- Build: `pio run -e esp32-s3-cam-edge-impulse`
- Upload: `pio run -e esp32-s3-cam-edge-impulse -t upload`

Modell-Import / echtes Edge-Impulse-SDK:
- `edge_impulse/firmware_esp32.bin` ist ein fertiges Flash-Image und kein C++-SDK. Das echte Modell muss aus Edge Impulse exportiert werden und dann in den Ordner `edge_impulse/edge-impulse-sdk/` kopiert werden.
- Die erwartete Struktur sieht so aus:
  - `edge_impulse/edge-impulse-sdk/classifier/ei_run_classifier.h`
  - `edge_impulse/edge-impulse-sdk/model-parameters/model_metadata.h`
- Sobald diese Dateien vorliegen, nimmt der Code in `src/app_person_detect.cpp` automatisch den echten Modellpfad und verwendet den echten Edge-Impulse-Inferenzaufruf statt des Fallbacks.
- Das Flash-Image kann weiterhin separat mit dem vorhandenen Script aus `edge_impulse/flash_linux.sh` oder dem Upload-Target geflasht werden.

Hinweis:
Konfigurationen wie `sdkconfig`, Partitionslayout, PSRAM und Upload-Parameter sind in `platformio.ini` hinterlegt.
