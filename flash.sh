#!/bin/bash
docker run --it --rm -v "$PWD":/project -w /project --device "$1" espressif/idf idf.py flash