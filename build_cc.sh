#!/bin/sh

cd edge_impulse/Person_detection_inferencing_cpp
cmake -S . -B build
cmake --build build
cd ../..
