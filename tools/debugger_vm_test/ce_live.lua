-- Guest test only. Installed in the disposable test payload's autorun/custom.
local root=assert(os.getenv('KSWORD_TEST_ROOT'))
local log=assert(io.open(root..'\\ce-live.log','w'))
local function record(s) log:write(s,'\n');log:flush() end
local function check(v,s) if not v then error(s) end record('PASS '..s) end
local done=false
local function failure(err) record('FAIL '..tostring(err));done=true end
local phase=0
local info={}
local deadline=os.time()+60
local function scan(value,label)
    local ms=createMemScan()
    ms.firstScan(soExactValue,vtQword,rtRounded,string.format('%.0f',value),'',
        info.data,info.data+4095,'+W',fsmAligned,'8',false,false,false,false)
    ms.waitTillDone()
    local fl=createFoundList(ms)
    fl.initialize()
    check(fl.Count==1 and tonumber(fl.Address[0],16)==info.data,label..' exact-value first scan')
    fl.deinitialize()
    check(writeQword(info.data,value+1),label..' scan value update')
    ms.nextScan(soExactValue,rtRounded,string.format('%.0f',value+1),'',false,false,false,false,false)
    ms.waitTillDone()
    fl.initialize()
    check(fl.Count==1 and tonumber(fl.Address[0],16)==info.data,label..' exact-value next scan')
    fl.destroy();ms.destroy()
    check(writeQword(info.data,value),label..' scan value restored')
