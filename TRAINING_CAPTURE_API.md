# Training Capture API

Das Collector-Target `esp32-s3-cam-capture` sendet jedes JPEG direkt per HTTP POST an einen Dienst auf dem Pi.

## Endpoint

- `POST /api/v1/captures`
- Content-Type: `image/jpeg`

## Request-Header

- `X-Device-Id`: feste Kamera-ID, z. B. `goouuuu-cam`
- `X-Capture-Mode`: `motion` oder `idle`
- `X-Capture-Reason`: z. B. `pir_motion` oder `idle_interval`
- `X-Captured-At-Us`: Capture-Zeitstempel vom ESP32 in Mikrosekunden
- `X-Image-Width`: Bildbreite
- `X-Image-Height`: Bildhoehe
- `X-Sequence-Index`: laufende Nummer innerhalb einer Bewegungsserie
- `X-Sequence-Size`: maximale Serienlaenge, aktuell `3`

## Erfolgsantwort

- `200 OK`, `201 Created`, `202 Accepted` oder `204 No Content`

## Dateinamen auf dem Pi

Der Receiver setzt den Dateinamen serverseitig aus der Pi-Zeit, damit die Ablage nicht von der ESP-Uhr abhaengt.

- `CAM_250612_143015_123.jpg`
- `CAM_250612_143017_041.jpg`

Das Format ist `CAM_YYMMDD_HHMMSS_MMM.jpg`.

## Curl-Test

```bash
curl -X POST \
  -H "Content-Type: image/jpeg" \
  -H "X-Device-Id: goouuuu-cam" \
  -H "X-Capture-Mode: motion" \
  -H "X-Capture-Reason: pir_motion" \
  -H "X-Captured-At-Us: 1718200000123456" \
  -H "X-Image-Width: 640" \
  -H "X-Image-Height: 480" \
  -H "X-Sequence-Index: 1" \
  -H "X-Sequence-Size: 3" \
  --data-binary @frame.jpg \
  http://raspberrypi.local:8080/api/v1/captures
```