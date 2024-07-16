# Build type (debug, sanitize, or release)
BUILD_TYPE := debug

COMMON_CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -fPIC -I.
CFLAGS_DEBUG =  $(COMMON_CFLAGS) -Og -g -DSPARSEMAP_DIAGNOSTIC -DDEBUG
CFLAGS_SANITIZE = $(COMMON_CFLAGS) -Og -g -DSPARSEMAP_DIAGNOSTIC -DDEBUG -fsanitize=address,leak,object-size,pointer-compare,pointer-subtract,null,return,bounds,pointer-overflow,undefined -fsanitize-address-use-after-scope -std=c11
CFLAGS_RELEASE = $(COMMON_CFLAGS) -Ofast
CFLAGS_TEST = $(COMMON_CFLAGS) -Og -g -DDEBUG -I. -Itest
CFLAGS := $(if $(filter debug,$(BUILD_TYPE)),$(CFLAGS_DEBUG), \
            $(if $(filter release,$(BUILD_TYPE)),$(CFLAGS_RELEASE), \
            $(if $(filter sanitize,$(BUILD_TYPE)),$(CFLAGS_SANITIZE), \
            $(error Unknown build type: $(BUILD_TYPE)))))

LDFLAGS = -lm
LDFLAGS_VERBOSE = -Wl,-v

SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)
STATIC_LIB = libsparsemap.a
SHARED_LIB = libsparsemap.so
TEST_TARGETS = test/test test/soak test/ex_1 test/ex_2 test/ex_3 test/ex_4
TEST_SRCS = $(filter-out $(wildcard test/ex_*) test/midl.c $(TEST_TARGETS:=.c), $(wildcard test/*.c))
TEST_OBJS = $(TEST_SRCS:.c=.o)
TEST_DEPS = $(filter-out $(TEST_TARGETS:=.h), $(TEST_SRCS:.c=.h))

# Targets
all: $(STATIC_LIB) $(SHARED_LIB)

info:
	$(info TEST_SRCS: $(TEST_SRCS))
	$(info TEST_OBJS: $(TEST_OBJS))
	$(info TEST_DEPS: $(TEST_DEPS))

$(STATIC_LIB): $(OBJS)
	ar rcs libsparsemap.a $(OBJS)

$(SHARED_LIB): $(OBJS)
	$(CC) -shared -o libsparsemap.so $(OBJS) $(LDFLAGS)

check: $(TEST_TARGETS)

test/test: $(TEST_OBJS) $(STATIC_LIB) test/test.o
	$(CC) $^ $(LDFLAGS) -o $@

test/soak: $(TEST_OBJS) $(STATIC_LIB) test/soak.o
	$(CC) $^ $(LDFLAGS) -o $@

test/ex_1: test/ex_1.o $(STATIC_LIB)
	$(CC) $^ $(LDFLAGS) -o $@

test/ex_2: test/ex_2.o $(STATIC_LIB)
	$(CC) $^ $(LDFLAGS) -o $@

test/ex_3: test/ex_3.o $(STATIC_LIB)
	$(CC) $^ $(LDFLAGS) -o $@

test/ex_4: test/ex_4.o $(STATIC_LIB)
	$(CC) $^ $(LDFLAGS) -o $@

$(TEST_OBJS): $(TEST_SRCS) $(TEST_DEPS)
	$(CC) $(CFLAGS_TEST) -c $(subst .o,.c,$@) -o $@

$(OBJS): $(SRCS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f libsparsemap.a libsparsemap.so $(OBJS) $(TEST_OBJS) \
		test/test test/soak test/ex_1 test/ex_2 test/ex_3 test/ex_4
