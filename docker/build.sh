#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

IMAGE_NAME=ppc-pod-toolchain

if [ ! -f "$PROJECT_DIR/sdk/MacOSX10.5.sdk.tar.gz" ]; then
  echo "Missing $PROJECT_DIR/sdk/MacOSX10.5.sdk.tar.gz - see README.md for how to extract it from the G4." >&2
  exit 1
fi

echo "Building Docker image ($IMAGE_NAME)..."
docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$PROJECT_DIR"

echo "Image built. Compile via docker/ppc-cc, e.g.:"
echo "  docker/ppc-cc -Wall yourfile.c -o yourfile_ppc"
echo "The toolchain is not extracted to the host - it's not relocatable (see Dockerfile"
echo "comment above the final CMD). docker/ppc-cc runs compiles through this image instead."
