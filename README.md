# lua-dump
```sh
make test          # 构建 dump.so 并运行 test.lua
make bench         # 编译基准程序
./bench            # 运行基准
```

## 用法

```lua
local dump = require "dump"

local t = { 1, 2, 3, 4, 5 }
print(dump.packstring(t))               -- {1,2,3,4,5}
print(dump.packstring(dump.pretty, t))  -- {1, 2, 3, 4, 5}

local ptr, sz = dump.pack(msg)

dump.trash(ptr, sz)
```