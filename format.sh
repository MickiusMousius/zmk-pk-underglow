#!/bin/bash
# A simple script to run clang-format over the zmk-pk-underglow source code

# Get the directory of this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"

echo "Formatting source files in $SCRIPT_DIR/src and $SCRIPT_DIR/include..."

# Find all .c and .h files in src/ and include/ and format them in-place
find "$SCRIPT_DIR/src" "$SCRIPT_DIR/include" -type f \( -name "*.c" -o -name "*.h" \) -exec clang-format --style="{IndentWidth: 4, ColumnLimit: 120, MaxEmptyLinesToKeep: 2}" -i {} +

echo "Formatting complete!"
