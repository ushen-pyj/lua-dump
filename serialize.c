#include <lua.h>    
#include <lauxlib.h>  
#include <stdlib.h>   
#include <string.h>   
#include <stdio.h>  

#define INDENT_CHAR ' '
#define INDENT_WIDTH 2
#define MAX_INDENT_LEVEL 32 

#define INITIAL_BUFFER_SIZE 4096
#define CHUNK_SIZE 4096

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define GET_INDENT(v) (MIN(v->indent_level, MAX_INDENT_LEVEL) * INDENT_WIDTH)

#define STR_NIL "nil"
#define STR_TRUE "true"
#define STR_FALSE "false"
#define STR_COMMA_NEWLINE ",\n"
#define STR_EQUALS " = "
#define STR_NIL_NOT_TABLE "nil /* not a table */"
#define STR_TABLE_BEGIN "{\n"
#define STR_NEWLINE "\n"
#define STR_TABLE_END "}"
#define STR_UNSUPPORTED "unsupported "
#define STR_SPACE " "
#define STR_QUOTE "\""
#define STR_NIL_LEN 3
#define STR_TRUE_LEN 4
#define STR_FALSE_LEN 5
#define STR_COMMA_NEWLINE_LEN 2
#define STR_EQUALS_LEN 3
#define STR_NIL_NOT_TABLE_LEN 18
#define STR_TABLE_BEGIN_LEN 2
#define STR_NEWLINE_LEN 1
#define STR_TABLE_END_LEN 1
#define STR_UNSUPPORTED_LEN 12
#define STR_SPACE_LEN 1
#define STR_QUOTE_LEN 1

static char indent_cache[MAX_INDENT_LEVEL * INDENT_WIDTH + 1];
static int indent_cache_initialized = 0;

static void init_indent_cache() {
    if (!indent_cache_initialized) {
        memset(indent_cache, INDENT_CHAR, MAX_INDENT_LEVEL * INDENT_WIDTH);
        indent_cache[MAX_INDENT_LEVEL * INDENT_WIDTH] = '\0';
        indent_cache_initialized = 1;
    }
}

typedef struct buffer {
    char *data;       // 最终输出缓冲区
    size_t size;      // 当前数据大小
    size_t capacity;  // 当前分配容量
    int indent_level; // 缩进级别
    
    char chunk[CHUNK_SIZE]; // 块缓冲区
    size_t chunk_used;      // 块缓冲区已使用大小
} buffer;


static void
buffer_init(buffer *buffer) {
    buffer->capacity = INITIAL_BUFFER_SIZE; 
    buffer->data = (char *)malloc(buffer->capacity);
    buffer->size = 0;
    buffer->indent_level = 0;
    buffer->chunk_used = 0;
    if (buffer->data) {
        buffer->data[0] = '\0';
    }
}


static void
buffer_free(buffer *buffer) {
    if (buffer->data) {
        free(buffer->data);
        buffer->data = NULL;
    }
    buffer->size = 0;
    buffer->capacity = 0;
    buffer->chunk_used = 0;
}

// 在使用buffer_flush_chunk之前先声明buffer_ensure_capacity
static void
buffer_ensure_capacity(buffer *buffer, size_t additional) {
    if (buffer->size + additional >= buffer->capacity) {
        size_t new_capacity = buffer->capacity * 2;
        while (buffer->size + additional >= new_capacity) {
            new_capacity *= 2;
        }
        char *new_data = (char *)realloc(buffer->data, new_capacity);
        if (new_data) {
            buffer->data = new_data;
            buffer->capacity = new_capacity;
        }
    }
}

// 将块缓冲区刷新到主缓冲区
static void
buffer_flush_chunk(buffer *buffer) {
    if (buffer->chunk_used > 0) {
        buffer_ensure_capacity(buffer, buffer->chunk_used + 1);
        memcpy(buffer->data + buffer->size, buffer->chunk, buffer->chunk_used);
        buffer->size += buffer->chunk_used;
        buffer->data[buffer->size] = '\0';
        buffer->chunk_used = 0;
    }
}

