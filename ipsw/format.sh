#!/bin/bash
# Format C source files using xcrun clang-format
# Usage: ./format.sh [files...]
# If no files are provided, it formats all .c and .h files in the current directory and subdirectories.

if [ $# -eq 0 ]; then
    # Find all .c and .h files and format them
    find . -type f \( -name "*.c" -o -name "*.h" \) -exec xcrun clang-format -style=file -i {} +
    echo "Formatted all .c and .h files."
else
    # Format specific files provided as arguments
    xcrun clang-format -style=file -i "$@"
    echo "Formatted: $@"
fi
