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
local settingsPath = os.getenv("KSWORD_CE_SETTINGS_FILE")
local revision, controlError = 0, 0
local backendError, settingsError = 0, 0
local lastPolicyError = 0
local lastPoll = 0
local defaultOptions = {mode = 0, shadowMemoryWrites = false, allowFallback = true,
    logFallback = true, nativeContextFallback = true, nativeSuspendFallback = true, maxShadowPages = 32}
local function values(text, count)
    if text == nil or #text > 4096 then return nil end
    local result = {}
    for token in text:gmatch("%S+") do result[#result + 1] = token end
    if #result ~= count or result[1] ~= "CE2" then return nil end
    for index = 2, count do
        if not result[index]:match("^%d+$") then return nil end
        result[index] = tonumber(result[index])
        if result[index] > 4294967295 then return nil end
    end
    return result
end
local function parseOptions(packet, first)
    for index = first, first + 5 do if packet[index] > 1 then return nil end end
    local limit = packet[first + 6]
    if limit < 1 or limit > 32 then return nil end
    return {mode = packet[first + 1], shadowMemoryWrites = packet[first + 2] == 1,
        allowFallback = packet[first + 3] == 1, logFallback = true,
        nativeContextFallback = packet[first + 4] == 1, nativeSuspendFallback = packet[first + 5] == 1,
        maxShadowPages = limit}, packet[first] == 1
end
local function persist(status, options)
    if settingsPath == nil or settingsPath == "" or options == nil then return end
    local temporary = os.getenv("KSWORD_CE_SETTINGS_TEMP_FILE") or (settingsPath .. ".new")
    local file, detail = io.open(temporary, "w")
    if file == nil then
        settingsError = 5; appendLog("Cannot persist CE backend settings: " .. tostring(detail)); return
    end
    local wrote, writeError = file:write("CE2 ", status.useHvm and 1 or 0, " ", options.mode,
        " ", options.shadowMemoryWrites and 1 or 0, " ", options.allowFallback and 1 or 0,
        " ", options.nativeContextFallback and 1 or 0, " ", options.nativeSuspendFallback and 1 or 0,
        " ", options.maxShadowPages, "\n")
    local closed, closeError = file:close()
    if wrote == nil or closed == nil then
        settingsError = 5; appendLog("Cannot write CE backend settings: " .. tostring(writeError or closeError)); return
    end
    local moveFile = getAddressSafe("kernel32.MoveFileExW", true)
    local renamed, renameError
    if moveFile ~= nil then
        local moved, result = pcall(executeCodeLocalEx, moveFile,
            {type = 4, value = temporary}, {type = 4, value = settingsPath}, 9)
        renamed = moved and result ~= nil and result ~= 0
        renameError = moved and "MoveFileExW returned failure" or result
    else
        renamed, renameError = false, "MoveFileExW is unavailable"
    end
    settingsError = renamed and 0 or 5
    if not renamed then appendLog("Cannot replace CE backend settings: " .. tostring(renameError)) end
end
local function initializeOptions()
    local requested, selected = defaultOptions, false
    if settingsPath ~= nil and settingsPath ~= "" then
        local file = io.open(settingsPath, "r")
        if file ~= nil then
            local packet = values(file:read(4097), 8); file:close()
            local saved, savedHvm
            if packet ~= nil then saved, savedHvm = parseOptions(packet, 2) end
            if saved ~= nil then requested, selected = saved, savedHvm
            else appendLog("Invalid saved CE backend settings; using ordinary write/freeze defaults (Shadow off)") end
        end
    end
    local actual, errorCode = KSword.setOptions(requested)
    if actual == nil or errorCode ~= 0 then
        appendLog("CE memory policy initialization rejected, error " .. tostring(errorCode)); return
    end
    if selected then
        local accepted, selectionError = KSword.useHvm(true)
        if not accepted then appendLog("Saved HVM selection rejected, error " .. selectionError) end
    end
    local status = KSword.status()
    if status ~= nil then persist(status, actual) end
    appendLog("CE edits and freeze writes share the backend policy: Shadow execution-view code patches " ..
        ((actual.shadowMemoryWrites or actual.mode == 1) and "ON" or "OFF") ..
        "; RX/RWX numeric freeze on a Shadow page does not change ordinary data reads. " ..
        "Stealth forces Shadow. Explicit fallback is " .. (actual.allowFallback and "allowed and logged" or "disabled"))
end
local initialized, initializeError = pcall(initializeOptions)
if not initialized then appendLog("CE options API unavailable: " .. tostring(initializeError)) end
local backendTimer = createTimer(nil, false)
backendTimer.Interval = 250
local function publishState(status, policy)
    if statePath == nil or statePath == "" then return end
    local state = io.open(statePath .. ".new", "w")
    if state ~= nil then
        state:write(revision, " ", controlError, " ", status.useHvm and 1 or 0,
            " ", status.directMemoryWindow and 1 or 0, " ", status.residentActive and 1 or 0,
            " ", status.eptBreakpointProtocol or 0)
        if policy ~= nil then
            state:write(" CE2 ", status.driverReady and 1 or 0, " 1 ", policy.mode,
                " ", policy.shadowMemoryWrites and 1 or 0, " ", policy.allowFallback and 1 or 0,
                " ", policy.nativeContextFallback and 1 or 0, " ", policy.nativeSuspendFallback and 1 or 0,
                " ", policy.maxShadowPages, " ", policy.activePath, " ", policy.shadowWritePages,
                " ", policy.activeBreakpoints, " ", policy.canChangeOptions and 1 or 0,
                " ", policy.fallbackCount, " ", policy.lastFallbackError, " ", settingsError)
        end
        state:write("\n")
        state:close()
        os.remove(statePath)
        local renamed, detail = os.rename(statePath .. ".new", statePath)
        if not renamed then appendLog("Cannot publish CE backend acknowledgement: " .. tostring(detail)) end
    else
        appendLog("Cannot open CE backend acknowledgement file")
    end
end
local function pollBackend()
    if controlPath ~= nil and controlPath ~= "" then
        local request = io.open(controlPath, "r")
        if request ~= nil then
            local text = request:read(4097)
            request:close()
            local newRevision, requested = text:match("^(%d+)%s+([01])%s*$")
            newRevision = tonumber(newRevision)
            if newRevision ~= nil and newRevision > revision and newRevision <= 4294967295 and #text <= 4096 then
                revision = newRevision
                local ok
                ok, controlError = KSword.useHvm(requested == "1")
                if not ok then appendLog("HVM selection rejected, error " .. controlError) end
                if ok then
                    local status = KSword.status()
                    local optionsOk, options = pcall(KSword.options)
                    if status ~= nil and optionsOk and options ~= nil then persist(status, options) end
                end
                lastPoll = 0
            else
                local packet = values(text, 10)
                if packet ~= nil and packet[2] > revision then
                    revision = packet[2]
                    local options, selected = parseOptions(packet, 4)
                    if options == nil or packet[3] > 2 then
                        controlError = 87; appendLog("Invalid CE backend control request")
                    elseif packet[3] == 0 then
                        local accepted
                        accepted, controlError = KSword.useHvm(selected)
                        if not accepted then appendLog("HVM selection rejected, error " .. controlError) end
                    elseif packet[3] == 1 then
                        local actual
                        actual, controlError = KSword.setOptions(options)
                        if controlError == 0 and actual ~= nil then appendLog("CE memory policy acknowledged by backend") end
                    else
                        local accepted
                        accepted, controlError = KSword.restoreShadowWrites()
                    end
                    if controlError == 0 then
                        local status, actual = KSword.status(), KSword.options()
                        if status ~= nil and actual ~= nil then persist(status, actual) end
                    end
                    lastPoll = 0
                end
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
        local label = not status.driverReady and "KSword Connected" or
            (status.useHvm and "KSword HVM" or "KSword R0")
        mainForm.Caption = caption .. " [" .. label .. "]"
    end
    local policyOk, policy, policyError = pcall(KSword.policy)
    if not policyOk then policy = nil end
    if policyError ~= nil and policyError ~= 0 then
        if policyError ~= lastPolicyError then appendLog("CE policy status failed: " .. policyError) end
        policy = nil
    end
    lastPolicyError = policyError or 0
    publishState(status, policy)
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
