#include <lua.h>    
#include <lauxlib.h>  
#include <stdlib.h>   
#include <string.h>   
#include <stdio.h>  

#define INDENT_CHAR ' '
#define INDENT_WIDTH 2
#define MAX_INDENT_LEVEL 128 

#define INITIAL_BUFFER_SIZE 4096

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
    char *data;      
    size_t size;       
    size_t capacity;    
    int indent_level;   
} buffer;


static void
buffer_init(buffer *buffer) {
    buffer->capacity = INITIAL_BUFFER_SIZE; 
    buffer->data = (char *)malloc(buffer->capacity);
    buffer->size = 0;
    buffer->indent_level = 0;
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
}


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

static void
buffer_append_binary(buffer *buffer, const void *data, size_t len, int indent_level) {
    int spaces = indent_level * INDENT_WIDTH;
    if (indent_level > MAX_INDENT_LEVEL) {
        spaces = MAX_INDENT_LEVEL * INDENT_WIDTH;
    }
    buffer_ensure_capacity(buffer, len + spaces + 1);
    memcpy(buffer->data + buffer->size, indent_cache, spaces);
    memcpy(buffer->data + buffer->size + spaces, data, len);
    buffer->size += len + spaces;
    buffer->data[buffer->size] = '\0';
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

static void serialize_value(lua_State *L, buffer *buffer, int index, int indent_level);

static void
serialize_nil(buffer *buffer, int indent_level) {
    buffer_append_binary(buffer, STR_NIL, STR_NIL_LEN, indent_level);
}

static void
serialize_boolean(buffer *buffer, int boolean, int indent_level) {
    if (boolean) {
        buffer_append_binary(buffer, STR_TRUE, STR_TRUE_LEN, indent_level);
    } else {
        buffer_append_binary(buffer, STR_FALSE, STR_FALSE_LEN, indent_level);
    }
}

static void
serialize_number(lua_State *L, buffer *buffer, int index, int indent_level) {
    if (lua_isinteger(L, index)) {
        lua_Integer i = lua_tointeger(L, index);
        char temp[32];
        int len = 0;
        
        if (i < 0) {
            temp[len++] = '-';
            i = -i; 
        }
        
        if (i == 0) {
            temp[len++] = '0';
        } else {
            char digits[20]; 
            int digit_count = 0;
            while (i > 0) {
                digits[digit_count++] = '0' + (i % 10);
                i /= 10;
            }
            
            for (int j = digit_count - 1; j >= 0; j--) {
                temp[len++] = digits[j];
            }
        }
        
        buffer_append_binary(buffer, temp, len, indent_level);
    } else {
        lua_Number n = lua_tonumber(L, index);
        char temp[32];
        int len = snprintf(temp, sizeof(temp), "%g", n);
        buffer_append_binary(buffer, temp, len, indent_level);
    }
}

static void
serialize_string(buffer *buffer, const char *str, size_t len, int is_key, int indent_level) {
    if (!is_key) {
        // 直接向缓冲区添加引号和字符串内容
        int spaces = indent_level * INDENT_WIDTH;
        if (indent_level > MAX_INDENT_LEVEL) {
            spaces = MAX_INDENT_LEVEL * INDENT_WIDTH;
        }
        buffer_ensure_capacity(buffer, len + spaces + 3); // 2个引号 + 内容 + 空终止符
        memcpy(buffer->data + buffer->size, indent_cache, spaces);
        buffer->size += spaces;
        buffer->data[buffer->size++] = '"';
        memcpy(buffer->data + buffer->size, str, len);
        buffer->size += len;
        buffer->data[buffer->size++] = '"';
        buffer->data[buffer->size] = '\0';
    } else {
        buffer_append_binary(buffer, str, len, indent_level);
    }
}

static void serialize_array_part(lua_State *L, buffer *buffer, int index, size_t array_size) {
    for (size_t i = 1; i <= array_size; i++) {
        lua_rawgeti(L, index, i);
        serialize_value(L, buffer, -1, buffer->indent_level);
        lua_pop(L, 1);
        
        if (i < array_size) {
            buffer_append_binary(buffer, STR_COMMA_NEWLINE, STR_COMMA_NEWLINE_LEN, 0);
        }
    }
}

static void serialize_hash_part(lua_State *L, buffer *buffer, int index, size_t array_size, int has_array) {
    int has_hash = 0;
    
    lua_pushnil(L);
    int indent_level = buffer->indent_level;
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
            buffer_append_binary(buffer, STR_COMMA_NEWLINE, STR_COMMA_NEWLINE_LEN, 0);
            has_hash = 1;
        } else if (has_hash) {
            buffer_append_binary(buffer, STR_COMMA_NEWLINE, STR_COMMA_NEWLINE_LEN, 0);
        } else {
            has_hash = 1;
        }
        
        
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t len;
            const char *str = lua_tolstring(L, -2, &len);
            serialize_string(buffer, str, len, 1, indent_level);
        } else {
            serialize_value(L, buffer, -2, indent_level);
        }
        
        buffer_append_binary(buffer, STR_EQUALS, STR_EQUALS_LEN, 0);
        
        serialize_value(L, buffer, -1, 0);
        
        lua_pop(L, 1);
    }
}

static void
serialize_table(lua_State *L, buffer *buffer, int index, int indent_level) {
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }
    
    if (!lua_istable(L, index)) {
        buffer_append_binary(buffer, STR_NIL_NOT_TABLE, STR_NIL_NOT_TABLE_LEN, indent_level);
        return;
    }
    
    buffer_append_binary(buffer, STR_TABLE_BEGIN, STR_TABLE_BEGIN_LEN, indent_level);
    buffer_indent(buffer);
    
    size_t array_size = lua_rawlen(L, index);
    int has_array = array_size > 0;
    
    if (has_array) {
        serialize_array_part(L, buffer, index, array_size);
    }
    
    serialize_hash_part(L, buffer, index, array_size, has_array);
    
    buffer_append_binary(buffer, STR_NEWLINE, STR_NEWLINE_LEN, 0);
    buffer_unindent(buffer);
    buffer_append_binary(buffer, STR_TABLE_END, STR_TABLE_END_LEN, buffer->indent_level);
}

static void
serialize_userdata(buffer *buffer, void *p, int indent_level) {
    char temp[64];
    int len = snprintf(temp, sizeof(temp), "userdata(%p)", p);
    buffer_append_binary(buffer, temp, len, indent_level);
}

static void
serialize_value(lua_State *L, buffer *buffer, int index, int indent_level) {
    int type = lua_type(L, index);
    
    switch (type) {
        case LUA_TNIL:
            serialize_nil(buffer, indent_level);
            break;
        case LUA_TNUMBER:
            serialize_number(L, buffer, index, indent_level);
            break;
        case LUA_TBOOLEAN:
            serialize_boolean(buffer, lua_toboolean(L, index), indent_level);
            break;
        case LUA_TSTRING: {
            size_t len;
            const char *str = lua_tolstring(L, index, &len);
            serialize_string(buffer, str, len, 0, indent_level);
            break;
        }
        case LUA_TTABLE:
            serialize_table(L, buffer, index, indent_level);
            break;
        case LUA_TLIGHTUSERDATA:
            serialize_userdata(buffer, lua_touserdata(L, index), indent_level);
            break;
        default: {
            // const char* type_name = lua_typename(L, type);
            // int len = strlen(type_name);
            // buffer_append_binary(buffer, type_name, len);
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
            buffer_append_binary(&buffer, STR_SPACE, STR_SPACE_LEN, 0);
        }
        serialize_value(L, &buffer, i, 0);
    }
    
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