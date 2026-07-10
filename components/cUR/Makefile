# Root Makefile for UR C implementation testing

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g
LDFLAGS =
INCLUDES = -Isrc
SRCDIR = src
OBJDIR = src/obj

# DEBUG=1 switches to -O0 with AddressSanitizer + UndefinedBehaviorSanitizer.
# Requires a full rebuild when toggling (sanitized and non-sanitized objects
# cannot link together): `make clean && make DEBUG=1 test`.
ifeq ($(DEBUG),1)
  CFLAGS  += -O0 -fno-omit-frame-pointer -fsanitize=address,undefined
  LDFLAGS += -fsanitize=address,undefined
else
  CFLAGS  += -O2
endif

# Source files (exclude test files)
SOURCES = utils.c bytewords.c fountain_decoder.c fountain_encoder.c fountain_utils.c crc32.c ur_decoder.c ur_encoder.c ur.c sha256/sha256.c \
          types/byte_buffer.c types/cbor_data.c types/cbor_encoder.c types/cbor_decoder.c types/registry.c types/bytes_type.c types/psbt.c types/bip39.c \
          types/keypath.c types/hd_key.c types/multi_key.c types/output.c

# Object files
OBJECTS = $(SOURCES:%.c=$(OBJDIR)/%.o)

# Target library
TARGET = $(SRCDIR)/libur.a

# Test utilities + harness — linked into every test binary.
TEST_UTILS_OBJECT = tests/test_utils.o
TEST_HARNESS_OBJECT = tests/test_harness.o
TEST_SUPPORT_OBJECTS = $(TEST_UTILS_OBJECT) $(TEST_HARNESS_OBJECT)

# Test program stems — each matches a tests/test_ur_<stem>.c source.
# The user-facing target name is `test-<stem-with-dashes>` (e.g. test-bytes-decoder).
TEST_STEMS = bytes_decoder bytes_encoder output_decoder output_encoder \
             PSBT_decoder PSBT_encoder bip39_decoder \
             account_descriptor_decoder output_descriptor_roundtrip \
             negative

TEST_BINS = $(TEST_STEMS:%=tests/test_ur_%)
TEST_TARGETS = $(foreach s,$(TEST_STEMS),test-$(subst _,-,$(s)))

.PHONY: all clean test check coverage $(TEST_TARGETS)

all: $(TARGET)

# Run all tests. `check` is a GNU-convention alias.
test: $(TEST_TARGETS)
	@echo "All tests passed."

check: test

# Rebuilds the library with gcov instrumentation, runs every test, and
# produces coverage_html/. Requires gcov (from gcc) and lcov.
coverage:
	./scripts/coverage.sh

$(TARGET): $(OBJECTS)
	ar rcs $@ $^

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_UTILS_OBJECT): tests/test_utils.c tests/test_utils.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_HARNESS_OBJECT): tests/test_harness.c tests/test_harness.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Pattern rule: build each test binary.
tests/test_ur_%: tests/test_ur_%.c $(TEST_SUPPORT_OBJECTS) $(TARGET)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(TEST_SUPPORT_OBJECTS) -L$(SRCDIR) -lur -lm $(LDFLAGS) -o $@

# Generate a `test-<name>` phony target per stem that runs the corresponding binary.
define TEST_RUN_RULE
test-$(subst _,-,$(1)): tests/test_ur_$(1)
	./tests/test_ur_$(1)
endef
$(foreach s,$(TEST_STEMS),$(eval $(call TEST_RUN_RULE,$(s))))

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TEST_SUPPORT_OBJECTS) $(TEST_BINS)

# Dependencies
$(OBJDIR)/utils.o: $(SRCDIR)/utils.c $(SRCDIR)/utils.h
$(OBJDIR)/crc32.o: $(SRCDIR)/crc32.c $(SRCDIR)/crc32.h
$(OBJDIR)/bytewords.o: $(SRCDIR)/bytewords.c $(SRCDIR)/bytewords.h $(SRCDIR)/utils.h $(SRCDIR)/crc32.h
$(OBJDIR)/fountain_utils.o: $(SRCDIR)/fountain_utils.c $(SRCDIR)/fountain_utils.h $(SRCDIR)/fountain_decoder.h $(SRCDIR)/fountain_types.h $(SRCDIR)/utils.h $(SRCDIR)/sha256/sha256.h
$(OBJDIR)/fountain_decoder.o: $(SRCDIR)/fountain_decoder.c $(SRCDIR)/fountain_decoder.h $(SRCDIR)/fountain_utils.h $(SRCDIR)/fountain_types.h $(SRCDIR)/crc32.h $(SRCDIR)/utils.h
$(OBJDIR)/fountain_encoder.o: $(SRCDIR)/fountain_encoder.c $(SRCDIR)/fountain_encoder.h $(SRCDIR)/fountain_decoder.h $(SRCDIR)/fountain_utils.h $(SRCDIR)/fountain_types.h $(SRCDIR)/crc32.h $(SRCDIR)/utils.h
$(OBJDIR)/types/byte_buffer.o: $(SRCDIR)/types/byte_buffer.c $(SRCDIR)/types/byte_buffer.h $(SRCDIR)/sha256/sha256.h $(SRCDIR)/utils.h
$(OBJDIR)/ur_decoder.o: $(SRCDIR)/ur_decoder.c $(SRCDIR)/ur_decoder.h $(SRCDIR)/fountain_decoder.h $(SRCDIR)/bytewords.h $(SRCDIR)/utils.h
$(OBJDIR)/ur_encoder.o: $(SRCDIR)/ur_encoder.c $(SRCDIR)/ur_encoder.h $(SRCDIR)/fountain_encoder.h $(SRCDIR)/bytewords.h $(SRCDIR)/utils.h
$(OBJDIR)/ur.o: $(SRCDIR)/ur.c $(SRCDIR)/ur.h $(SRCDIR)/ur_decoder.h $(SRCDIR)/utils.h
$(OBJDIR)/sha256/sha256.o: $(SRCDIR)/sha256/sha256.c $(SRCDIR)/sha256/sha256.h