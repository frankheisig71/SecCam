# Edge Impulse Linux C++ evaluation for person detection

This directory contains a small standalone test harness to run an exported Edge Impulse model on Linux (for example WSL), independent of the ESP32 firmware pipeline.

## What this tool does

- reads input images in PGM (P5, grayscale) or PPM (P6, RGB)
- resizes to the model input size from the export metadata
- runs inference and prints class confidences

## Expected model directory

Default model root:

- ../Person_detection_inferencing

Use a custom model root if needed:

```bash
cmake -S . -B build -DEI_MODEL_ROOT=/path/to/your/export
cmake --build build
```

## Quickstart

```bash
cd edge_impulse/Person_detection_inferencing_cpp
cmake -S . -B build
cmake --build build
./build/person_detection_eval test/input_160_gray.pgm
```

## Input preparation

Install Pillow once if needed:

```bash
python3 -m pip install pillow
```

### Single grayscale test image (crop 160x160)

```bash
python3 - <<'PY'
from pathlib import Path
from PIL import Image

src = Path('test/input.jpg')
dst = Path('test/input_160_gray.pgm')

with Image.open(src) as img:
    gray = img.convert('L')
    w, h = gray.size
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    crop = gray.crop((left, top, left + side, top + side))
    crop_160 = crop.resize((160, 160), Image.Resampling.BILINEAR)

dst.parent.mkdir(parents=True, exist_ok=True)
with dst.open('wb') as f:
    f.write(b'P5\n160 160\n255\n')
    f.write(crop_160.tobytes())
print(dst)
PY
```

### Synthetic black grayscale image (160x160)

```bash
python3 - <<'PY'
from pathlib import Path

w, h = 160, 160
pixels = bytearray([0] * (w * h))

path = Path('test/input_160_gray.pgm')
path.parent.mkdir(parents=True, exist_ok=True)
with path.open('wb') as f:
    f.write(f'P5\n{w} {h}\n255\n'.encode('ascii'))
    f.write(pixels)
print(path)
PY
```

### Batch conversion from project test JPGs

Gray (output .pgm):

```bash
cd /home/frank/projects/PlatformIO/GooUuu-CAM
. .venv/bin/activate

python - <<'PY'
from pathlib import Path
from PIL import Image

src = Path('/home/frank/projects/PlatformIO/GooUuu-CAM/test')
out = Path('/home/frank/projects/PlatformIO/GooUuu-CAM/edge_impulse/Person_detection_inferencing_cpp/test')
square_size = 160
out.mkdir(exist_ok=True)

for f in sorted(src.glob('*.jpg')):
    with Image.open(f) as img:
        gray = img.convert('L')
        w, h = gray.size
        side = min(w, h)
        left = (w - side) // 2
        top = (h - side) // 2
        crop = gray.crop((left, top, left + side, top + side))
        resized = crop.resize((square_size, square_size), Image.Resampling.BILINEAR)
        target = out / (f.stem + '.pgm')
        resized.save(target)
        print(f'created: {target}')
PY
```

Color (output .ppm):

```bash
cd /home/frank/projects/PlatformIO/GooUuu-CAM
. .venv/bin/activate

python - <<'PY'
from pathlib import Path
from PIL import Image

src = Path('/home/frank/projects/PlatformIO/GooUuu-CAM/test')
out = Path('/home/frank/projects/PlatformIO/GooUuu-CAM/edge_impulse/Person_detection_inferencing_cpp/test')
square_size = 160
out.mkdir(exist_ok=True)

for f in sorted(src.glob('*.jpg')):
    with Image.open(f) as img:
        w, h = img.size
        side = min(w, h)
        left = (w - side) // 2
        top = (h - side) // 2
        crop = img.crop((left, top, left + side, top + side))
        resized = crop.resize((square_size, square_size), Image.Resampling.BILINEAR)
        target = out / (f.stem + '.ppm')
        resized.save(target)
        print(f'created: {target}')
PY
```

## Running evaluations

Single image:

```bash
./build/person_detection_eval test/input_160_gray.pgm
```

Batch over grayscale test files:

```bash
cd /home/frank/projects/PlatformIO/GooUuu-CAM/edge_impulse/Person_detection_inferencing_cpp
for f in test/*.pgm; do
  echo "=== $f ==="
  ./build/person_detection_eval "$f"
  echo
done
```

## Model notes

Training setup (latest reference):

- transfer learning
- 160x160 px
- cycles: 30
- learning rate: 0.001
- data augmentation: yes

### person-detection-cpp-linux-g160x160-impulse-float

- latency: ~11s per analysis

|           | no_person | person |
|-----------|-----------|--------|
| no_person | 100%      | 0%     |
| person    | 5.9%      | 94.1%  |
| F1 score  | 0.98      | 0.97   |

### person-detection-cpp-linux-g160x160-impulse-byte

- latency: ~3.8s per analysis

|           | no_person | person |
|-----------|-----------|--------|
| no_person | 100%      | 0%     |
| person    | 8.8%      | 91.2%  |
| F1 score  | 0.96      | 0.95   |

### person-detection-cpp-linux-g96x96-impulse-float

- latency: ~4.3s per analysis

|           | no_person | person |
|-----------|-----------|--------|
| no_person | 100%      | 0%     |
| person    | 2.9%      | 97.1%  |
| F1 score  | 0.99      | 0.99   |

### person-detection-cpp-linux-g96x96-impulse-byte

- latency: ~1.5s per analysis

|           | no_person | person |
|-----------|-----------|--------|
| no_person | 100%      | 0%     |
| person    | 5.9%      | 94.1%  |
| F1 score  | 0.98      | 0.97   |

This project is intended as a convenient Linux-side validation tool before sending only compact preprocessed images over the network from the ESP32.
