#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

int luaopen_dump(lua_State *L);

/* ---- 计时 ---- */

static double
now_sec(void) {
#ifdef _WIN32
	static LARGE_INTEGER freq;
	LARGE_INTEGER c;
	if (freq.QuadPart == 0) {
		QueryPerformanceFrequency(&freq);
	}
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* ---- RNG ---- */

static uint32_t rng;

static uint32_t
rnd(void) {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static int
rnd_range(int lo, int hi) {
	return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* ---- 随机造表 ---- */

static size_t nodes_built;

static const char *ident_keys[] = {
	"id", "name", "value", "count", "flag", "data", "item", "node",
	"left", "right", "meta", "type", "code", "score", "extra",
};

static void push_random_value(lua_State *L, int depth_left, int breadth);

static void
push_random_string(lua_State *L) {
	char buf[64];
	int kind = rnd_range(0, 4);
	int n, i;

	switch (kind) {
	case 0:
		n = sprintf(buf, "str_%u", rnd());
		break;
	case 1:
		n = sprintf(buf, "line\\nwith\\t\"quote\"_%u", rnd() & 0xffff);
		break;
	case 2:
		n = sprintf(buf, "key %u space", rnd() & 0xfff);
		break;
	default:
		n = rnd_range(4, 24);
		for (i = 0; i < n; i++) {
			buf[i] = (char)rnd_range(32, 126);
		}
		buf[n] = '\0';
		break;
	}
	lua_pushlstring(L, buf, (size_t)n);
}

static void
push_random_table(lua_State *L, int depth_left, int breadth) {
	int i;
	int array_n = breadth / 2;
	int hash_n = breadth - array_n;

	if (!lua_checkstack(L, 16)) {
		fprintf(stderr, "lua_checkstack failed\n");
		exit(1);
	}
	lua_createtable(L, array_n, hash_n);
	nodes_built++;

	for (i = 1; i <= array_n; i++) {
		push_random_value(L, depth_left - 1, breadth);
		lua_rawseti(L, -2, i);
	}

	for (i = 0; i < hash_n; i++) {
		char buf[40];
		int key_kind = rnd_range(0, 2);
		/* 用唯一前缀避免键碰撞把表写瘪，保证压测数据量真实 */
		if (key_kind == 0) {
			sprintf(buf, "%s_%d",
				ident_keys[rnd_range(0, (int)(sizeof(ident_keys) / sizeof(ident_keys[0]) - 1))],
				i);
			lua_pushstring(L, buf);
		} else if (key_kind == 1) {
			lua_pushinteger(L, 1000 + i);
		} else {
			sprintf(buf, "k %d %u", i, rnd() & 0xfff);
			lua_pushstring(L, buf);
		}
		push_random_value(L, depth_left - 1, breadth);
		lua_settable(L, -3);
	}
}

static void
push_random_value(lua_State *L, int depth_left, int breadth) {
	int t;

	nodes_built++;
	if (depth_left <= 0) {
		t = rnd_range(0, 4); /* 不再造表 */
	} else {
		t = rnd_range(0, 6);
	}

	switch (t) {
	case 0:
		lua_pushnil(L);
		break;
	case 1:
		lua_pushboolean(L, (int)(rnd() & 1));
		break;
	case 2:
		lua_pushinteger(L, (lua_Integer)(int32_t)rnd());
		break;
	case 3:
		lua_pushnumber(L, (lua_Number)(rnd() % 100000) / 100.0);
		break;
	case 4:
		push_random_string(L);
		break;
	default:
		push_random_table(L, depth_left, breadth);
		break;
	}
}

/* 给表注入自环：t._cycle = t */
static void
inject_cycle(lua_State *L, int index) {
	index = lua_absindex(L, index);
	lua_pushvalue(L, index);
	lua_setfield(L, index, "_cycle");
}

/* ---- dump 调用 ---- */

enum {
	MODE_PACKSTRING = 0,
	MODE_PACK = 1
};

static size_t
call_dump(lua_State *L, int mod_idx, int value_idx, int mode) {
	size_t len = 0;

	mod_idx = lua_absindex(L, mod_idx);
	value_idx = lua_absindex(L, value_idx);

	if (mode == MODE_PACKSTRING) {
		lua_getfield(L, mod_idx, "packstring");
		lua_pushvalue(L, value_idx);
		if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
			fprintf(stderr, "packstring error: %s\n", lua_tostring(L, -1));
			exit(1);
		}
		(void)lua_tolstring(L, -1, &len);
		lua_pop(L, 1);
		return len;
	}

	lua_getfield(L, mod_idx, "pack");
	lua_pushvalue(L, value_idx);
	if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
		fprintf(stderr, "pack error: %s\n", lua_tostring(L, -1));
		exit(1);
	}
	/* stack: ptr, sz */
	len = (size_t)lua_tointeger(L, -1);
	lua_getfield(L, mod_idx, "trash");
	lua_pushvalue(L, -3); /* ptr */
	lua_pushvalue(L, -3); /* sz */
	lua_call(L, 2, 0);
	lua_pop(L, 2);
	return len;
}

static void
print_result(const char *name, size_t out_bytes, double sec, int dumps) {
	double mb = (double)out_bytes / (1024.0 * 1024.0);
	double mbs = sec > 0 ? mb / sec : 0;
	double dps = sec > 0 ? (double)dumps / sec : 0;
	printf("%-10s  dumps=%-7d  out=%7.2f MB  time=%8.3f ms  %8.0f dumps/s  %7.1f MB/s\n",
		name, dumps, mb, sec * 1000.0, dps, mbs);
}

/* ---- 场景 ---- */

static void
bench_large(lua_State *L, int mod_idx, int mode, int depth, int breadth, int rounds) {
	size_t total_out = 0;
	double t0, t1;
	int i;
	int table_idx;

	nodes_built = 0;
	push_random_table(L, depth, breadth);
	inject_cycle(L, -1);
	table_idx = lua_gettop(L);

	printf("[large]     depth=%d breadth=%d nodes~=%lu rounds=%d\n",
		depth, breadth, (unsigned long)nodes_built, rounds);

	/* warmup */
	(void)call_dump(L, mod_idx, table_idx, mode);

	t0 = now_sec();
	for (i = 0; i < rounds; i++) {
		total_out += call_dump(L, mod_idx, table_idx, mode);
	}
	t1 = now_sec();

	print_result("large", total_out, t1 - t0, rounds);
	printf("            avg out/dump = %.2f KB\n",
		(double)(total_out / (size_t)rounds) / 1024.0);
	lua_pop(L, 1);
}

static void
bench_throughput(lua_State *L, int mod_idx, int mode, int n_tables, int depth, int breadth, int iters) {
	size_t total_out = 0;
	double t0, t1;
	int i;
	int base;
	int bag; /* 容器表，避免上千张表压在 Lua 栈上 */

	base = lua_gettop(L);
	lua_createtable(L, n_tables, 0);
	bag = lua_gettop(L);
	nodes_built = 0;
	for (i = 1; i <= n_tables; i++) {
		push_random_table(L, depth, breadth);
		if ((rnd() & 7) == 0) {
			inject_cycle(L, -1);
		}
		lua_rawseti(L, bag, i);
	}
	printf("[thruput]   tables=%d depth=%d breadth=%d nodes~=%lu iters=%d\n",
		n_tables, depth, breadth, (unsigned long)nodes_built, iters);

	/* warmup */
	for (i = 1; i <= n_tables; i++) {
		lua_rawgeti(L, bag, i);
		(void)call_dump(L, mod_idx, -1, mode);
		lua_pop(L, 1);
	}

	t0 = now_sec();
	for (i = 0; i < iters; i++) {
		lua_rawgeti(L, bag, (i % n_tables) + 1);
		total_out += call_dump(L, mod_idx, -1, mode);
		lua_pop(L, 1);
	}
	t1 = now_sec();

	print_result("thruput", total_out, t1 - t0, iters);
	lua_settop(L, base);
}

static void
bench_wide_array(lua_State *L, int mod_idx, int mode, int n, int rounds) {
	size_t total_out = 0;
	double t0, t1;
	int i;
	int table_idx;

	lua_createtable(L, n, 0);
	for (i = 1; i <= n; i++) {
		int t = rnd_range(0, 3);
		switch (t) {
		case 0:
			lua_pushinteger(L, i);
			break;
		case 1:
			lua_pushnumber(L, (lua_Number)i * 0.5);
			break;
		case 2:
			lua_pushboolean(L, i & 1);
			break;
		default:
			lua_pushfstring(L, "v_%d", i);
			break;
		}
		lua_rawseti(L, -2, i);
	}
	table_idx = lua_gettop(L);
	printf("[array]     len=%d rounds=%d\n", n, rounds);

	(void)call_dump(L, mod_idx, table_idx, mode);
	t0 = now_sec();
	for (i = 0; i < rounds; i++) {
		total_out += call_dump(L, mod_idx, table_idx, mode);
	}
	t1 = now_sec();
	print_result("array", total_out, t1 - t0, rounds);
	lua_pop(L, 1);
}

static void
run_suite(lua_State *L, int mod_idx, int mode, uint32_t seed) {
	rng = seed ? seed : 1;
	printf("--- mode=%s ---\n", mode == MODE_PACK ? "pack+trash" : "packstring");

	bench_large(L, mod_idx, mode, 5, 12, 50);
	bench_wide_array(L, mod_idx, mode, 100000, 20);
	bench_throughput(L, mod_idx, mode, 1000, 3, 8, 20000);

	printf("\n--- heavier large table ---\n");
	rng = seed ? seed : 1;
	bench_large(L, mod_idx, mode, 6, 16, 10);
	printf("\n");
}

int
main(int argc, char **argv) {
	lua_State *L;
	int mod_idx;
	uint32_t seed = 0xC0FFEE;

	if (argc >= 2) {
		seed = (uint32_t)strtoul(argv[1], NULL, 0);
	}

	L = luaL_newstate();
	if (!L) {
		fprintf(stderr, "luaL_newstate failed\n");
		return 1;
	}
	luaL_openlibs(L);

	luaopen_dump(L);
	mod_idx = lua_gettop(L);

	printf("=== lua-dump bench (seed=0x%x) ===\n\n", seed);
	run_suite(L, mod_idx, MODE_PACKSTRING, seed);
	run_suite(L, mod_idx, MODE_PACK, seed);

	lua_close(L);
	return 0;
}
