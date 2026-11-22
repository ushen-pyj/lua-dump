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

static const char indent_cache[MAX_INDENT + 1] =
	"                                "
	"                                ";

typedef struct {
	lua_State *L;
	char *data;
	size_t n;
	size_t size;
	int pretty;
	size_t line_start;
} dumpbuf;

#define DUMP_BUF_MT "dump.buf"

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
buf_release(dumpbuf *B) {
	free(B->data);
	B->data = NULL;
	B->n = 0;
	B->size = 0;
}

static int
lbuf_close(lua_State *L) {
	dumpbuf *B = (dumpbuf *)luaL_checkudata(L, 1, DUMP_BUF_MT);
	buf_release(B);
	return 0;
}

static dumpbuf *
buf_new(lua_State *L, int pretty) {
	dumpbuf *B = (dumpbuf *)lua_newuserdatauv(L, sizeof(*B), 0);
	B->L = L;
	B->data = NULL;
	B->n = 0;
	B->size = 0;
	B->pretty = pretty;
	B->line_start = 0;
	luaL_setmetatable(L, DUMP_BUF_MT);
	lua_toclose(L, -1);

	B->data = (char *)malloc(BUF_INIT);
	if (B->data == NULL) {
		luaL_error(L, "dump: out of memory");
	}
	B->size = BUF_INIT;
	return B;
}

static void *
buf_detach(dumpbuf *B, size_t *sz) {
	void *p = B->data;
	*sz = B->n;
	if (p != NULL && B->n < B->size) {
		void *q = realloc(p, B->n);
		if (q != NULL) {
			p = q;
		}
	}
	B->data = NULL;
	B->n = 0;
	B->size = 0;
	return p;
}

static void
buf_grow(dumpbuf *B, size_t need) {
	size_t newsize = B->size;
	char *p;
	if (need > (size_t)-1 - B->n) {
		luaL_error(B->L, "dump: buffer overflow");
	}
	while (B->n + need > newsize) {
		if (newsize > (size_t)-1 / 2) {
			newsize = B->n + need;
			break;
		}
		newsize *= 2;
	}
	p = (char *)realloc(B->data, newsize);
	if (p == NULL) {
		luaL_error(B->L, "dump: out of memory");
	}
	B->data = p;
	B->size = newsize;
}

/* s must not point into B->data: growth can move the allocation. */
static void
buf_add(dumpbuf *B, const char *s, size_t len) {
	if (len == 0) {
		return;
	}
	if (len > B->size - B->n) {
		buf_grow(B, len);
	}
	memcpy(B->data + B->n, s, len);
	B->n += len;
}

static void
buf_addch(dumpbuf *B, char c) {
	if (B->n == B->size) {
		buf_grow(B, 1);
	}
	B->data[B->n++] = c;
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
		B->data[split++] = ' ';
	}
	memmove(B->data + split, B->data + text, len);
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
	while (i < len) {
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
		if (is_ident(str, len)) {
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
	if (B->pretty) {
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
			int n = sprintf(buf, "%.14g", (double)lua_tonumber(L, index));
			if (n > 0) {
				buf_add(B, buf, (size_t)n);
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

		/* Ancestors stay rooted on the Lua stack, including across __pairs calls. */
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
		if (luaL_getmetafield(L, index, "__pairs") != LUA_TNIL) {
			lua_pushvalue(L, index);
			lua_call(L, 1, 3); /* iter, state, key */
			for (;;) {
				lua_pushvalue(L, -3);
				lua_pushvalue(L, -3);
				lua_pushvalue(L, -3);
				lua_call(L, 2, 2); /* nkey, value */
				if (lua_isnil(L, -2)) {
					lua_pop(L, 5);
					break;
				}
				dump_entry(L, B, 1, path, depth + 1, content, &previous_inline);
				lua_pop(L, 1);
				lua_remove(L, -2);
			}
		} else {
			size_t array_size = lua_rawlen(L, index);
			if (array_size > 0) {
				size_t i;
				for (i = 1; i <= array_size; i++) {
					lua_rawgeti(L, index, (lua_Integer)i);
					dump_entry(L, B, 0, path, depth + 1, content, &previous_inline);
					lua_pop(L, 1);
				}
			}
			lua_pushnil(L);
			while (lua_next(L, index) != 0) {
				if (lua_isinteger(L, -2)) {
					lua_Integer k = lua_tointeger(L, -2);
					if (k >= 1 && (lua_Unsigned)k <= array_size) {
						lua_pop(L, 1);
						continue;
					}
				}
				dump_entry(L, B, 1, path, depth + 1, content, &previous_inline);
				lua_pop(L, 1);
			}
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

	for (i = first; i <= nargs; i++) {
		if (i != first) {
			buf_addch(B, ' ');
		}
		dump_value(L, B, i, path, 0);
	}
}

static int
lpack(lua_State *L) {
	int nargs = lua_gettop(L);
	int pretty = lua_rawequal(L, 1, lua_upvalueindex(1));
	dumpbuf *B;
	size_t sz;
	void *ptr;

	if (nargs == pretty) {
		lua_pushlightuserdata(L, NULL);
		lua_pushinteger(L, 0);
		return 2;
	}

	B = buf_new(L, pretty);
	dump_args(L, B, nargs);
	if (B->n > (size_t)LUA_MAXINTEGER) {
		return luaL_error(L, "dump: buffer overflow");
	}
	ptr = buf_detach(B, &sz);
	lua_pushlightuserdata(L, ptr);
	lua_pushinteger(L, (lua_Integer)sz);
	return 2;
}

static int
lpackstring(lua_State *L) {
	int nargs = lua_gettop(L);
	int pretty = lua_rawequal(L, 1, lua_upvalueindex(1));
	dumpbuf *B;

	if (nargs == pretty) {
		lua_pushliteral(L, "");
		return 1;
	}
	B = buf_new(L, pretty);
	dump_args(L, B, nargs);
	lua_pushlstring(L, B->data, B->n);
	return 1;
}

static int
ltrash(lua_State *L) {
	switch (lua_type(L, 1)) {
	case LUA_TSTRING:
		break;
	case LUA_TLIGHTUSERDATA: {
		void *msg = lua_touserdata(L, 1);
		luaL_checkinteger(L, 2);
		free(msg);
		break;
	}
	default:
		return luaL_error(L, "dump.trash invalid param %s", lua_typename(L, lua_type(L, 1)));
	}
	return 0;
}

int
luaopen_dump(lua_State *L) {
	luaL_Reg l[] = {
		{ "pack", lpack },
		{ "packstring", lpackstring },
		{ "trash", ltrash },
		{ NULL, NULL },
	};

	luaL_checkversion(L);
	luaL_newmetatable(L, DUMP_BUF_MT);
	lua_pushcfunction(L, lbuf_close);
	lua_setfield(L, -2, "__close");
	/* A failed coroutine may be collected without coroutine.close. */
	lua_pushcfunction(L, lbuf_close);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	luaL_newlibtable(L, l);
	lua_newtable(L);  /* Identity token: ordinary values remain valid dump arguments. */
	lua_pushvalue(L, -1);
	lua_setfield(L, -3, "pretty");
	luaL_setfuncs(L, l, 1);
	return 1;
}
