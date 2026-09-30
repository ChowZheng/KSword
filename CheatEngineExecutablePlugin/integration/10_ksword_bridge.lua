-- KSword Cheat Engine 自动初始化脚本。
-- 加载公共调试后端，转发日志并按请求选择 HVM；CE 只增加标题加载标识。

local statusPath = os.getenv("KSWORD_CE_BRIDGE_STATUS_FILE")
local logPath = os.getenv("KSWORD_CE_LOG_FILE")

local function appendLog(message)
    if logPath == nil or logPath == "" then
        return
    end
    local logFile = io.open(logPath, "a")
    if logFile ~= nil then
        logFile:write(os.date("%Y-%m-%d %H:%M:%S"), " ",
            tostring(message):gsub("[\r\n]", " "), "\n")
        logFile:close()
    end
end

local function writeStatus(value)
    if statusPath == nil or statusPath == "" then
        return
    end
    local statusFile = io.open(statusPath, "w")
    if statusFile ~= nil then
        statusFile:write(value)
        statusFile:close()
    end
end

local function finishBridgeInitialization()
    appendLog("KSword driver bridge is ready")
    writeStatus("ready")
end

local bridgePath = os.getenv("KSWORD_CE_BRIDGE_DLL")
if bridgePath == nil or bridgePath == "" then
    bridgePath = getCheatEngineDir() .. "..\\..\\bridge\\x64\\KswordCheatEnginePlugin.dll"
end
if not cheatEngineIs64Bit() then appendLog("KSword requires 64-bit Cheat Engine"); writeStatus("failed"); return end

-- loadPlugin 会同时调用 CEPlugin_InitializePlugin；返回 nil 表示加载或初始化失败。
local loadOk, pluginId = pcall(loadPlugin, bridgePath)
if not loadOk or pluginId == nil then
    appendLog("KSword driver bridge failed to load: " .. tostring(pluginId))
    writeStatus("failed")
    return
end
appendLog("KSword driver bridge loaded")

local apiOk, apiOrError = pcall(function()
    local factory = dofile(getCheatEngineDir() .. "autorun\\ksword_hvm.lua")
    return factory(appendLog, bridgePath)
end)
if not apiOk then
    appendLog("KSword backend API failed: " .. tostring(apiOrError))
    writeStatus("failed")
    return
end
_G.KSword = apiOrError

local controlPath = os.getenv("KSWORD_CE_CONTROL_FILE")
local statePath = os.getenv("KSWORD_CE_BACKEND_STATE_FILE")
local revision, controlError = 0, 0
local backendError = 0
local lastPoll = 0
local backendTimer = createTimer(nil, false)
backendTimer.Interval = 250
local function publishState(status)
    if statePath == nil or statePath == "" then return end
    local state = io.open(statePath .. ".new", "w")
    if state ~= nil then
        state:write(revision, " ", controlError, " ", status.useHvm and 1 or 0,
            " ", status.directMemoryWindow and 1 or 0, " ", status.residentActive and 1 or 0,
            " ", status.eptBreakpointProtocol or 0, "\n")
        state:close()
        os.remove(statePath)
        os.rename(statePath .. ".new", statePath)
    end
end
local function pollBackend()
    if controlPath ~= nil and controlPath ~= "" then
        local request = io.open(controlPath, "r")
        if request ~= nil then
            local text = request:read("*a")
            request:close()
            local newRevision, requested = text:match("^(%d+)%s+([01])%s*$")
            newRevision = tonumber(newRevision)
            if newRevision ~= nil and newRevision > revision then
                revision = newRevision
                local ok
                ok, controlError = KSword.useHvm(requested == "1")
                if not ok then appendLog("HVM selection rejected, error " .. controlError) end
            end
        end
    end
    -- Query once a second; command processing still runs every quarter second.
    if os.time() == lastPoll then return end
    lastPoll = os.time()
    local status, errorCode = KSword.status()
    if status == nil then
        controlError = errorCode
        publishState({})
        if backendError ~= errorCode then appendLog("Backend status failed: " .. errorCode) end
        backendError = errorCode
        local mainForm = getMainForm()
        if mainForm ~= nil then
            mainForm.Caption = mainForm.Caption:gsub("%s*%[KSword [^%]]*%]$", "")
        end
        return
    end
    if backendError ~= 0 then
        if controlError == backendError then controlError = 0 end
        backendError = 0
        appendLog("KSword backend session reconnected")
    end
    local mainForm = getMainForm()
    if mainForm ~= nil then
        local caption = mainForm.Caption:gsub("%s*%[KSword [^%]]*%]$", "")
        mainForm.Caption = caption .. (status.useHvm and " [KSword HVM]" or " [KSword R0]")
    end
    publishState(status)
end
backendTimer.OnTimer = function()
    local ok, errorText = pcall(pollBackend)
    if not ok then
        backendTimer.Enabled = false
        controlError = 31
        publishState({})
        appendLog("Backend polling stopped: " .. tostring(errorText))
    end
end
_G.KSwordBackendTimer = backendTimer
backendTimer.Enabled = true

-- 等 CE 主消息循环空闲后再打开目标，避免在 autorun 加载栈中同步触发
-- 模块/内存区枚举。桥接已在定时器创建前安装，因此首个进程句柄仍经过 KSword。
local targetPid = tonumber(os.getenv("KSWORD_CE_TARGET_PID") or "")
if targetPid ~= nil and targetPid > 0 then
    local openTimer = createTimer(nil, false)
    openTimer.Interval = 750
    openTimer.OnTimer = function(timer)
        timer.Enabled = false
        timer.destroy()
        _G.KSwordBridgeOpenTimer = nil
        local openOk, openError = pcall(openProcess, targetPid)
        if not openOk or getOpenedProcessID() ~= targetPid then
            appendLog("Failed to open process " .. targetPid .. ": " ..
                tostring(openError))
            writeStatus("failed")
            return
        end
        appendLog("Opened process " .. targetPid)
        finishBridgeInitialization()
    end
    _G.KSwordBridgeOpenTimer = openTimer
    openTimer.Enabled = true
else
    finishBridgeInitialization()
end
