Target: esp32-s3-cam-setup

Kurzbeschreibung:
Setup-Target für die Erstkonfiguration des Geräts. Dieses Build startet das Gerät im Access-Point-/Setup-Modus und ermöglicht manuelle Kameratrigger und Konfiguration ohne aktive Personenerkennung.

Wichtige Build-Flags (aus platformio.ini):
- `-DAPP_SETUP=1`
- `-DAPP_WIFI_IS_AP=1`
- `-DAPP_PERSON_DETECT_ENABLED=0`
- `-DAPP_CAMERA_TRIGGER_FRAMES=1`

Anwendung / Nutzung:
- Build: `pio run -e esp32-s3-cam-setup`
- Upload: `pio run -e esp32-s3-cam-setup -t upload`

Hinweis:
Verwenden Sie dieses Target für die erstmalige Einrichtung (z. B. Wi‑Fi-Konfiguration als AP). Weitere Konfigurationen sind in `platformio.ini` und den `sdkconfig`-Dateien zu finden.

Setup-AP Zugangsdaten (Default):
- SSID: GooUuuu-CAM
- Passwort: goouuuu123
- IP-Adresse: 192.168.4.1

Diese Default-Werte können per Build-Flags in `platformio.ini` überschrieben werden. Beispiel:

```ini
build_flags =
	-DAPP_SETUP=1
	-DAPP_WIFI_IS_AP=1
	-DAPP_WIFI_AP_SSID="MeineSetupSSID"
	-DAPP_WIFI_AP_PASSWORD="MeinPasswort"
	-DAPP_WIFI_AP_IP_ADDR="192.168.10.1"
```
