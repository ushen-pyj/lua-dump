#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INDENT_WIDTH 2
#define MAX_DEPTH 32
#define MAX_INDENT (MAX_DEPTH * INDENT_WIDTH)
#define BUF_INIT 4096
#define PRETTY_WIDTH 80
#define MAX_BYTES (1024u * 1024u)
#define TRUNCATED "<truncated>"
#define UNAVAILABLE "<dump unavailable>"

static const char indent_cache[MAX_INDENT + 1] =
	"                                "
	"                                ";

typedef struct {
	char *data;
	size_t len;
} packet;

typedef struct {
	lua_State *L;
	packet *owner;
	size_t n;
	size_t size;
	int pretty;
	size_t line_start;
	int stopped;
} dumpbuf;

#define PACKET_MT "dump.packet"
#define WRITER_MT "dump.writer"
#define MEMORY_MT "dump.packet_memory"

/* Native packet bytes are invisible to Lua's GC. Detached messages belong
 * to the transport and must no longer influence the producing Lua state. */
typedef struct {
	size_t held;
	size_t last_step;
} packet_memory;

static int
is_ident_start(unsigned char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int
is_ident(const char *s, size_t len) {
	size_t i;
	if (len == 0) {
		return 0;
	}
	if (!is_ident_start((unsigned char)s[0])) {
		return 0;
	}
	for (i = 1; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (!(is_ident_start(c) || (c >= '0' && c <= '9'))) {
			return 0;
		}
	}
	return 1;
}

static void
packet_release(packet *p) {
	free(p->data);
	p->data = NULL;
	p->len = 0;
}

/* Empty packets and packets from older builds have no charge. */
static void
uncharge(lua_State *L, packet *p) {
	if (p->len == 0) {
		return;
	}
	lua_getiuservalue(L, 1, 1);
	packet_memory *memory = (packet_memory *)luaL_testudata(L, -1, MEMORY_MT);
	if (memory != NULL) {
		memory->held -= p->len;
		if (memory->last_step > memory->held) {
			memory->last_step = memory->held;
		}
	}
	lua_pop(L, 1);
}

static int
lbuf_close(lua_State *L) {
	packet *p = (packet *)luaL_testudata(L, 1, PACKET_MT);
	if (p == NULL) {
		p = (packet *)luaL_checkudata(L, 1, WRITER_MT);
	} else {
		uncharge(L, p);
	}
	packet_release(p);
	return 0;
}

static packet *
packet_new(lua_State *L, const char *metatable, int uservalues) {
	packet *p = (packet *)lua_newuserdatauv(L, sizeof(*p), uservalues);
	p->data = NULL;
	p->len = 0;
	luaL_setmetatable(L, metatable);
	return p;
}

static void
buf_grow(dumpbuf *B, size_t need) {
	size_t newsize = B->size ? B->size : BUF_INIT;
	char *p;
	/* buf_add bounds the sum by MAX_BYTES, a power of two. */
	while (B->n + need > newsize) {
		newsize *= 2;
	}
	p = (char *)realloc(B->owner->data, newsize);
	if (p == NULL) {
		packet_release(B->owner);
		luaL_error(B->L, "dump: out of memory");
	}
	B->owner->data = p;
	B->size = newsize;
}

/* Growth can move the allocation, so s must not point into the output. */
static void
buf_add(dumpbuf *B, const char *s, size_t len) {
	if (B->stopped) {
		return;
	}
	if (len > MAX_BYTES - B->n) {
		len = MAX_BYTES - B->n;
		B->stopped = 1;
	}
	if (len > B->size - B->n) {
		buf_grow(B, len);
	}
	memcpy(B->owner->data + B->n, s, len);
	B->n += len;
}

static void
buf_addch(dumpbuf *B, char c) {
	buf_add(B, &c, 1);
}

static void
buf_newline(dumpbuf *B, int depth) {
	int n = depth * INDENT_WIDTH;
	buf_addch(B, '\n');
	B->line_start = B->n;
	buf_add(B, indent_cache, (size_t)n);
}

/* Fold a single indented line back onto the previous one. The copy is bounded
 * by PRETTY_WIDTH, with one byte reserved for the next comma or closing brace. */
static int
try_join_line(dumpbuf *B, size_t split, size_t previous_line, int depth, int space) {
	size_t used = split - previous_line;
	size_t text, len;
	if (B->line_start != split + 1 || used >= PRETTY_WIDTH - (size_t)space) {
		return 0;
	}
	text = B->line_start + (size_t)depth * INDENT_WIDTH;
	len = B->n - text;
	if (len >= PRETTY_WIDTH - used - (size_t)space) {
		return 0;
	}
	if (space) {
		B->owner->data[split++] = ' ';
	}
	memmove(B->owner->data + split, B->owner->data + text, len);
	B->n = split + len;
	B->line_start = previous_line;
	return 1;
}

static void
buf_add_int(dumpbuf *B, lua_Integer v) {
	char tmp[32];
	char *p = tmp + sizeof(tmp);
	unsigned long long x;
	int neg = 0;

	if (v < 0) {
		neg = 1;
		x = (unsigned long long)(-(v + 1)) + 1;
	} else {
		x = (unsigned long long)v;
	}
	do {
		*--p = (char)('0' + (x % 10));
		x /= 10;
	} while (x > 0);
	if (neg) {
		*--p = '-';
	}
	buf_add(B, p, (size_t)(tmp + sizeof(tmp) - p));
}

static void dump_value(lua_State *L, dumpbuf *B, int index, const void **path, int depth);

static int
need_escape(unsigned char c) {
	return c < 0x20 || c == 0x7f || c == '"' || c == '\\';
}

static void
dump_string(dumpbuf *B, const char *s, size_t len) {
	size_t i = 0;
	buf_addch(B, '"');
	/* Each input byte needs at least one output byte. One extra detects overflow. */
	if (len > MAX_BYTES - B->n + 1) {
		len = MAX_BYTES - B->n + 1;
	}
	while (i < len && !B->stopped) {
		size_t start = i;
		unsigned char c;
		while (i < len && !need_escape((unsigned char)s[i])) {
			i++;
		}
		if (i > start) {
			buf_add(B, s + start, i - start);
		}
		if (i >= len) {
			break;
		}
		c = (unsigned char)s[i++];
		switch (c) {
		case '\n':
			buf_add(B, "\\n", 2);
			break;
		case '\t':
			buf_add(B, "\\t", 2);
			break;
		case '\r':
			buf_add(B, "\\r", 2);
			break;
		case '"':
			buf_add(B, "\\\"", 2);
			break;
		case '\\':
			buf_add(B, "\\\\", 2);
			break;
		default: {
			static const char digits[] = "0123456789abcdef";
			char hex[] = { '\\', 'x', digits[c >> 4], digits[c & 15] };
			buf_add(B, hex, sizeof(hex));
			break;
		}
		}
	}
	buf_addch(B, '"');
}

static void
dump_key(lua_State *L, dumpbuf *B, int key, const void **path, int depth) {
	if (lua_type(L, key) == LUA_TSTRING) {
		size_t len;
		const char *str = lua_tolstring(L, key, &len);
		size_t scan = MAX_BYTES - B->n + 1;
		if (is_ident(str, len < scan ? len : scan)) {
			buf_add(B, str, len);
		} else {
			dump_string(B, str, len);
		}
	} else {
		buf_addch(B, '[');
		dump_value(L, B, key, path, depth);
		buf_addch(B, ']');
	}
}

/* Value is at -1. A keyed entry also has its key at -2. */
static void
dump_entry(lua_State *L, dumpbuf *B, int keyed, const void **path, int depth,
	size_t content, int *previous_inline) {
	size_t previous_line = B->line_start;
	size_t split;
	if (B->n != content) {
		buf_addch(B, ',');
	}
	split = B->n;
	if (B->pretty) {
		buf_newline(B, depth);
	}
	if (keyed) {
		dump_key(L, B, -2, path, depth);
		if (B->pretty) {
			buf_add(B, " = ", 3);
		} else {
			buf_addch(B, '=');
		}
	}
	dump_value(L, B, -1, path, depth);
	if (B->pretty && !B->stopped) {
		int single_line = B->line_start == split + 1;
		if (*previous_inline && single_line) {
			try_join_line(B, split, previous_line, depth, 1);
		}
		*previous_inline = single_line;
	}
}

static void
dump_value(lua_State *L, dumpbuf *B, int index, const void **path, int depth) {
	int type = lua_type(L, index);
	if (B->stopped) {
		return;
	}

	switch (type) {
	case LUA_TNIL:
		buf_add(B, "nil", 3);
		break;
	case LUA_TBOOLEAN:
		if (lua_toboolean(L, index)) {
			buf_add(B, "true", 4);
		} else {
			buf_add(B, "false", 5);
		}
		break;
	case LUA_TNUMBER:
		if (lua_isinteger(L, index)) {
			buf_add_int(B, lua_tointeger(L, index));
		} else {
			char buf[64];
			int n = snprintf(buf, sizeof(buf), "%.14g", (double)lua_tonumber(L, index));
			if (n > 0 && (size_t)n < sizeof(buf)) {
				buf_add(B, buf, (size_t)n);
			} else {
				buf_add(B, "<number>", 8);
			}
		}
		break;
	case LUA_TSTRING: {
		size_t len;
		const char *str = lua_tolstring(L, index, &len);
		dump_string(B, str, len);
		break;
	}
	case LUA_TTABLE: {
		int previous_inline = 0;
		size_t opening_line = B->line_start;
		size_t content;
		size_t array_size, slot;
		int top = lua_gettop(L);
		int i;
		const void *table;

		index = lua_absindex(L, index);
		if (depth >= MAX_DEPTH) {
			buf_add(B, "<...>", 5);
			break;
		}
		if (!lua_checkstack(L, 8)) {
			buf_add(B, "<...>", 5);
			break;
		}

		/* Ancestors stay rooted on the Lua stack. */
		table = lua_topointer(L, index);
		for (i = 0; i < depth; i++) {
			if (path[i] == table) {
				buf_add(B, "<cycle>", 7);
				return;
			}
		}
		path[depth] = table;

		buf_addch(B, '{');
		content = B->n;
		array_size = lua_rawlen(L, index);
		for (slot = 1; slot <= array_size && !B->stopped; slot++) {
			lua_rawgeti(L, index, (lua_Integer)slot);
			dump_entry(L, B, 0, path, depth + 1, content, &previous_inline);
			lua_pop(L, 1);
		}
		lua_pushnil(L);
		while (!B->stopped && lua_next(L, index) != 0) {
			lua_Integer k = lua_isinteger(L, -2) ? lua_tointeger(L, -2) : 0;
			if (k < 1 || (lua_Unsigned)k > array_size) {
				dump_entry(L, B, 1, path, depth + 1, content, &previous_inline);
			}
			lua_pop(L, 1);
		}
		lua_settop(L, top);  /* A cutoff can leave the iteration key on the stack. */
		if (B->stopped) {
			return;
		}
		if (B->pretty && B->n != content &&
			!try_join_line(B, content, opening_line, depth + 1, 0)) {
			buf_newline(B, depth);
		}
		buf_addch(B, '}');

		break;
	}
	default: {
		const char *name = lua_typename(L, type);
		buf_addch(B, '<');
		buf_add(B, name, strlen(name));
		buf_addch(B, '>');
		break;
	}
	}
}

static void
dump_args(lua_State *L, dumpbuf *B, int nargs) {
	int i;
	const void *path[MAX_DEPTH];
	int first = B->pretty ? 2 : 1;

	for (i = first; i <= nargs && !B->stopped; i++) {
		if (i != first) {
			buf_addch(B, ' ');
		}
		dump_value(L, B, i, path, 0);
	}
	if (B->stopped) {
		size_t marker_len = sizeof(TRUNCATED) - 1;
		if (B->n > MAX_BYTES - marker_len) {
			B->n = MAX_BYTES - marker_len;
		}
		B->stopped = 0;
		buf_add(B, TRUNCATED, marker_len);
	}
}

static int
format(lua_State *L, int as_string) {
	int nargs = lua_gettop(L);
	int pretty = lua_rawequal(L, 1, lua_upvalueindex(1));
	if (as_string && nargs == pretty) {
		lua_pushliteral(L, "");
		return 1;
	}
	packet_memory *memory = NULL;
	if (!as_string) {
		memory = (packet_memory *)luaL_checkudata(L, lua_upvalueindex(2), MEMORY_MT);
		if (memory->held - memory->last_step >= MAX_BYTES && lua_gc(L, LUA_GCISRUNNING)) {
			memory->last_step = memory->held;
			lua_gc(L, LUA_GCSTEP, (int)(MAX_BYTES / 1024));
		}
	}
	packet *p = packet_new(L, WRITER_MT, !as_string);
	dumpbuf B = { .L = L, .owner = p, .pretty = pretty };
	if (as_string) {
		lua_toclose(L, -1);
	}
	dump_args(L, &B, nargs);
	p->len = B.n;
	if (as_string) {
		lua_pushlstring(L, p->data, p->len);
		return 1;
	}
	/* Trim spare capacity before the packet can sit in a logger queue. */
	if (p->len > 0 && p->len < B.size) {
		char *smaller = (char *)realloc(p->data, p->len);
		if (smaller != NULL) {
			p->data = smaller;
		}
	}
	luaL_setmetatable(L, PACKET_MT);
	lua_pushvalue(L, lua_upvalueindex(2));
	lua_setiuservalue(L, -2, 1);
	memory->held += p->len;
	lua_pushinteger(L, (lua_Integer)p->len);
	return 2;
}

static int
lpack(lua_State *L) {
	return format(L, 0);
}

static int
lpackstring(lua_State *L) {
	return format(L, 1);
}

static int
ldetach(lua_State *L) {
	packet *p = (packet *)luaL_checkudata(L, 1, PACKET_MT);
	uncharge(L, p);
	lua_pushlightuserdata(L, p->data);
	lua_pushinteger(L, (lua_Integer)p->len);
	p->data = NULL;
	p->len = 0;
	return 2;
}

static int
ltrash(lua_State *L) {
	if (lua_type(L, 1) == LUA_TSTRING) {
		return 0;
	}
	if (luaL_testudata(L, 1, PACKET_MT) == NULL) {
		return luaL_error(L, "dump.trash invalid param %s", luaL_typename(L, 1));
	}
	return lbuf_close(L);
}

/* Keep the fallback rooted from module load so an allocation failure needs
 * no new strings or handles. Run formatting below a protected Lua boundary. */
static int
lprotected(lua_State *L) {
	int nargs = lua_gettop(L);
	int results = lua_type(L, lua_upvalueindex(2)) == LUA_TSTRING ? 1 : 2;
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	if (lua_pcall(L, nargs, results, 0) != LUA_OK) {
		lua_pop(L, 1);
		lua_pushvalue(L, lua_upvalueindex(2));
		if (results == 2) {
			lua_pushinteger(L, 0);
			lua_pushvalue(L, lua_upvalueindex(3));
			return 3;
		}
	}
	return results;
}

static void
register_close(lua_State *L, const char *name, lua_CFunction close) {
	luaL_newmetatable(L, name);
	lua_pushcfunction(L, close);
	lua_setfield(L, -2, "__close");
	lua_pushcfunction(L, close);
	lua_setfield(L, -2, "__gc");
	lua_pushstring(L, name);
	lua_setfield(L, -2, "__metatable");
	lua_pop(L, 1);
}

int
luaopen_dump(lua_State *L) {
	luaL_Reg l[] = {
		{ "detach", ldetach },
		{ "trash", ltrash },
		{ NULL, NULL },
	};

	luaL_checkversion(L);
	register_close(L, WRITER_MT, lbuf_close);
	register_close(L, PACKET_MT, lbuf_close);
	luaL_newlib(L, l);
	lua_newtable(L);
	lua_setfield(L, -2, "pretty");
	lua_getfield(L, -1, "pretty");
	packet_memory *memory = (packet_memory *)lua_newuserdatauv(L, sizeof(*memory), 0);
	*memory = (packet_memory){0};
	luaL_newmetatable(L, MEMORY_MT);
	lua_setmetatable(L, -2);
	lua_pushcclosure(L, lpack, 2);
	packet_new(L, PACKET_MT, 0);
	lua_pushliteral(L, UNAVAILABLE);
	lua_pushcclosure(L, lprotected, 3);
	lua_setfield(L, -2, "pack");
	lua_getfield(L, -1, "pretty");
	lua_pushcclosure(L, lpackstring, 1);
	lua_pushliteral(L, UNAVAILABLE);
	lua_pushcclosure(L, lprotected, 2);
	lua_setfield(L, -2, "packstring");
	return 1;
}
