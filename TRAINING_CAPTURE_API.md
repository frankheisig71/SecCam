# Training Capture API

Das Collector-Target `esp32-s3-cam-edge-impulse-capture` sendet jedes JPEG direkt per HTTP POST an einen Dienst auf dem Pi.

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

Ein einfacher Receiver kann daraus Dateinamen wie diese erzeugen:

- `goouuuu-cam_motion_1718200000123456_s1of3.jpg`
- `goouuuu-cam_idle_1718200900123456_s1of1.jpg`

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