static void
buffer_append_binary(buffer *buffer, const void *data, size_t len) {
    // 如果数据太大无法放入块缓冲区，先刷新块缓冲区
    if (buffer->chunk_used + len > CHUNK_SIZE) {
        // 刷新现有块缓冲区
        buffer_flush_chunk(buffer);
        
        // 对于非常大的数据，直接添加到主缓冲区
        if (len > CHUNK_SIZE) {
            buffer_ensure_capacity(buffer, len + 1);
            memcpy(buffer->data + buffer->size, data, len);
            buffer->size += len;
            buffer->data[buffer->size] = '\0';
            return;
        }
    }
    
    // 将数据添加到块缓冲区
    memcpy(buffer->chunk + buffer->chunk_used, data, len);
    buffer->chunk_used += len;
}

static void
buffer_append_binary_array(buffer *buffer, const void **data_array, const size_t *len_array, size_t count) {
    size_t total_len = 0;
    for (size_t i = 0; i < count; i++) {
        total_len += len_array[i];
    }
    
    // 如果总数据大小无法放入块缓冲区，先刷新块缓冲区
    if (buffer->chunk_used + total_len > CHUNK_SIZE) {
        buffer_flush_chunk(buffer);
        
        // 对于非常大的数据，直接添加到主缓冲区
        if (total_len > CHUNK_SIZE) {
            buffer_ensure_capacity(buffer, total_len + 1);
            for (size_t i = 0; i < count; i++) {
                memcpy(buffer->data + buffer->size, data_array[i], len_array[i]);
                buffer->size += len_array[i];
            }
            buffer->data[buffer->size] = '\0';
            return;
        }
    }
    
    // 将数据添加到块缓冲区
    for (size_t i = 0; i < count; i++) {
        memcpy(buffer->chunk + buffer->chunk_used, data_array[i], len_array[i]);
        buffer->chunk_used += len_array[i];
    }
}

static void
buffer_append_indent(buffer *buffer) {
    int spaces = GET_INDENT(buffer);
    
    // 检查块缓冲区容量
    if (buffer->chunk_used + spaces > CHUNK_SIZE) {
        buffer_flush_chunk(buffer);
    }
    
    memcpy(buffer->chunk + buffer->chunk_used, indent_cache, spaces);
    buffer->chunk_used += spaces;
}

static void
buffer_indent(buffer *buffer) {
    buffer->indent_level++;
}

static void
buffer_unindent(buffer *buffer) {
    if (buffer->indent_level > 0) {
        buffer->indent_level--;
    }
}

static void serialize_value(lua_State *L, buffer *buffer, int index);

static void
serialize_nil(buffer *buffer) {
    buffer_append_binary(buffer, STR_NIL, STR_NIL_LEN);
}

static void
serialize_boolean(buffer *buffer, int boolean) {
    if (boolean) {
        buffer_append_binary(buffer, STR_TRUE, STR_TRUE_LEN);
    } else {
        buffer_append_binary(buffer, STR_FALSE, STR_FALSE_LEN);
    }
}

static void
serialize_number(lua_State *L, buffer *buffer, int index) {
    // 确保块缓冲区有足够空间存储数字（数字转字符串最多32字节）
    if (buffer->chunk_used + 32 > CHUNK_SIZE) {
        buffer_flush_chunk(buffer);
    }
    
    if (lua_isinteger(L, index)) {
        lua_Integer i = lua_tointeger(L, index);
        
        int len = snprintf(buffer->chunk + buffer->chunk_used, 32, "%lld", i);
        buffer->chunk_used += len;
    } else {
        lua_Number n = lua_tonumber(L, index);
        int len = snprintf(buffer->chunk + buffer->chunk_used, 32, "%g", n);
        buffer->chunk_used += len;
    }
}

static void
serialize_string(buffer *buffer, const char *str, size_t len) {
    const void *data_array[3] = {STR_QUOTE, str, STR_QUOTE};
    size_t len_array[3] = {STR_QUOTE_LEN, len, STR_QUOTE_LEN};
    buffer_append_binary_array(buffer, data_array, len_array, 3);
}

static void serialize_array_part(lua_State *L, buffer *buffer, int index, size_t array_size) {
    for (size_t i = 1; i <= array_size; i++) {
        if (i > 1) {
            const void *data_array[2] = {STR_COMMA_NEWLINE, indent_cache};
            size_t len_array[2] = {STR_COMMA_NEWLINE_LEN, GET_INDENT(buffer)};
            buffer_append_binary_array(buffer, data_array, len_array, 2);
        }else{
            buffer_append_indent(buffer);
        }
        
        
        lua_rawgeti(L, index, i);
        serialize_value(L, buffer, -1);
        lua_pop(L, 1);
        
    }
}

