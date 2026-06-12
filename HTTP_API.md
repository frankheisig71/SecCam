# GooUuuu CAM HTTP API

Die Firmware stellt einen kleinen HTTP-Server bereit.

Basisadresse:

- AP-Modus: normalerweise `http://192.168.4.1/`
- STA-Modus: die per DHCP erhaltene IP wird beim Verbindungsaufbau ins Log geschrieben

## Endpunkte

### `GET /`

Liefert die einfache Browser-Oberflaeche fuer Status, Sofortaufnahme und Bildanzeige.

Antwort:

- `200 OK`
- Content-Type: `text/html`

### `GET /status`

Liefert Metadaten zum aktuell im RAM gehaltenen Bild.

Antwort:

- `200 OK`
- Content-Type: `application/json`

Beispiel:

```json
{
  "has_image": true,
  "size": 58231,
  "captured_at_ms": 1718012345678,
  "person_ready": true,
  "person_present": true,
  "person_score": 0.842,
  "person_analyzed_at_ms": 1718012345600,
  "person_inference_ms": 118
}
```

Bedeutung:

- `has_image`: Ob bereits ein Bild im RAM liegt
- `size`: JPEG-Groesse in Byte
- `captured_at_ms`: Unix-Zeitstempel in Millisekunden
- `person_ready`: Ob bereits eine Auswertung fuer das letzte Bild vorliegt
- `person_present`: Schaetzung, ob mit ausreichender Wahrscheinlichkeit eine Person im Bild ist
- `person_score`: hoechste vom Modell gelieferte Personenwahrscheinlichkeit
- `person_analyzed_at_ms`: Zeitstempel der letzten Auswertung
- `person_inference_ms`: Dauer der letzten Inferenz in Millisekunden
- `person_motion_score`: Anteil geaenderter Rasterzellen im Vergleich zur Referenz
- `person_motion_ratio_threshold`: aktuell konfigurierte Schaltschwelle fuer Bewegung
- `person_motion_cell_diff_threshold`: Helligkeitsdifferenz pro Rasterzelle, ab der eine Zelle als geaendert gilt
- `person_reference_updated_at_ms`: Zeitstempel des letzten Referenzbilds

### `GET /reference`

Liefert das stark heruntergerechnete Referenzbild als Graustufenraster.

Antwort:

- `200 OK`
- Content-Type: `application/json`

### `GET /motion-debug`

Liefert aktuelles Raster, Referenzraster, absolutes Differenzraster und die daraus erzeugte Schwellwert-Maske.

Antwort:

- `200 OK`, wenn ein aktuelles Bild vorliegt
- `503 Service Unavailable`, wenn noch kein Bild zum Debuggen vorliegt

### `GET /image.jpg`

Liefert das aktuell im RAM gehaltene JPEG.

Antwort:

- `200 OK`, Content-Type `image/jpeg`, wenn ein Bild vorhanden ist
- `503 Service Unavailable`, wenn noch kein Bild aufgenommen wurde

Hinweis:

- Es wird bewusst immer nur das letzte Bild aus dem RAM geliefert, kein Live-Stream.

### `POST /capture`

Fordert sofort eine neue Aufnahme an. Der Endpunkt nutzt dieselbe zentrale Capture-Logik wie Start, Zeit-Trigger und GPIO-Trigger.

Antwort bei Erfolg:

- `200 OK`
- Content-Type: `application/json`

```json
{
  "ok": true,
  "message": "New image captured"
}
```

Antwort bei Fehler oder wenn gerade kein Capture moeglich ist:

- `503 Service Unavailable`
- Content-Type: `application/json`

```json
{
  "ok": false,
  "message": "Capture failed: ..."
}
```

## Verhalten

- Die Kamera haelt immer nur das letzte Bild im RAM.
- Periodisch wird alle 60 Sekunden ein neues Bild aufgenommen.
- Wenn mindestens 10 Sekunden keine Bewegung am Trigger anlag, wird ein neues Referenzbild fuer den Motion-Vergleich aufgenommen.
- Ein LOW-zu-HIGH-Wechsel an GPIO 21 kann ebenfalls eine neue Aufnahme ausloesen.
- Nach einem Trigger gilt eine gemeinsame Cooldown-Zeit von 5 Sekunden.
- Waehren der Aufnahme wird die CPU auf 240 MHz angehoben und danach wieder auf 80 MHz freigegeben.
- Das Bild wird intern fuer Motion-Debug und Inferenz nach RGB dekodiert und anschliessend als JPEG fuer den Browser im RAM gehalten.
- Die Personenerkennung ist eine Wahrscheinlichkeitsaussage und keine harte, garantierte Klassifikation.
