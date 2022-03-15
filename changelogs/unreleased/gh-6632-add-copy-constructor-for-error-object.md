## feature/lua/error

* This patch implements new method Lua module
which makes a deep copy of the error object.
The copied object does not contain next error fields of the source object.
This method allows you to return a copy of the original error to a user,
thus ensuring that the original error object stays intact.
Closes #6632.

Usage example:

```lua
e1 = box.error.new({code = 1, reason = "1"})
e2 = box.error.new({code = 2, reason = "2"})
e3 = box.error.new({code = 3, reason = "3"})
e4 = box.error.new({code = 4, reason = "4"})
e1:set_prev(e2)
e2:set_prev(e3)
e3:set_prev(e4)
--- e1: e1 -> e2 -> e3 -> e4

copy1 = e1:copy()
--- copy1: copy(e1) -> copy(e2) -> copy(e3) -> copy(e4)

copy2 = e2:copy()
--- copy2: copy(e2) -> copy(e3) -> copy(e4)
```