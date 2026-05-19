ARCH    ?= arm64
CC      := clang
CFLAGS  := -Wall -Wno-deprecated-declarations -g3 -O0 -arch $(ARCH)

SRC     := ./src
TEST    := ./test
INCLUDE := ./include

LIBCKPT_SOURCES := $(SRC)/libckpt.c $(SRC)/pac.c $(SRC)/vm_region.c \
                   $(SRC)/writeckpt.c $(SRC)/time_wrappers.c \
                   $(SRC)/readckpt.c

RESTART_SOURCES := $(SRC)/restart.c $(SRC)/pac.c $(SRC)/vm_region.c \
                   $(SRC)/readckpt.c

BINARIES        := ckpt printckpt rand count
ALL             := $(BINARIES) restart libckpt.dylib

build: $(ALL)

libckpt.dylib: $(LIBCKPT_SOURCES)
	$(CC) $(CFLAGS) -I$(INCLUDE) -dynamiclib -fPIC -o $@ $^

TEXT_ADDR := 0x300000000
DATA_ADDR := 0x300004000
restart: $(RESTART_SOURCES)
	$(CC) $(CFLAGS) -I$(INCLUDE) \
	-Wl,-segaddr,__TEXT,$(TEXT_ADDR) \
	-Wl,-segaddr,__DATA,$(DATA_ADDR) -o $@ $^

%: $(SRC)/%.c
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $<

%: $(TEST)/%.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(ALL)
	rm -rf *.dSYM *.dylib *.o *.dat
