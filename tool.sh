#!/bin/bash
docker run -it --rm -v "$PWD":/project -w /project espressif/idf idf.py "$2"