#!/bin/sh

for f in edge_impulse/Person_detection_inferencing_cpp/test/*.jpg; do
  echo "=== $f ==="
  ./edge_impulse/Person_detection_inferencing_cpp/build/person_detection_eval "$f"
  echo
done
