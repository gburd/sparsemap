#!/bin/sh
# autogen.sh - generate build system files

set -e

echo "Generating build system files..."

# Create m4 directory if it doesn't exist
test -d m4 || mkdir m4

# Run autoreconf
autoreconf --install --force --verbose

echo ""
echo "Build system files generated successfully."
echo "You can now run ./configure && make"
