# lua-dump


```lua
local dump = require "dump"

print(dump.packstring({ a = 1, b = { 2, 3 } }))

local ptr, sz = dump.pack(msg)

dump.trash(ptr, sz)
```