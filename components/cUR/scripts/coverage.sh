#!/bin/bash
#
# Coverage report script for BC_UR library
# Generates code coverage report for src/ files using gcov/lcov
#

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== BC_UR Code Coverage Report ===${NC}"

# Check for required tools
check_tool() {
    if ! command -v "$1" &> /dev/null; then
        echo -e "${RED}Error: $1 is not installed.${NC}"
        echo "Install with: sudo apt install $2"
        exit 1
    fi
}

check_tool gcov gcc
check_tool lcov lcov

# Clean previous coverage data
echo -e "${YELLOW}Cleaning previous build and coverage data...${NC}"
make clean
rm -rf coverage_html coverage.info
find . -name "*.gcda" -delete
find . -name "*.gcno" -delete

# Coverage build directory
COV_OBJDIR="src/obj_cov"
mkdir -p "$COV_OBJDIR"
mkdir -p "$COV_OBJDIR/sha256"
mkdir -p "$COV_OBJDIR/types"

# Build with coverage flags
echo -e "${YELLOW}Building with coverage instrumentation...${NC}"

CFLAGS="-Wall -Wextra -std=c99 -g -O0 --coverage -fprofile-arcs -ftest-coverage"
INCLUDES="-Isrc"

# Compile all source files with coverage
SOURCES=(
    utils.c bytewords.c fountain_decoder.c fountain_encoder.c
    fountain_utils.c crc32.c ur_decoder.c ur_encoder.c ur.c
    sha256/sha256.c
    types/byte_buffer.c types/cbor_data.c types/cbor_encoder.c types/cbor_decoder.c
    types/registry.c types/bytes_type.c types/psbt.c types/bip39.c
    types/keypath.c types/hd_key.c types/multi_key.c types/output.c
)

for src in "${SOURCES[@]}"; do
    obj="${src%.c}.o"
    echo "  Compiling $src"
    gcc $CFLAGS $INCLUDES -c "src/$src" -o "$COV_OBJDIR/$obj"
done

# Create coverage library
echo -e "${YELLOW}Creating coverage library...${NC}"
ar rcs src/libur_cov.a "$COV_OBJDIR"/*.o "$COV_OBJDIR"/sha256/*.o "$COV_OBJDIR"/types/*.o

# Discover and run all tests
echo -e "${YELLOW}Discovering tests in tests/ folder...${NC}"

# Find all test_*.c files in tests folder (excluding the support objects
# test_utils.c and test_harness.c, which are linked into every test).
TEST_FILES=($(find tests -maxdepth 1 -name "test_*.c" \
    ! -name "test_utils.c" ! -name "test_harness.c" -type f | sort))

if [ ${#TEST_FILES[@]} -eq 0 ]; then
    echo -e "${RED}Error: No test files found in tests/ folder${NC}"
    exit 1
fi

echo -e "Found ${GREEN}${#TEST_FILES[@]}${NC} test files"

# Build test support objects (utilities + harness) once, link into every test
echo -e "${YELLOW}Building test support objects...${NC}"
SUPPORT_OBJS=""
for support in test_utils test_harness; do
    src="tests/${support}.c"
    if [ -f "$src" ]; then
        obj="tests/${support}_cov.o"
        gcc $CFLAGS $INCLUDES -c "$src" -o "$obj"
        SUPPORT_OBJS="$SUPPORT_OBJS $obj"
    fi
done

# Build and run each test
echo -e "${YELLOW}Building and running tests...${NC}"
for test_file in "${TEST_FILES[@]}"; do
    test_name=$(basename "$test_file" .c)
    echo -e "  Building ${test_name}..."
    gcc $CFLAGS $INCLUDES "$test_file" $SUPPORT_OBJS -Lsrc -lur_cov -lm -o "tests/${test_name}_cov" --coverage

    echo -e "  Running ${test_name}..."
    "./tests/${test_name}_cov" || true
done

# Generate coverage data
echo -e "${YELLOW}Generating coverage report...${NC}"

# Capture coverage data
lcov --capture --directory . --output-file coverage.info

# Filter to only include src/ files (exclude tests and system headers)
lcov --remove coverage.info '/usr/*' '*/tests/*' --output-file coverage.info --ignore-errors unused

# Clean up intermediary files before generating HTML
echo -e "${YELLOW}Cleaning up intermediary files...${NC}"
find . -name "*.gcda" -delete
find . -name "*.gcno" -delete
rm -f tests/*_cov
rm -f tests/test_utils_cov.o tests/test_harness_cov.o
rm -f src/libur_cov.a
rm -rf "$COV_OBJDIR"

# Generate HTML report
echo -e "${YELLOW}Generating HTML coverage report...${NC}"
genhtml coverage.info --output-directory coverage_html --title "BC_UR Coverage Report"

# Print summary
echo ""
echo -e "${GREEN}=== Coverage Summary ===${NC}"
lcov --summary coverage.info 2>&1 || true

# Clean up coverage.info
rm -f coverage.info

echo ""
echo -e "${GREEN}HTML report generated: ${PROJECT_ROOT}/coverage_html/index.html${NC}"
echo -e "Open with: ${YELLOW}xdg-open coverage_html/index.html${NC}"