end
local timer=createTimer(nil,false)
timer.Interval=500
timer.OnTimer=function()
    if done then timer.Enabled=false;return end
    local ok,err=pcall(function()
        if os.time()>deadline then error('timeout in phase '..phase) end
        if phase==0 then
            getMainForm().show()
            check(KSword~=nil,'production CE bridge and Lua API loaded')
            check(cheatEngineIs64Bit(),'CE is 64 bit')
            local status, se=KSword.status()
            record('INITIAL_STATUS error='..tostring(se)..' status='..tostring(status))
            local mod=executeCodeLocalEx(getAddressSafe('kernel32.GetModuleHandleW',true),{type=4,value='KswordCheatEnginePlugin.dll'})
            local proc=executeCodeLocalEx(getAddressSafe('kernel32.GetProcAddress',true),{type=0,value=mod},{type=3,value='KSwordDebuggerCall'})
            local call=createMemoryStream();call.Size=48
            writeBytesLocal(call.Memory,stringToByteTable(string.rep('\0',48)))
            writeIntegerLocal(call.Memory,1);writeIntegerLocal(call.Memory+4,48)
            writeIntegerLocal(call.Memory+8,1)
            local out=createMemoryStream();out.Size=40
            writeQwordLocal(call.Memory+24,out.Memory);writeIntegerLocal(call.Memory+36,40)
            record('ABI_MEMORY='..tostring(call.Memory)..' MODULE='..tostring(mod)..' PROC='..tostring(proc)..' HEADER='..table.concat(readBytesLocal(call.Memory,48,true),','))
            record('ABI_TYPED_RESULT='..tostring(executeCodeLocalEx(proc,{type=0,value=call.Memory}))..' RETURNED='..tostring(readIntegerLocal(call.Memory+44)))
            call.destroy();out.destroy()
            local f=assert(io.open(root..'\\target.json','r'));local text=f:read('*a');f:close()
            for k,v in text:gmatch('"([^"]+)":(%d+)') do info[k]=tonumber(v) end
            openProcess(info.pid)
            check(getOpenedProcessID()==info.pid,'opened controlled target through CE')
            local wrote=writeQword(info.data,0x11223344);local read=readQword(info.data)
            record('R0_MEMORY wrote='..tostring(wrote)..' read='..tostring(read)..' address='..tostring(info.data))
            check(wrote and read==0x11223344,'R0 memory write and read')
            scan(0x11223344,'R0')
            local a=allocateMemory(4096,nil,PAGE_READWRITE)
            check(a~=nil and a~=0,'R0 allocation')
            check(writeQword(a,0x778899) and readQword(a)==0x778899,'allocated memory write and read')
            check(fullAccess(a,4096),'R0 memory protection')
            deAlloc(a)
            local st=assert(KSword.status())
            check(getMainForm().Caption:find('[KSword R0]',1,true)~=nil,'CE title reports KSword R0')
            record('STATUS_R0 window='..tostring(st.directMemoryWindow)..' ept='..tostring(st.eptBreakpointProtocol))
            local selected,e=KSword.useHvm(true)
            check(selected,'HVM memory selection error='..tostring(e))
            check(writeQword(info.data,0x55667788) and readQword(info.data)==0x55667788,'HVM strict private-window memory write and read')
            scan(0x55667788,'HVM')
            check(KSword.useHvm(false),'switch back to R0')
            if os.getenv('KSWORD_TEST_USE_HVM')=='1' then
                check(KSword.useHvm(true),'HVM breakpoint fallback selected')
            end
            debugProcess(1)
            _G.KSwordGuestAttachTime=os.time()
            phase=1
        elseif phase==1 then
            if not debug_isDebugging() then return end
            if os.time()-KSwordGuestAttachTime<3 then return end
            check(debug_getCurrentDebuggerInterface()==1,'Windows debugger interface')
            _G.debugger_onBreakpoint=function()
                local success,message=pcall(function()
                    if phase==2 then
                        check(RIP==info.code+3,'execution breakpoint exact RIP')
                        check(RAX==0x100,'instruction not yet executed')
                        RAX=0x1234
                        phase=3
                        debug_continueFromBreakpoint(co_stepinto)
                    elseif phase==3 then
                        record('STEP_ACTUAL rip='..tostring(RIP)..' expected='..tostring(info.code+6)..' rax='..tostring(RAX))
                        check(RIP==info.code+6 and RAX==0x1235,'one instruction step and register edit')
                        phase=4
                        debug_continueFromBreakpoint(co_run)
                    elseif phase==5 then
                        check(RIP==info.code+3 and RAX==0x100,'repeat execute breakpoint stops before instruction')
                        debug_removeBreakpoint(info.code+3)
                        phase=6
                        debug_continueFromBreakpoint(co_run)
                    end
                end)
                if not success then failure(message) end
                return 1
            end
            local method=os.getenv('KSWORD_TEST_BREAKPOINT_METHOD')=='int3' and bpmInt3 or bpmDebugRegister
            record('BREAKPOINT_METHOD='..tostring(method))
            local bpResult=debug_setBreakpoint(info.code+3,1,bptExecute,method)
            record('BREAKPOINT_INSTALL_RESULT='..tostring(bpResult)..' broken='..tostring(debug_isBroken()))
            record('BREAKPOINT_LIST='..table.concat(debug_getBreakpointList(),','))
            if os.getenv('KSWORD_TEST_USE_HVM')=='1' then
                check(readBytes(info.code+3,1)==0x48,'ShadowPage INT3 hidden from CE memory reads')
                check(getMainForm().Caption:find('[KSword HVM]',1,true)~=nil,'CE title reports KSword HVM')
            end
            phase=2
            check(writeQword(info.control,1),'release target gate')
        elseif phase==4 then
            if readQword(info.control+16)==0 then return end
            check(readQword(info.data)==0x1235,'edited register reached target memory on resume')
            phase=5
            check(writeQword(info.control,1),'release target gate for repeat breakpoint')
        elseif phase==6 then
            if readQword(info.control+16)<2 then return end
            check(readQword(info.data)==0x101,'removed breakpoint allows target instruction to execute')
            detachIfPossible()
            check(writeQword(info.control,9),'target clean exit')
            record('FINAL PASS CE repeat breakpoints, context, step and memory');done=true
        end
    end)
    if not ok then failure(err) end
end
timer.Enabled=true
_G.KSwordGuestTestTimer=timer
