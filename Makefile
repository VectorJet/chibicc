CFLAGS=-std=c11 -g -fno-common -Wall -Wno-switch

# Opt-in LLVM backend (--backend=llvm). Off by default, so the default build
# has no LLVM dependency. Enable with `make WITH_LLVM=1`; switching the setting
# rebuilds all objects automatically.
WITH_LLVM?=0
LLVM_CONFIG?=llvm-config-19
CLANG_DRIVER?=clang-19

ifeq ($(WITH_LLVM),1)
CFLAGS+=-DCHIBICC_LLVM -DCHIBICC_CLANG='"$(CLANG_DRIVER)"' $(shell $(LLVM_CONFIG) --cflags)
LDFLAGS+=$(shell $(LLVM_CONFIG) --ldflags) $(shell $(LLVM_CONFIG) --libs core target analysis native)
endif

SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)

TEST_SRCS=$(wildcard test/*.c)
TESTS=$(TEST_SRCS:.c=.exe)

# Stage 1

chibicc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Changing WITH_LLVM must rebuild every object, so depend on a per-setting stamp.
.with-llvm-$(WITH_LLVM):
	rm -f .with-llvm-*
	touch $@

$(OBJS): chibicc.h .with-llvm-$(WITH_LLVM)

test/%.exe: chibicc test/%.c
	./chibicc -Iinclude -Itest -c -o test/$*.o test/$*.c
	$(CC) -pthread -o $@ test/$*.o -xc test/common

test: $(TESTS)
	for i in $^; do echo $i; ./$i || exit 1; echo; done
	test/driver.sh ./chibicc
	$(if $(filter 1,$(WITH_LLVM)),$(MAKE) test-llvm,)

test-all: test test-stage2

# LLVM backend tests (requires WITH_LLVM=1)

test-llvm: chibicc
	@test "$(WITH_LLVM)" = 1 || { echo 'test-llvm requires WITH_LLVM=1'; exit 1; }
	test/llvm/run.sh ./chibicc

# Stage 2

stage2/chibicc: $(OBJS:%=stage2/%)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

stage2/%.o: chibicc %.c
	mkdir -p stage2/test
	./chibicc -c -o $(@D)/$*.o $*.c

stage2/test/%.exe: stage2/chibicc test/%.c
	mkdir -p stage2/test
	./stage2/chibicc -Iinclude -Itest -c -o stage2/test/$*.o test/$*.c
	$(CC) -pthread -o $@ stage2/test/$*.o -xc test/common

test-stage2: $(TESTS:test/%=stage2/test/%)
	for i in $^; do echo $$i; ./$$i || exit 1; echo; done
	test/driver.sh ./stage2/chibicc

# Misc.

clean:
	rm -rf chibicc tmp* $(TESTS) test/*.s test/*.exe stage2 .with-llvm-*
	find * -type f '(' -name '*~' -o -name '*.o' ')' -exec rm {} ';'

.PHONY: test test-llvm clean test-stage2
