LUA_INC ?= $(HOME)/AppData/Local/Programs/Lua/include
LUA_LIB ?= $(HOME)/AppData/Local/Programs/Lua/lib
CC = gcc

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

ifneq (,$(filter $(UNAME_S),Linux Darwin))
  TARGET = dump.so
  CCFLAGS = -Wall -O2 -fPIC -shared -I$(LUA_INC)
  LIBS =
  BENCH = bench
  BENCH_FLAGS = -Wall -O2 -I$(LUA_INC)
  BENCH_LIBS = -L$(LUA_LIB) -llua -lm
  ifeq ($(UNAME_S),Linux)
    BENCH_LIBS += -ldl
  endif
else
  TARGET = dump.dll
  CCFLAGS = -Wall -O2 -shared -I"$(LUA_INC)"
  LIBS = -L"$(LUA_LIB)" -llua54
  BENCH = bench.exe
  BENCH_FLAGS = -Wall -O2 -I"$(LUA_INC)"
  BENCH_LIBS = -L"$(LUA_LIB)" -llua54
endif

all: $(TARGET)

$(TARGET): dump.c
	$(CC) $(CCFLAGS) -o $@ $^ $(LIBS)

bench: $(BENCH)

$(BENCH): bench.c dump.c
	$(CC) $(BENCH_FLAGS) -o $@ bench.c dump.c $(BENCH_LIBS)

clean:
	rm -f dump.so dump.dll bench bench.exe

.PHONY: all bench clean
