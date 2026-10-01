-- Guest-only regression. The launcher owns DebugTarget and prepares a foreign
-- standard HVM resident before starting the real CE payload/production bridge.
-- This fixture never prepares, starts, stops, clears, or rolls back HVM.
local root=assert(os.getenv('KSWORD_TEST_ROOT'),'KSWORD_TEST_ROOT is required')
local backendPath=assert(os.getenv('KSWORD_DEBUGGER_LOG_FILE'),'a fresh backend log is required')
local log=assert(io.open(root..'\\ce-data-live.log','w'))
local function record(message) log:write(message,'\n');log:flush() end
local function check(value,message) if not value then error(message) end record('PASS '..message) end
local done,phase=false,0
local info,baseline={},nil
local processHandle,threadHandle=nil,nil
local attachTime,breakRequested,armedLogOffset=nil,false,nil
local hits=0
local deadline=os.time()+60
local native={}
local function windows(name,...)
    local address=native[name]
    if address==nil then
        address=assert(getAddressSafe('kernel32.'..name,true),name..' is missing')
        native[name]=address
    end
    local args={...}
    for index,value in ipairs(args) do args[index]={type=0,value=value} end
    return executeCodeLocalEx(address,table.unpack(args))
end
local function readLog(offset)
    local file=assert(io.open(backendPath,'rb'),'backend log is missing')
    local length=assert(file:seek('end'))
    assert(length>=offset,'backend log was unexpectedly truncated')
    assert(file:seek('set',offset))
    local text=file:read('*a');file:close()
    return text,length
