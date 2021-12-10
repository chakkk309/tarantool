
-- init.lua -- internal file

local ffi = require('ffi')
ffi.cdef[[
struct method_info;

struct type_info {
    const char *name;
    const struct type_info *parent;
    const struct method_info *methods;
};
double
tarantool_uptime(void);
typedef int32_t pid_t;
pid_t getpid(void);
void
tarantool_exit(int);
]]

local fio = require("fio")

local soext = (jit.os == "OSX" and "dylib" or "so")

local ROCKS_LIB_PATH = '.rocks/lib/tarantool'
local ROCKS_LUA_PATH = '.rocks/share/tarantool'
local LIB_TEMPLATES = { '?.'..soext }
local LUA_TEMPLATES = { '?.lua', '?/init.lua' }
local ROCKS_LIB_TEMPLATES = { ROCKS_LIB_PATH .. '/?.'..soext }
local ROCKS_LUA_TEMPLATES = { ROCKS_LUA_PATH .. '/?.lua', ROCKS_LUA_PATH .. '/?/init.lua' }

get_stack = function()
    local info = debug.getinfo(1)
    local stack = {}
    local i = 1
    while info ~= nil do
        stack[i] = info
        i = i + 1
        info = debug.getinfo(i)
    end
    return stack
end

print_stack = function(stack)
    for k, v in pairs(stack) do
        print(v.short_src, ":", v.linedefined)
    end
end

old_require = require
local function remove_root_directory(path)
    local cur_dir = os.getenv("PWD") .. '/'
   local start_len = #path

    while #cur_dir > 0 and start_len == #path do
        if cur_dir == '/' then
            if path:sub(1, 1) == '/' then
                path = path:sub(2)
            end
            break
        end
        path = path:gsub(cur_dir, '')
        local ind= cur_dir:find('[^/]+/$')
        cur_dir = cur_dir:sub(1,ind - 1)
    end
    return path
end


local function module_name_form_filename(filename)
    local pathes = package.path .. package.cpath
    local result = filename
    result = result:gsub('builtin/', '')

    for path in  pathes:gmatch'/([A-Za-z\\/\\.0-9]+)\\?' do
        result = result:gsub('/' .. path, '');
    end

    result = result:gsub('/init.lua', '');
    result = result:gsub('%.lua', '');

    result = remove_root_directory(result)
    result = result:gsub(ROCKS_LIB_PATH .. '/', '');
    result = result:gsub(ROCKS_LUA_PATH .. '/', '');
    result = result:gsub('/', '.');
    return result
end

local function module_name_by_func(func_level)
    local debug = debug or old_require('debug')
    local src_name = debug.getinfo(func_level + 1).source
    src_name = src_name:sub(2)
    print("result module name = ", src_name)
    --local second_name, value = debug.getlocal(3, 1)
    --print("second mod name = ", second_name, value)
    --second_name, value = debug.getlocal(3, 2)
    --print("second mod name = ", second_name, value)
    --for k, v in pairs(debug.getinfo(func_level + 2)) do
    --    print(k, v)
    --end
    return module_name_form_filename(src_name)
end

require = function(modname)
    print('modname require 1 = ', modname)
    local debug = debug or old_require('debug')
    if modname == 'log' then
        print('require log')
        local log = old_require(modname)
        print('set module name')
        log.new(module_name_by_func(2))
        local stack1 = get_stack()
        print('stack1')
        print_stack(stack1)
        local stack2 = debug.traceback()
        print('stack2 = ', stack2)
        print('log.internal')
        print_stack(log.internal)
        return log
    end
    if modname == 'log.internal' then
        return debug.traceback()
    end
    print('stack in our require')
    print_stack(get_stack())
    print(debug.traceback())
    return old_require(modname)
end

local package_searchroot

local function searchroot()
    return package_searchroot or fio.cwd()
end

local function setsearchroot(path)
    if not path then
        -- Here we need to get this function caller's sourcedir.
        path = debug.sourcedir(3)
    elseif path == box.NULL then
        path = nil
    else
        assert(type(path) == 'string', 'Search root must be a string')
    end
    package_searchroot = path and fio.abspath(path)
end

