# lua-dump

```lua
local dump = require "dump"
local t = {1, 2, 3}

print(dump.packstring(t))               -- {1,2,3}
print(dump.packstring(dump.pretty, t))  -- {1, 2, 3}

local handle, size = dump.pack(t)
dump.trash(handle)
```

```lua
local skynet = require "skynet"
local core = require "skynet.core"
local dump = require "dump"

skynet.start(function()
    local logger = assert(skynet.localname ".logger")

    local function logdump(...)
        local handle <close>, size, fallback = dump.pack(...)
        if size == 0 then
            return core.send(logger, skynet.PTYPE_TEXT, 0, fallback or "")
        end
        return core.send(logger, skynet.PTYPE_TEXT, 0, dump.detach(handle))
    end

    logdump({1, 2, 3})
    logdump(dump.pretty, {1, 2, 3})
end)
```