static void serialize_hash_part(lua_State *L, buffer *buffer, int index, size_t array_size, int has_array) {
    int has_hash = 0;
    
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) == LUA_TNUMBER && 
            lua_isinteger(L, -2)) {
            lua_Integer key = lua_tointeger(L, -2);
            if (key > 0 && key <= (lua_Integer)array_size) {
                lua_pop(L, 1);
                continue;
            }
        }

        if (has_array && !has_hash) {
            buffer_append_binary(buffer, STR_COMMA_NEWLINE, STR_COMMA_NEWLINE_LEN);
            has_hash = 1;
        } else if (has_hash) {
            buffer_append_binary(buffer, STR_COMMA_NEWLINE, STR_COMMA_NEWLINE_LEN);
        } else {
            has_hash = 1;
        }
        
        buffer_append_indent(buffer);
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t len;
            const char *str = lua_tolstring(L, -2, &len);
            buffer_append_binary(buffer, str, len);
        }else {
            serialize_value(L, buffer, -2);
        }
        
        buffer_append_binary(buffer, STR_EQUALS, STR_EQUALS_LEN);
        
        serialize_value(L, buffer, -1);
        
        lua_pop(L, 1);
    }
}

static void
serialize_table(lua_State *L, buffer *buffer, int index) {
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }
    
    if (!lua_istable(L, index)) {
        buffer_append_binary(buffer, STR_NIL_NOT_TABLE, STR_NIL_NOT_TABLE_LEN);
        return;
    }
    buffer_append_binary(buffer, STR_TABLE_BEGIN, STR_TABLE_BEGIN_LEN);
    buffer_indent(buffer);
    
    size_t array_size = lua_rawlen(L, index);
    int has_array = array_size > 0;
    
    if (has_array) {
        serialize_array_part(L, buffer, index, array_size);
    }
    
    serialize_hash_part(L, buffer, index, array_size, has_array);
    
    buffer_unindent(buffer);
    const void *data_array[3] = {STR_NEWLINE, indent_cache, STR_TABLE_END};
    size_t len_array[3] = {STR_NEWLINE_LEN, GET_INDENT(buffer), STR_TABLE_END_LEN};
    buffer_append_binary_array(buffer, data_array, len_array, 3);
}

static void
serialize_userdata(buffer *buffer, void *p) {
    char temp[64];
    int len = snprintf(temp, sizeof(temp), "userdata(%p)", p);
    buffer_append_binary(buffer, temp, len);
}

static void
serialize_value(lua_State *L, buffer *buffer, int index) {
    int type = lua_type(L, index);
    
    switch (type) {
        case LUA_TNIL:
            serialize_nil(buffer);
            break;
        case LUA_TNUMBER:
            serialize_number(L, buffer, index);
            break;
        case LUA_TBOOLEAN:
            serialize_boolean(buffer, lua_toboolean(L, index));
            break;
        case LUA_TSTRING: {
            size_t len;
            const char *str = lua_tolstring(L, index, &len);
            serialize_string(buffer, str, len);
            break;
        }
        case LUA_TTABLE:
            serialize_table(L, buffer, index);
            break;
        case LUA_TLIGHTUSERDATA:
            serialize_userdata(buffer, lua_touserdata(L, index));
            break;
        default: {
            const char* type_name = lua_typename(L, type);
            int len = strlen(type_name);
            buffer_append_binary(buffer, type_name, len);
            break;
        }
    }
}

static int
serialize_pretty(lua_State *L) {
    buffer buffer;
    buffer_init(&buffer);
    
    int nargs = lua_gettop(L);
    
    for (int i = 1; i <= nargs; i++) {
        if (i > 1) {
            buffer_append_binary(&buffer, STR_SPACE, STR_SPACE_LEN);
        }
        serialize_value(L, &buffer, i);
    }
    
    // 确保所有数据都刷新到主缓冲区
    buffer_flush_chunk(&buffer);
    
    lua_pushlstring(L, buffer.data, buffer.size);
    
    buffer_free(&buffer);
    return 1;
}

int
luaopen_serialize(lua_State *L) {
    init_indent_cache();
    
    luaL_Reg l[] = {
        { "serialize_pretty", serialize_pretty },
        { NULL, NULL },
    };
    luaL_newlib(L, l);
    return 1;
} 