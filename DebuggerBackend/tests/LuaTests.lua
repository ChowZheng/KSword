local allocations, nextAddress = {}, 0x10000
local function memory(address, count)
    for base, stream in pairs(allocations) do
        if address >= base and address + count <= base + stream.Size then return stream, address - base end
    end
    error("Invalid local buffer " .. address)
end
function createMemoryStream()
    local stream = {Size = 0, Memory = nextAddress, data = {}}
    nextAddress = nextAddress + 0x100000
    allocations[stream.Memory] = stream
    function stream.destroy() allocations[stream.Memory] = nil end
    return stream
end
function stringToByteTable(value)
    local bytes = {}; for i = 1, #value do bytes[i] = value:byte(i) end; return bytes
end
function writeBytesLocal(address, bytes)
    local stream, offset = memory(address, #bytes)
    for i, value in ipairs(bytes) do stream.data[offset + i] = value end
end
function readBytesLocal(address, count)
    local stream, offset = memory(address, count)
    local bytes = {}; for i = 1, count do bytes[i] = stream.data[offset + i] or 0 end; return bytes
end
local function writeNumber(address, value, count)
    local bytes = {}; for i = 1, count do bytes[i] = (value >> ((i - 1) * 8)) & 255 end
    writeBytesLocal(address, bytes)
end
local function readNumber(address, count)
    local value = 0; for i, byte in ipairs(readBytesLocal(address, count)) do value = value | (byte << ((i - 1) * 8)) end
    return value
end
function writeIntegerLocal(address, value) writeNumber(address, value, 4) end
function writeQwordLocal(address, value) writeNumber(address, value, 8) end
function readIntegerLocal(address) return readNumber(address, 4) end
function readQwordLocal(address) return readNumber(address, 8) end
function cheatEngineIs64Bit() return true end
function getAddressSafe(name) return name:find("GetModuleHandleW", 1, true) and 1 or 2 end
local loaded, selected, selectionError, driverReady = true, false, 0, true
local versions = {[16] = 6, [17] = 6, [18] = 2, [19] = 6, [20] = 6,
    [21] = 1, [22] = 1, [23] = 1, [24] = 1, [25] = 2, [26] = 1,
    [27] = 1, [28] = 9, [29] = 1, [30] = 4, [31] = 1, [32] = 1}
function executeCodeLocalEx(address, parameter)
    if address == 1 then return loaded and 100 or 0 end
    if address == 2 then return 200 end
    assert(address == 200)
    assert(readIntegerLocal(parameter) == 1 and readIntegerLocal(parameter + 4) == 48)
    local command = readIntegerLocal(parameter + 8)
    local input, output = readQwordLocal(parameter + 16), readQwordLocal(parameter + 24)
    local returned, errorCode = 0, 0
    if command == 1 or command == 2 then
        if command == 2 then
            errorCode = selectionError
            if errorCode == 0 then selected = readIntegerLocal(input) == 1 end
        end
        writeIntegerLocal(output, 1); writeIntegerLocal(output + 4, 40)
        writeIntegerLocal(output + 8, driverReady and 1 or 0); writeIntegerLocal(output + 12, selected and 1 or 0)
        writeIntegerLocal(output + 16, 1); writeIntegerLocal(output + 32, 1)
        returned = 40
    elseif command == 3 then
        local requested = readIntegerLocal(input)
        assert(versions[requested] ~= nil)
        writeIntegerLocal(output, 1); writeIntegerLocal(output + 4, requested)
        writeIntegerLocal(output + 8, 64); writeIntegerLocal(output + 12, 64)
        writeIntegerLocal(output + 16, versions[requested]); returned = 24
    else
        assert(readIntegerLocal(input) == versions[command] and readIntegerLocal(input + 4) == 64)
        writeIntegerLocal(output, versions[command]); writeIntegerLocal(output + 4, 64); returned = 64
    end
    writeIntegerLocal(parameter + 40, errorCode); writeIntegerLocal(parameter + 44, returned)
    return errorCode
end
local messages = {}
local function log(message) messages[#messages + 1] = message end
local factory = assert(loadfile(backend_factory_file))()
local api = factory(log, "virtual/KswordCheatEnginePlugin.dll")
local status = assert(api.status()); assert(status.driverReady and not status.useHvm)
assert(api.useHvm(true)); assert(api.status().useHvm)
selectionError = 170; assert(not api.useHvm(false)); assert(api.status().useHvm)
selectionError = 0; assert(api.useHvm(false))
local commandCount = 0
for name in pairs(api.hvm.commands) do
    local packet = api.hvm.packet(name)
    packet.write32(8, 0).write64(16, 0x1234)
    assert(not pcall(packet.write64, 60, 1))
    local bytes, errorCode = packet.send(); assert(errorCode == 0 and #bytes == 64)
    packet.destroy(); packet.destroy(); commandCount = commandCount + 1
end
assert(commandCount == 17 and next(allocations) == nil)

-- Exercise the actual autorun timer and session files entirely in memory.
local files, timers = {}, {}
local environment = {KSWORD_CE_BRIDGE_STATUS_FILE = "status", KSWORD_CE_LOG_FILE = "log",
    KSWORD_CE_BRIDGE_DLL = "virtual/KswordCheatEnginePlugin.dll",
    KSWORD_CE_CONTROL_FILE = "control", KSWORD_CE_BACKEND_STATE_FILE = "state"}
local realDofile, realIo = dofile, io
io = {}
function io.open(path, mode)
    if mode == "r" and files[path] == nil then return nil end
    if mode == "w" then files[path] = "" end
    local handle = {}
    function handle.read() return files[path] end
    function handle.write(self, ...) for _, value in ipairs({...}) do files[path] = (files[path] or "") .. tostring(value) end end
    function handle.close() end
    return handle
end
local ticks = 0
os.getenv = function(name) return environment[name] end
os.time = function() ticks = ticks + 1; return ticks end
os.remove = function(path) files[path] = nil; return true end
os.rename = function(from, to) files[to] = files[from]; files[from] = nil; return true end
local form = {Caption = "Cheat Engine", Color = 42, Font = {Name = "Tahoma"}}
function getMainForm() return form end
function getCheatEngineDir() return "virtual/" end
function loadPlugin() return 0 end
function createTimer()
    local timer = {}; function timer.destroy() end; timers[#timers + 1] = timer; return timer
end
dofile = function(path)
    assert(path == "virtual/autorun\\ksword_hvm.lua")
    return realDofile(backend_factory_file)
end
assert(loadfile(backend_bootstrap_file))()
local timer = assert(KSwordBackendTimer)
timer.OnTimer(); assert(form.Caption == "Cheat Engine [KSword R0]" and files.status == "ready")
files.control = "1 1\n"; timer.OnTimer()
assert(form.Caption == "Cheat Engine [KSword HVM]" and files.state:match("^1 0 1 1 0 1"))
selectionError = 170; files.control = "2 0\n"; timer.OnTimer()
assert(files.state:match("^2 170 1 1 0 1") and form.Caption == "Cheat Engine [KSword HVM]")
assert(form.Color == 42 and form.Font.Name == "Tahoma")
driverReady = false; timer.OnTimer()
assert(form.Caption == "Cheat Engine [KSword Connected]")
driverReady = true; timer.OnTimer()
assert(form.Caption == "Cheat Engine [KSword HVM]")
loaded = false; timer.OnTimer()
assert(timer.Enabled == true and form.Caption == "Cheat Engine" and files.state:match("^2 126 0 0 0 0"))
loaded = true; timer.OnTimer()
assert(form.Caption == "Cheat Engine [KSword HVM]" and files.state:match("^2 0 1 1 0 1"))
assert(next(allocations) == nil)
io = realIo
print("PASS: Lua ABI packets, HVM acknowledgement, title-only update, plugin unload/reconnect")
