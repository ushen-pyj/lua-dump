s = require "serialize"

local a = {
	a = 1,
	b = {
		a = 1,
		b = 2.0,
		c = {
			a = 1,
			b = 2,
			c = "asfasfasf",
		},
	},
	c = 3,
}
local b = {}
b[a] = 2

print(s.serialize_pretty(b))
