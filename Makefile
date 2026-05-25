RCH    ?= arm64
CC      := clang
CFLAGS  := -Wall -Wno-deprecated-declarations -g3 -O0 -arch $(ARCH)

SRC     := ./src
TEST    := ./test
INCLUDE := ./include

LIBCKPT_SOURCES := $(SRC)/libckpt.c $(SRC)/pac.c $(SRC)/vm_common.c \
                   $(SRC)/vm_checkpoint.c $(SRC)/writeckpt.c \
                   $(SRC)/time_wrappers.c $(SRC)/shared_cache.c \
                   $(SRC)/exit_wrappers.c $(SRC)/pthread_wrappers.c

RESTART_SOURCES := $(SRC)/restart.c $(SRC)/pac.c $(SRC)/vm_common.c \
                   $(SRC)/vm_restore.c $(SRC)/readckpt.c \
                   $(SRC)/shared_cache.c

TESTS           := count funcptr det
BINARIES        := ckpt printckpt $(TESTS)
ALL             := $(BINARIES) restart libckpt.dylib

build: $(ALL)

libckpt.dylib: $(LIBCKPT_SOURCES)
	$(CC) $(CFLAGS) -I$(INCLUDE) -dynamiclib -fPIC -o $@ $^

__TEXT          := 0x300000000
__DATA          := 0x300004000
__LINKEDIT      := 0x300008000
restart: $(RESTART_SOURCES)
	$(CC) $(CFLAGS) -I$(INCLUDE)  		\
	-Wl,-segaddr,__TEXT,$(__TEXT) 		\
	-Wl,-segaddr,__DATA,$(__DATA) 		\
	-Wl,-segaddr,__LINKEDIT,$(__LINKEDIT) 	\
	-o $@ $^

%: $(SRC)/%.c
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $<

%: $(TEST)/%.c
	$(CC) $(CFLAGS) -o $@ $<

%: $(TEST)/%.cpp
	clang++ -std=c++20 $(CFLAGS) -o $@ $<

clean:
	rm -f $(ALL)
	rm -rf *.dSYM *.dylib *.o *.dat
