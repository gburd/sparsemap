
OBJS = sparsemap.o
STATIC_LIB = libsparsemap.a
SHARED_LIB = libsparsemap.so

LIBS =  -lm
#CFLAGS = -Wall -Wextra -Wpedantic -Of -std=c11 -Iinclude/ -fPIC
#CFLAGS = -Wall -Wextra -Wpedantic -Og -g -std=c11 -Iinclude/ -fPIC
#CFLAGS = -DSPARSEMAP_DIAGNOSTIC -DDEBUG -Wall -Wextra -Wpedantic -O0 -g -std=c11 -Iinclude/ -fPIC
CFLAGS = -DSPARSEMAP_DIAGNOSTIC -DDEBUG -Wall -Wextra -Wpedantic -Ofast -g -std=c11 -Iinclude/ -fPIC
#CFLAGS = -Wall -Wextra -Wpedantic -Og -g -std=c11 -Iinclude/ -fPIC
#CFLAGS = -Wall -Wextra -Wpedantic -Ofast -g -std=c11 -Iinclude/ -fPIC
#CFLAGS = -DSPARSEMAP_DIAGNOSTIC -DDEBUG -Wall -Wextra -Wpedantic -Og -g -fsanitize=address,leak,object-size,pointer-compare,pointer-subtract,null,return,bounds,pointer-overflow,undefined -fsanitize-address-use-after-scope -std=c11 -Iinclude/ -fPIC
#CFLAGS = -Wall -Wextra -Wpedantic -Og -g -fsanitize=all -fhardened -std=c11 -Iinclude/ -fPIC

#TEST_FLAGS = -DDEBUG -Wall -Wextra -Wpedantic -O0 -g -std=c11 -Iinclude/ -Itests/ -fPIC
TEST_FLAGS = -Wall -Wextra -Wpedantic -Ofast -g -std=c11 -Iinclude/ -Itests/ -fPIC
#TEST_FLAGS = -Wall -Wextra -Wpedantic -Og -g -std=c11 -Iinclude/ -Itests/ -fPIC
#TEST_FLAGS = -DDEBUG -Wall -Wextra -Wpedantic -Og -g -fsanitize=address,leak,object-size,pointer-compare,pointer-subtract,null,return,bounds,pointer-overflow,undefined -fsanitize-address-use-after-scope -std=c11 -Iinclude/ -fPIC

TESTS = tests/test tests/soak
TEST_OBJS = tests/test.o lib/munit.o lib/tdigest.o lib/common.o
LIB_OBJS = lib/munit.o lib/tdigest.o lib/common.o lib/roaring.o
EXAMPLES = examples/ex_1 examples/ex_2 examples/ex_3 examples/ex_4

.PHONY: all shared static clean test examples mls

all: static shared

static: $(STATIC_LIB)

shared: $(SHARED_LIB)

$(STATIC_LIB): $(OBJS)
	ar rcs $(STATIC_LIB) $?

$(SHARED_LIB): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $? -shared

examples: $(STATIC_LIB) $(EXAMPLES) $(TEST_OBJS)

mls: examples/mls

tests: $(TESTS)

test: tests
	env ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=verbosity=1:log_threads=1 ./tests/test

soak: tests
	env ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=verbosity=1:log_threads=1 ./tests/soak

tests/test: $(TEST_OBJS) $(LIB_OBJS) $(STATIC_LIB)
	$(CC) $^ $(LIBS) -o $@ $(TEST_FLAGS)

clean:
	rm -f $(OBJS)
	rm -f examples/main.c
	rm -f $(STATIC_LIB) $(SHARED_LIB)
	rm -f $(TESTS) tests/*.o
	rm -f $(EXAMPLES) examples/*.o

format:
	clang-format -i src/sparsemap.c include/sparsemap.h examples/ex_*.c tests/soak.c tests/test.c lib/common.c include/common.h
#	clang-format -i include/*.h src/*.c tests/*.c tests/*.h examples/*.c

%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $^

lib/%.o: tests/%.c
	$(CC) $(CFLAGS) -c -o $@ $^

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -c -o $@ $^

examples/%.o: examples/%.c
	$(CC) $(CFLAGS) -c -o $@ $^

examples/ex_1:  $(LIB_OBJS) examples/ex_1.o $(STATIC_LIB)
	$(CC) $^ $(LIBS) -o $@ $(TEST_FLAGS)

examples/ex_2: $(LIB_OBJS) examples/ex_2.o $(STATIC_LIB)
	$(CC) $^ $(LIBS) -o $@ $(TEST_FLAGS)

examples/ex_3: $(LIB_OBJS) examples/ex_3.o $(STATIC_LIB)
	$(CC) $^ $(LIBS) -o $@ $(TEST_FLAGS)

examples/ex_4: $(LIB_OBJS) examples/ex_4.o $(STATIC_LIB)
	$(CC) $^ $(LIBS) -o $@ $(TEST_FLAGS)

tests/soak: $(LIB_OBJS) tests/soak.o $(STATIC_LIB)
	$(CC) $^ $(LIBS) -o $@ $(TEST_FLAGS)

todo:
	rg -i 'todo|gsb|abort'

# cp src/sparsemap.c /tmp && clang-tidy src/sparsemap.c -fix -fix-errors -checks="readability-braces-around-statements" -- -DDEBUG -DSPARSEMAP_DIAGNOSTIC -DSPARSEMAP_ASSERT -Wall -Wextra -Wpedantic -Og -g -std=c11 -Iinclude/ -fPIC

# clear; make clean examples test && env ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=verbosity=1:log_threads=1 ./tests/test

# clear; make clean examples test && env ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=verbosity=1:log_threads=1 ./examples/soak