end
local function u32(bytes,offset)
    assert(#bytes>=offset+4,'short protocol response')
    local value=0
    for index=3,0,-1 do value=value*256+bytes[offset+index+1] end
    return value
end
local function query(name,operation)
    local packet=KSword.hvm.packet(name)
    if operation~=nil then packet.write32(8,operation) end
    local bytes,errorCode=packet.send();packet.destroy()
    check(errorCode==0,name..' query transport error='..tostring(errorCode))
    return bytes
end
local function snapshot(label)
    local status,errorCode=KSword.status()
    check(errorCode==0 and status~=nil,label..' actual status ACK')
    local policy,policyError=KSword.policy()
    check(policyError==0 and policy~=nil,label..' actual policy ACK')
    local hvm=query('status')
    check(u32(hvm,0)==6 and u32(hvm,128)==0,label..' exact HVM v6 status')
    local views=query('view',4)
    check(u32(views,8)==0,label..' execution-view query status')
    local value={status=status,policy=policy,generation=u32(hvm,136),
        stateFlags=u32(hvm,132),processors=u32(hvm,140),prepared=u32(hvm,144),
        resident=u32(hvm,152),residentImplementation=u32(hvm,156),
        eptRules=u32(hvm,176),views=u32(views,16)}
    record(string.format('%s POLICY path=%d bindings=%d writePages=%d fallbackCount=%d lastFallback=%d canChange=%s',
        label,policy.activePath,policy.activeBreakpoints,policy.shadowWritePages,
        policy.fallbackCount,policy.lastFallbackError,tostring(policy.canChangeOptions)))
    record(string.format('%s HVM generation=%d flags=0x%x prepared=%d resident=%d/%d implementation=%d eptRules=%d views=%d ownsResident=%s selected=%s',
        label,value.generation,value.stateFlags,value.prepared,value.resident,value.processors,
        value.residentImplementation,value.eptRules,value.views,
        tostring(status.ownsResident),tostring(status.useHvm)))
    check(status.driverReady and status.residentActive and not status.ownsResident,
        label..' foreign resident remains active and unowned')
    check(value.processors>0 and value.prepared==value.processors and value.resident==value.processors and
        (value.stateFlags & 0x4000)~=0,label..' foreign residency remains on every processor')
    check(policy.activePath==0 and policy.shadowWritePages==0 and value.eptRules==0 and value.views==0,
        label..' native data route owns no EPT rules or Shadow execution views')
    if baseline~=nil then
        check(value.generation==baseline.generation and value.stateFlags==baseline.stateFlags and
            value.processors==baseline.processors and value.prepared==baseline.prepared and
            value.resident==baseline.resident and value.residentImplementation==baseline.residentImplementation,
            label..' foreign HVM state and generation are unchanged')
    end
    return value
end
local function context()
    -- Call the original Windows export directly for independent evidence of
    -- visible physical DR addresses. No remote code or context writes are used.
    local stream=createMemoryStream();stream.Size=1248
    writeBytesLocal(stream.Memory,stringToByteTable(string.rep('\0',stream.Size)))
    local address=math.floor((stream.Memory+15)/16)*16
    writeIntegerLocal(address+48,0x100017) -- AMD64 control/integer/segment/debug flags
    local result=windows('GetThreadContext',threadHandle,address)
    if result==nil or result==0 then
        local errorCode=windows('GetLastError');stream.destroy()
        error('original Windows GetThreadContext failed: '..tostring(errorCode))
    end
    local value={dr={},dr6=readQwordLocal(address+104),dr7=readQwordLocal(address+112),
        rax=readQwordLocal(address+120),rip=readQwordLocal(address+248)}
    for slot=0,3 do value.dr[slot+1]=readQwordLocal(address+72+slot*8) end
    stream.destroy()
    record(string.format('NATIVE_CONTEXT RIP=0x%x RAX=0x%x DR0=0x%x DR1=0x%x DR2=0x%x DR3=0x%x DR6=0x%x DR7=0x%x',
        value.rip,value.rax,value.dr[1],value.dr[2],value.dr[3],value.dr[4],value.dr6,value.dr7))
    return value
end
local function watchSlot(value)
    local found=nil
    for slot=0,3 do
        if ((value.dr7 >> (slot*2)) & 3)~=0 then
            check(value.dr[slot+1]==info.data and ((value.dr7 >> (16+slot*4)) & 3)==1 and
                ((value.dr7 >> (18+slot*4)) & 3)==2,
                'enabled native DR slot '..slot..' is the requested 8-byte data write watchpoint')
            check(found==nil,'only one data DR slot is enabled')
            found=slot
        end
    end
    check(found~=nil,'actual Windows context contains a native data watchpoint')
    return found
end
local function recordBreakpointBookkeeping(label)
    -- CE's getBreakpointAddresses enumerates inactive/marked-for-deletion
    -- records too. Its deletion countdown is not a physical DR ownership API.
    local list=debug_getBreakpointList()
    record(label..' CE_BREAKPOINT_BOOKKEEPING count='..#list..' addresses='..table.concat(list,','))
    for _,address in ipairs(list) do
        check(address==info.data,label..' retained CE entry is only the removed data watchpoint')
    end
end
local function verifyClearedContext(value)
    check((value.dr7 & 0xff)==0,'original Windows DR7 has no enabled local or global breakpoint slot')
    check(value.dr[1]==0 and value.dr[2]==0 and value.dr[3]==0 and value.dr[4]==0,
        'original Windows DR0 through DR3 addresses are cleared')
    check((value.dr6 & 0xf)==0,'safe verification pause has no data DR hit bit')
end
local function closeHandles()
    if threadHandle~=nil then windows('CloseHandle',threadHandle);threadHandle=nil end
    if processHandle~=nil then windows('CloseHandle',processHandle);processHandle=nil end
end
local function failure(message)
    if done then return end
    record('FAIL phase='..phase..' '..tostring(message))
    -- Cleanup is limited to this target and its CE breakpoint. External HVM
    -- residency is left to the launching fixture, including on failure.
    if info.data~=nil then pcall(debug_removeBreakpoint,info.data) end
    if debug_isDebugging() then pcall(detachIfPossible) end
    if info.control~=nil then pcall(writeQword,info.control,9) end
    pcall(closeHandles)
    done=true;log:close()
end
_G.debugger_onBreakpoint=function()
    if done then return 0 end
    -- Attach and an explicitly requested pause must finish before arming.
    -- Returning zero leaves that event held; the timer arms and continues it.
    if phase==1 then
        record('ATTACH_PAUSE RIP='..tostring(RIP)..' RAX='..tostring(RAX))
        return 0
    end
    local ok,message=pcall(function()
        if phase==4 then
            -- This is the explicit verification pause after the first result,
            -- not another data hit. Only the armed phase increments hits.
            record('CLEAR_VERIFICATION_PAUSE RIP='..tostring(RIP)..' RAX='..tostring(RAX))
            check(debug_getCurrentDebuggerInterface()==1,'clear verification uses the Windows debugger')
            verifyClearedContext(context())
            local cleared=snapshot('CLEARED_PAUSE')
            check(cleared.policy.activeBreakpoints==0 and cleared.policy.canChangeOptions,
                'held verification pause confirms zero backend bindings')
            recordBreakpointBookkeeping('CLEARED_PAUSE')
            check(hits==1 and readQword(info.control+16)==1,'verification pause is separate from the sole data callback')
            phase=5
            check(writeQword(info.control,1),'release a second actual data store after physical DR removal')
            debug_continueFromBreakpoint(co_run)
            return
        end
        check(phase==2,'only the gated data write produces the armed callback')
        hits=hits+1;check(hits==1,'one actual CE data watchpoint callback')
        check(debug_getCurrentDebuggerInterface()==1,'callback uses the Windows debugger')
        check(RIP==info.code+16 and RAX==0x101,'CE stops after the data store with RAX=0x101')
        local actual=context()
        check(actual.rip==info.code+16 and actual.rax==0x101,'original Windows context matches CE post-store registers')
        local slot=watchSlot(actual)
        check((actual.dr6 & (1 << slot))~=0 and (actual.dr6 & 0x4000)==0,
            'DR6 reports the native data slot hit, not trap-flag single stepping')
        local text=readLog(armedLogOffset)
        local event=string.format('Debug exception 2147483652, PID %d, TID %d',info.pid,info.tid)
        check(text:find(event,1,true)~=nil,'production transport delivered actual EXCEPTION_SINGLE_STEP for the target thread')
        record('ACTUAL_EVENT '..event)
        check(readQword(info.data)==0x101,'data was actually written before the watchpoint stop')
        local hit=snapshot('HIT')
        check(hit.status.useHvm and hit.policy.activeBreakpoints==1 and not hit.policy.canChangeOptions,
            'native data binding remains tracked while HVM is selected')
        debug_removeBreakpoint(info.data)
        record('WATCHPOINT_REMOVED at held data callback')
        phase=3
        debug_continueFromBreakpoint(co_run)
    end)
    if not ok then failure(message) end
    return 1
end
local timer=createTimer(nil,false)
timer.Interval=500
timer.OnTimer=function()
    if done then timer.Enabled=false;return end
    local ok,message=pcall(function()
        if os.time()>deadline then error('timeout in phase '..phase) end
        if phase==0 then
            getMainForm().show()
            check(KSword~=nil and cheatEngineIs64Bit(),'real 64-bit CE production bridge is loaded')
            local file=assert(io.open(root..'\\target.json','r'))
            local text=file:read('*a');file:close()
            for name,value in text:gmatch('"([^"]+)":(%d+)') do info[name]=tonumber(value) end
            for _,name in ipairs({'pid','tid','control','data','code'}) do check(info[name]~=nil and info[name]>0,'target metadata '..name) end
            processHandle=windows('OpenProcess',0x101000,0,info.pid)
            check(processHandle~=nil and processHandle~=0,'retained identity handle for the controlled target')
            threadHandle=windows('OpenThread',0x48,0,info.tid)
            check(threadHandle~=nil and threadHandle~=0 and windows('GetProcessIdOfThread',threadHandle)==info.pid,
                'retained target main-thread identity')
            openProcess(info.pid)
            check(getOpenedProcessID()==info.pid,'CE opens only the controlled DebugTarget')
            local options,errorCode=KSword.setOptions({mode=0,shadowMemoryWrites=false,allowFallback=true,
                logFallback=true,nativeContextFallback=true,nativeSuspendFallback=true,maxShadowPages=32})
            check(errorCode==0 and options~=nil and options.mode==0 and not options.shadowMemoryWrites and
                options.allowFallback and options.logFallback and options.nativeContextFallback and options.nativeSuspendFallback,
                'actual ACK selects normal policy, explicit fallback, and no Shadow memory writes')
            baseline=snapshot('BASELINE')
            check(baseline.policy.activeBreakpoints==0,'no preexisting CE bindings')
            local selected,selectionError=KSword.useHvm(true)
            check(selected,'HVM selection ACK error='..tostring(selectionError))
            check(snapshot('SELECTED').status.useHvm,'HVM selection does not seize the foreign resident')
            check(readQword(info.control)==0 and readQword(info.control+16)==0,'target command gate is initially idle')
            phase=1;attachTime=os.time()
            debugProcess(1)
        elseif phase==1 then
            if not debug_isDebugging() or os.time()-attachTime<3 then return end
            check(debug_getCurrentDebuggerInterface()==1,'real Windows debugger interface attached')
            if not debug_isBroken() then
                if not breakRequested then
                    breakRequested=true;debug_breakThread(info.tid)
                    record('REQUEST_SAFE_PAUSE target main thread')
                end
                return
            end
            check(readQword(info.control)==0 and readQword(info.control+16)==0,'safe attach pause precedes target execution')
            check(#debug_getBreakpointList()==0,'safe pause has no execution breakpoint binding')
            local result=debug_setBreakpointForThread(info.tid,info.data,8,bptWrite,bpmDebugRegister)
            record('INSTALL bptWrite bpmDebugRegister size=8 address='..info.data..' result='..tostring(result))
            local list=debug_getBreakpointList()
            check(#list==1 and list[1]==info.data,'CE lists exactly the requested data watchpoint')
            local armed=snapshot('ARMED')
            check(armed.status.useHvm and armed.policy.activeBreakpoints==1 and not armed.policy.canChangeOptions,
                'actual policy tracks one native binding with HVM selected')
            watchSlot(context())
            local backend,offset=readLog(0)
            check(backend:find('Fallback EPT data watchpoint -> visible native hardware debug register, error 50:',1,true)~=nil and
                backend:find('native context result error=0; no HVM acquisition, stop, or rollback',1,true)~=nil,
                'successful visible data DR fallback has an explicit production log')
            check(armed.policy.fallbackCount>baseline.policy.fallbackCount,'actual fallback counter records the data route')
            armedLogOffset=offset;phase=2
            check(writeQword(info.control,1),'release only the controlled data-store gate')
            debug_continueFromBreakpoint(co_run)
        elseif phase==3 then
            if readQword(info.control+16)==0 then return end
            check(hits==1 and readQword(info.control+16)==1 and readQword(info.control+8)==0x101 and
                readQword(info.data)==0x101,'resumed target actually returns and stores 0x101 once')
            local cleared=snapshot('CLEARED')
            check(cleared.policy.activeBreakpoints==0 and cleared.policy.canChangeOptions,'native binding cleanup is acknowledged')
            local backend=readLog(armedLogOffset)
            check(backend:find('Native data watchpoint clear: result error=0; no HVM acquisition, stop, or rollback',1,true)~=nil,
                'production log confirms successful native data DR clear without HVM takeover')
            recordBreakpointBookkeeping('CLEARED')
            phase=4
            debug_breakThread(info.tid)
            record('REQUEST_CLEAR_VERIFICATION_PAUSE target main thread')
        elseif phase==5 then
            if readQword(info.control+16)<2 then return end
            check(hits==1 and readQword(info.control+16)==2 and readQword(info.control+8)==0x101 and
                readQword(info.data)==0x101,'second actual store completes with no additional data callback')
            local repeated=snapshot('AFTER_SECOND_STORE')
            check(repeated.policy.activeBreakpoints==0 and repeated.policy.canChangeOptions,
                'second store leaves backend bindings cleared')
            recordBreakpointBookkeeping('AFTER_SECOND_STORE')
            detachIfPossible()
            phase=6
        elseif phase==6 then
            if debug_isDebugging() then return end
            local detached=snapshot('DETACHED')
            check(detached.status.attachedProcessId==0,'actual backend session is detached')
            check(writeQword(info.control,9),'request target clean exit after detaching')
            phase=7
        elseif phase==7 then
            if windows('WaitForSingleObject',processHandle,0)==258 then return end
            check(windows('WaitForSingleObject',processHandle,0)==0,'test-owned target has actually exited')
            local code=createMemoryStream();code.Size=4
            local result=windows('GetExitCodeProcess',processHandle,code.Memory)
            local exitCode=readIntegerLocal(code.Memory);code.destroy()
            check(result~=nil and result~=0 and exitCode==0,'test-owned target exited successfully')
            local final=snapshot('FINAL')
            check(final.policy.activeBreakpoints==0 and final.status.attachedProcessId==0,'no debugger bindings or attachment remain')
            closeHandles()
            record('FINAL PASS CE native data watchpoint, real #DB, target result 0x101, foreign HVM preserved')
            done=true;log:close();timer.Enabled=false
        end
    end)
    if not ok then failure(message) end
end
timer.Enabled=true
_G.KSwordGuestDataTestTimer=timer
