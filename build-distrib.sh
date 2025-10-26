#!/bin/bash
# Create portable distribution package of PrusaSlicer

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BINARY="$BUILD_DIR/src/prusa-slicer"

if [ ! -f "$BINARY" ]; then
    echo "Error: Binary not found at $BINARY"
    echo "Please run ./build-docker.sh first"
    exit 1
fi

VERSION=$(grep "set(SLIC3R_VERSION" "$SCRIPT_DIR/version.inc" | sed 's/.*"\(.*\)".*/\1/' || echo "dev")
DISTRIB_NAME="prusa-slicer-${VERSION}-linux-x64"
DISTRIB_DIR="$SCRIPT_DIR/dist/$DISTRIB_NAME"

echo "Creating distribution package: $DISTRIB_NAME"

rm -rf "$SCRIPT_DIR/dist"
mkdir -p "$DISTRIB_DIR"

echo "[1/4] Copying binary..."
mkdir -p "$DISTRIB_DIR/bin"
cp "$BINARY" "$DISTRIB_DIR/bin/prusa-slicer"

echo "[2/4] Copying resources..."
cp -r "$SCRIPT_DIR/resources" "$DISTRIB_DIR/"

echo "[3/4] Creating launcher script..."
cat > "$DISTRIB_DIR/prusa-slicer.sh" << 'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/bin/prusa-slicer" "$@"
EOF
chmod +x "$DISTRIB_DIR/prusa-slicer.sh"

ln -s bin/prusa-slicer "$DISTRIB_DIR/prusa-slicer"
cat > "$DISTRIB_DIR/README.txt" << EOF
PrusaSlicer Portable Distribution
==================================

To run PrusaSlicer:
  ./prusa-slicer.sh

Or:
  ./prusa-slicer

Or from bin directory:
  ./bin/prusa-slicer

The binary expects to find the 'resources' directory in the same location.

System Requirements:
- Linux x86_64
- GTK+ 3.0
- OpenGL support

Built on: $(date)
Build host: $(uname -n)
EOF

echo "[4/4] Creating tarball..."
cd "$SCRIPT_DIR/dist"
tar -I pigz -cf "${DISTRIB_NAME}.tar.gz" "$DISTRIB_NAME"

echo ""
echo "Distribution package created:"
echo "  $SCRIPT_DIR/dist/${DISTRIB_NAME}.tar.gz"
echo ""
echo "Package size: $(du -h "$SCRIPT_DIR/dist/${DISTRIB_NAME}.tar.gz" | cut -f1)"