dostring = function(s, ...)
    local chunk, message = loadstring(s)
    if chunk == nil then
        error(message, 2)
    end
    return chunk(...)
end

local fiber = require("fiber")
local function exit(code)
    code = (type(code) == 'number') and code or 0
    ffi.C.tarantool_exit(code)
    -- Make sure we yield even if the code after
    -- os.exit() never yields. After on_shutdown
    -- fiber completes, we will never wake up again.
    while true do fiber.yield() end
end
rawset(os, "exit", exit)

local function uptime()
    return tonumber(ffi.C.tarantool_uptime());
end

local function pid()
    return tonumber(ffi.C.getpid())
end

local function mksymname(name)
    local mark = string.find(name, "-")
    if mark then name = string.sub(name, mark + 1) end
    return "luaopen_" .. string.gsub(name, "%.", "_")
end

local function load_lib(file, name)
    return package.loadlib(file, mksymname(name))
end

local function load_lua(file)
    return loadfile(file)
end

local function traverse_path(path)
    path = fio.abspath(path)
    local paths = { path }

    while path ~= '/' do
        path = fio.dirname(path)
        table.insert(paths, path)
    end

    return paths
end

-- Generate a search function, which performs searching through
-- templates setup in options.
--
-- @param path_fn function which returns a base path for the
--     resulting template
-- @param templates table with lua search templates
-- @param need_traverse bool flag which tells search function to
--     build multiple paths by expanding base path up to the
--     root ('/')
-- @return a searcher function which builds a path template and
--     calls package.searchpath
local function gen_search_func(path_fn, templates, need_traverse)
    assert(type(path_fn) == 'function', 'path_fn must be a function')
    assert(type(templates) == 'table', 'templates must be a table')

    return function(name)
        local path = path_fn() or '.'
        local paths = need_traverse and traverse_path(path) or { path }

        local searchpaths = {}

        for _, path in ipairs(paths) do
            for _, template in pairs(templates) do
                table.insert(searchpaths, fio.pathjoin(path, template))
            end
        end

        local searchpath = table.concat(searchpaths, ';')

        return package.searchpath(name, searchpath)
    end
end

-- Compose a loader function from options.
--
-- @param search_fn function will be used to search a file from
--     path template
-- @param load_fn function will be used to load a file, found by
--     search function
-- @return function a loader, which first search for the file and
--     then loads it
local function gen_loader_func(search_fn, load_fn)
    assert(type(search_fn) == 'function', 'search_fn must be defined')
    assert(type(load_fn) == 'function', 'load_fn must be defined')

    return function(name)
        if not name then
            return "empty name of module"
        end
        local file, err = search_fn(name)
        if not file then
            return err
        end
        local loaded, err = load_fn(file, name)
        if err == nil then
            return loaded
        else
            return err
        end
    end
end

local search_lua = gen_search_func(searchroot, LUA_TEMPLATES)
local search_lib = gen_search_func(searchroot, LIB_TEMPLATES)
local search_rocks_lua = gen_search_func(searchroot, ROCKS_LUA_TEMPLATES, true)
local search_rocks_lib = gen_search_func(searchroot, ROCKS_LIB_TEMPLATES, true)

local search_funcs = {
    search_lua,
    search_lib,
    search_rocks_lua,
    search_rocks_lib,
    function(name) return package.searchpath(name, package.path) end,
    function(name) return package.searchpath(name, package.cpath) end,
}

local function search(name)
    if not name then
        return "empty name of module"
    end
    for _, searcher in ipairs(search_funcs) do
        local file = searcher(name)
        if file ~= nil then
            return file
        end
    end
    return nil
end

-- loader_preload 1
table.insert(package.loaders, 2, gen_loader_func(search_lua, load_lua))
table.insert(package.loaders, 3, gen_loader_func(search_lib, load_lib))
table.insert(package.loaders, 4, gen_loader_func(search_rocks_lua, load_lua))
table.insert(package.loaders, 5, gen_loader_func(search_rocks_lib, load_lib))
-- package.path   6
-- package.cpath  7
-- croot          8

rawset(package, "search", search)
rawset(package, "searchroot", searchroot)
rawset(package, "setsearchroot", setsearchroot)



return {
    uptime = uptime;
    pid = pid;
}
