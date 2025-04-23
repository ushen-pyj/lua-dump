#include <lua.h>        // Lua核心API
#include <lauxlib.h>    // Lua辅助库
#include <stdlib.h>     // 标准库函数
#include <stdint.h>     // 整数类型定义
#include <assert.h>     // 断言
#include <string.h>     // 字符串处理函数
#include <stdio.h>      // 标准输入输出

// 定义缩进的字符
#define INDENT_CHAR ' '
#define INDENT_WIDTH 2

// 写入缓冲区结构体定义
typedef struct pretty_buffer {
    char *data;         // 缓冲区数据
    size_t size;        // 当前大小
    size_t capacity;    // 容量
    int indent_level;   // 当前缩进级别
} pretty_buffer;

// 初始化缓冲区
static void
buffer_init(pretty_buffer *buffer) {
    buffer->capacity = 1024;  // 初始容量
    buffer->data = (char *)malloc(buffer->capacity);
    buffer->size = 0;
    buffer->indent_level = 0;
    if (buffer->data) {
        buffer->data[0] = '\0';
    }
}

// 释放缓冲区
static void
buffer_free(pretty_buffer *buffer) {
    if (buffer->data) {
        free(buffer->data);
        buffer->data = NULL;
    }
    buffer->size = 0;
    buffer->capacity = 0;
}

// 确保缓冲区有足够空间
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

// 向缓冲区添加字符串
static void
buffer_append_string(pretty_buffer *buffer, const char *str) {
    size_t len = strlen(str);
    buffer_ensure_capacity(buffer, len + 1);
    strcpy(buffer->data + buffer->size, str);
    buffer->size += len;
}

// 向缓冲区添加格式化字符串
static void
buffer_append_format(pretty_buffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    va_list args_copy;
    va_copy(args_copy, args);
    
    // 计算格式化后字符串的长度
    int len = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    
    if (len < 0) {
        va_end(args);
        return;
    }
    
    // 确保缓冲区有足够的空间
    buffer_ensure_capacity(buffer, len + 1);
    
    // 格式化字符串并添加到缓冲区
    vsnprintf(buffer->data + buffer->size, len + 1, format, args);
    buffer->size += len;
    
    va_end(args);
}

// 添加缩进
static void
buffer_append_indent(pretty_buffer *buffer) {
    int spaces = buffer->indent_level * INDENT_WIDTH;
    buffer_ensure_capacity(buffer, spaces + 1);
    
    for (int i = 0; i < spaces; i++) {
        buffer->data[buffer->size++] = INDENT_CHAR;
    }
    buffer->data[buffer->size] = '\0';
}

// 增加缩进级别
static void
buffer_indent(pretty_buffer *buffer) {
    buffer->indent_level++;
}

// 减少缩进级别
static void
buffer_unindent(pretty_buffer *buffer) {
    if (buffer->indent_level > 0) {
        buffer->indent_level--;
    }
}

// 向前声明函数
static void serialize_value(lua_State *L, pretty_buffer *buffer, int index);

// 序列化nil
static void
serialize_nil(pretty_buffer *buffer) {
    buffer_append_string(buffer, "nil");
}

// 序列化布尔值
static void
serialize_boolean(pretty_buffer *buffer, int boolean) {
    buffer_append_string(buffer, boolean ? "true" : "false");
}

// 序列化数字
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

// 序列化字符串
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
                    // 使用\x格式输出控制字符
                    buffer_append_format(buffer, "\\x%02x", (unsigned char)c);
                } else {
                    // 普通字符直接添加
                    buffer_ensure_capacity(buffer, 2);
                    buffer->data[buffer->size++] = c;
                    buffer->data[buffer->size] = '\0';
                }
                break;
        }
    }
    
    buffer_append_string(buffer, "\"");
}

// 序列化表
static void
serialize_table(lua_State *L, pretty_buffer *buffer, int index) {
    // 确保索引有效
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }
    
    // 检查是否是一个表
    if (!lua_istable(L, index)) {
        buffer_append_string(buffer, "nil /* not a table */");
        return;
    }
    
    // 开始表
    buffer_append_string(buffer, "{\n");
    buffer_indent(buffer);
    
    // 获取数组部分的大小
    size_t array_size = lua_rawlen(L, index);
    int has_array = array_size > 0;
    int has_hash = 0;
    
    // 处理数组部分
    for (size_t i = 1; i <= array_size; i++) {
        buffer_append_indent(buffer);
        
        lua_rawgeti(L, index, i);
        serialize_value(L, buffer, -1);
        lua_pop(L, 1);
        
        if (i < array_size) {
            buffer_append_string(buffer, ",\n");
        }
    }
    
    // 处理哈希部分
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        // 检查键是否为数字且在数组部分的范围内
        if (lua_type(L, -2) == LUA_TNUMBER && 
            lua_isinteger(L, -2)) {
            lua_Integer key = lua_tointeger(L, -2);
            if (key > 0 && key <= (lua_Integer)array_size) {
                // 跳过数组部分已经处理的键
                lua_pop(L, 1);
                continue;
            }
        }
        
        // 如果有数组部分，并且这是第一个哈希部分的元素，添加分隔符
        if (has_array && !has_hash) {
            buffer_append_string(buffer, ",\n");
            has_hash = 1;
        } else if (has_hash) {
            buffer_append_string(buffer, ",\n");
        } else {
            has_hash = 1;
        }
        
        buffer_append_indent(buffer);
        
        // 序列化键
        buffer_append_string(buffer, "[");
        serialize_value(L, buffer, -2);
        buffer_append_string(buffer, "] = ");
        
        // 序列化值
        serialize_value(L, buffer, -1);
        
        lua_pop(L, 1);
    }
    
    buffer_append_string(buffer, "\n");
    buffer_unindent(buffer);
    buffer_append_indent(buffer);
    buffer_append_string(buffer, "}");
}

// 序列化用户数据
static void
serialize_userdata(pretty_buffer *buffer, void *p) {
    buffer_append_format(buffer, "userdata(%p)", p);
}

// 序列化单个值
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

// Lua函数：将Lua值序列化为带缩进的字符串
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

// 注册模块
int
luaopen_serialize(lua_State *L) {
    luaL_Reg l[] = {
        { "serialize_pretty", serialize_pretty },
        { NULL, NULL },
    };
    luaL_newlib(L, l);
    return 1;
} 
