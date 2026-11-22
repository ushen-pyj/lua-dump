CFLAGS ?= -O2 -Wall -Wextra
LUA ?= lua5.4
LUA_CFLAGS ?= $(if $(LUA_INC),-I$(LUA_INC),$(shell pkg-config --cflags lua5.4))
LUA_LIBS ?= $(if $(LUA_LIB),-L$(LUA_LIB) -llua -lm -ldl,$(shell pkg-config --libs lua5.4))

.PHONY: all test clean

all: dump.so

dump.so: dump.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LUA_CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ $< $(LDLIBS)

test: dump.so
	LUA_CPATH_5_4='./?.so' $(LUA) test.lua

bench: bench.c dump.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LUA_CFLAGS) $(LDFLAGS) -o $@ $^ $(LUA_LIBS) $(LDLIBS)

clean:
	$(RM) dump.so bench
