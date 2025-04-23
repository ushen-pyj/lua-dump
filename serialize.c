#include <lua.h>    
#include <lauxlib.h>  
#include <stdlib.h>   
#include <stdint.h>  
#include <assert.h>    
#include <string.h>   
#include <stdio.h>  

#define INDENT_CHAR ' '
#define INDENT_WIDTH 2

typedef struct pretty_buffer {
    char *data;      
    size_t size;       
    size_t capacity;    
    int indent_level;   
} pretty_buffer;


static void
buffer_init(pretty_buffer *buffer) {
    buffer->capacity = 1024; 
    buffer->data = (char *)malloc(buffer->capacity);
    buffer->size = 0;
    buffer->indent_level = 0;
    if (buffer->data) {
        buffer->data[0] = '\0';
    }
}


static void
buffer_free(pretty_buffer *buffer) {
    if (buffer->data) {
        free(buffer->data);
        buffer->data = NULL;
    }
    buffer->size = 0;
    buffer->capacity = 0;
}


static void
buffer_ensure_capacity(pretty_buffer *buffer, size_t additional) {
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
buffer_append_string(pretty_buffer *buffer, const char *str) {
    size_t len = strlen(str);
    buffer_ensure_capacity(buffer, len + 1);
    strcpy(buffer->data + buffer->size, str);
    buffer->size += len;
}

static void
buffer_append_format(pretty_buffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    va_list args_copy;
    va_copy(args_copy, args);
    
    int len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    
    if (len < 0) {
        va_end(args);
        return;
    }
    
    buffer_ensure_capacity(buffer, len + 1);
    
    vsnprintf(buffer->data + buffer->size, len + 1, format, args);
    buffer->size += len;
    
    va_end(args);
}

static void
buffer_append_indent(pretty_buffer *buffer) {
    int spaces = buffer->indent_level * INDENT_WIDTH;
    buffer_ensure_capacity(buffer, spaces + 1);
    
    for (int i = 0; i < spaces; i++) {
        buffer->data[buffer->size++] = INDENT_CHAR;
    }
    buffer->data[buffer->size] = '\0';
}

static void
buffer_indent(pretty_buffer *buffer) {
    buffer->indent_level++;
}

static void
buffer_unindent(pretty_buffer *buffer) {
    if (buffer->indent_level > 0) {
        buffer->indent_level--;
    }
}

static void serialize_value(lua_State *L, pretty_buffer *buffer, int index);

static void
serialize_nil(pretty_buffer *buffer) {
    buffer_append_string(buffer, "nil");
}

static void
serialize_boolean(pretty_buffer *buffer, int boolean) {
    buffer_append_string(buffer, boolean ? "true" : "false");
}

static void
serialize_number(lua_State *L, pretty_buffer *buffer, int index) {
    if (lua_isinteger(L, index)) {
        lua_Integer i = lua_tointeger(L, index);
        buffer_append_format(buffer, "%lld", (long long)i);
    } else {
        lua_Number n = lua_tonumber(L, index);
        buffer_append_format(buffer, "%g", n);
    }
}

static void
serialize_string(pretty_buffer *buffer, const char *str, size_t len) {
    buffer_append_string(buffer, "\"");
    
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '\\': buffer_append_string(buffer, "\\\\"); break;
            case '\"': buffer_append_string(buffer, "\\\""); break;
            case '\n': buffer_append_string(buffer, "\\n"); break;
            case '\r': buffer_append_string(buffer, "\\r"); break;
            case '\t': buffer_append_string(buffer, "\\t"); break;
            default:
                if ((unsigned char)c < 32 || c == 127) {
                    buffer_append_format(buffer, "\\x%02x", (unsigned char)c);
                } else {
                    buffer_ensure_capacity(buffer, 2);
                    buffer->data[buffer->size++] = c;
                    buffer->data[buffer->size] = '\0';
                }
                break;
        }
    }
    
    buffer_append_string(buffer, "\"");
}

static void
serialize_table(lua_State *L, pretty_buffer *buffer, int index) {
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }
    
    if (!lua_istable(L, index)) {
        buffer_append_string(buffer, "nil /* not a table */");
        return;
    }
    
    buffer_append_string(buffer, "{\n");
    buffer_indent(buffer);
    
    size_t array_size = lua_rawlen(L, index);
    int has_array = array_size > 0;
    int has_hash = 0;
    
    for (size_t i = 1; i <= array_size; i++) {
        buffer_append_indent(buffer);
        
        lua_rawgeti(L, index, i);
        serialize_value(L, buffer, -1);
        lua_pop(L, 1);
        
        if (i < array_size) {
            buffer_append_string(buffer, ",\n");
        }
    }
    
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
            buffer_append_string(buffer, ",\n");
            has_hash = 1;
        } else if (has_hash) {
            buffer_append_string(buffer, ",\n");
        } else {
            has_hash = 1;
        }
        
        buffer_append_indent(buffer);
        
        buffer_append_string(buffer, "[");
        serialize_value(L, buffer, -2);
        buffer_append_string(buffer, "] = ");
        
        serialize_value(L, buffer, -1);
        
        lua_pop(L, 1);
    }
    
    buffer_append_string(buffer, "\n");
    buffer_unindent(buffer);
    buffer_append_indent(buffer);
    buffer_append_string(buffer, "}");
}

static void
serialize_userdata(pretty_buffer *buffer, void *p) {
    buffer_append_format(buffer, "userdata(%p)", p);
}

static void
serialize_value(lua_State *L, pretty_buffer *buffer, int index) {
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
        default:
            buffer_append_format(buffer, "<%s>", lua_typename(L, type));
            break;
    }
}

static int
serialize_pretty(lua_State *L) {
    pretty_buffer buffer;
    buffer_init(&buffer);
    
    int nargs = lua_gettop(L);
    
    for (int i = 1; i <= nargs; i++) {
        if (i > 1) {
            buffer_append_string(&buffer, "\n");
        }
        serialize_value(L, &buffer, i);
    }
    
    lua_pushlstring(L, buffer.data, buffer.size);
    
    buffer_free(&buffer);
    return 1;
}

int
luaopen_serialize(lua_State *L) {
    luaL_Reg l[] = {
        { "serialize_pretty", serialize_pretty },
        { NULL, NULL },
    };
    luaL_newlib(L, l);
    return 1;
} 
