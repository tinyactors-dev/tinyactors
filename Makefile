CC = clang
AR = ar
BUILD = build
CPPFLAGS = -Isrc -Itests
CFLAGS = -std=c99 -O2 -g -Wall -Wextra -Werror -pedantic
LDFLAGS =
COMPILER_SUPPORT = src/compiler_runtime.c
LIB_SOURCES = $(filter-out src/compiler_runtime.c,$(wildcard src/*.c)) $(COMPILER_SUPPORT)
LIB_OBJECTS = $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SOURCES))
TEST_SOURCES = $(filter-out tests/host.c,$(wildcard tests/*.c))
TEST_BINS = $(patsubst tests/%.c,$(BUILD)/test-%,$(TEST_SOURCES))

.PHONY: all test check syntax arm symbols sanitize ubsan benchmark benchmark-ring benchmark-actors arity ruby-interface
all: $(BUILD)/libtinyactors.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/tinyactors.h src/internal.h Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffreestanding -fno-stack-protector -c $< -o $@

# Compiler support must remain usable before sanitizer interception initializes.
# The ASan target instead links the host primitives so their interceptors apply.
$(BUILD)/compiler_runtime.o: src/compiler_runtime.c Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fno-sanitize=all -ffreestanding -fno-stack-protector -c $< -o $@

$(BUILD)/libtinyactors.a: $(LIB_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD)/test-%: tests/%.c tests/host.c tests/host.h $(BUILD)/libtinyactors.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< tests/host.c $(BUILD)/libtinyactors.a $(LDFLAGS) -o $@

test: $(TEST_BINS)
	@set -e; for test in $(TEST_BINS); do echo "RUN $$test"; "$$test"; done

check: test syntax arm symbols arity ruby-interface

ruby-interface:
	ruby tools/check_ruby_interface.rb

syntax:
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffreestanding -fsyntax-only $(LIB_SOURCES) examples/*.c $(TEST_SOURCES) tests/host.c
	$(CC) -x c++ -std=c++11 -Wall -Wextra -Werror -pedantic -ffreestanding -include src/tinyactors.h -fsyntax-only /dev/null

arm:
	$(CC) -target arm-none-eabi -Isrc $(CFLAGS) -ffreestanding -fsyntax-only $(LIB_SOURCES) examples/*.c

symbols: $(BUILD)/libtinyactors.a
	ruby tools/check_symbols.rb $(BUILD)/libtinyactors.a

arity:
	ruby tools/check_arity.rb

$(BUILD)/bench-entities: benchmarks/entities.c $(BUILD)/libtinyactors.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libtinyactors.a $(LDFLAGS) -o $@

$(BUILD)/tinyactors-bench: benchmarks/ring.c $(BUILD)/libtinyactors.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libtinyactors.a $(LDFLAGS) -lrt -o $@

$(BUILD)/tinyactors-actors-bench: benchmarks/actors.c $(BUILD)/libtinyactors.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libtinyactors.a $(LDFLAGS) -o $@

benchmark-ring: $(BUILD)/tinyactors-bench
	$(BUILD)/tinyactors-bench

benchmark-actors: $(BUILD)/tinyactors-actors-bench
	$(BUILD)/tinyactors-actors-bench

benchmark: $(BUILD)/bench-entities $(BUILD)/tinyactors-bench $(BUILD)/tinyactors-actors-bench
	$(BUILD)/bench-entities
	$(BUILD)/tinyactors-bench
	$(BUILD)/tinyactors-actors-bench

sanitize:
	$(MAKE) BUILD=$(BUILD)/asan-host COMPILER_SUPPORT= CFLAGS='-std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test

ubsan:
	$(MAKE) BUILD=$(BUILD)/ubsan CFLAGS='-std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=undefined -fno-sanitize-recover=all' LDFLAGS='-fsanitize=undefined' test
