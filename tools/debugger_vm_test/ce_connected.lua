-- Guest-only check: run with KswordARK stopped and the production bridge loaded.
local root=assert(os.getenv('KSWORD_TEST_ROOT'))
local log=assert(io.open(root..'\\ce-connected.log','w'))
local function record(s) log:write(s,'\n');log:flush() end
local function check(v,s) if not v then error(s) end record('PASS '..s) end
local timer=createTimer(nil,false)
timer.Interval=1500
timer.OnTimer=function()
    timer.Enabled=false
    local ok,err=pcall(function()
        check(cheatEngineIs64Bit(),'CE is 64 bit')
        check(KSword~=nil,'production bridge loaded without driver')
        local st=assert(KSword.status())
        check(not st.driverReady and not st.useHvm,'backend accurately reports disconnected R0')
        getMainForm().show()
        record('TITLE '..getMainForm().Caption)
        check(getMainForm().Caption:find('[KSword Connected]',1,true)~=nil,'CE title reports KSword Connected')
        local f=assert(io.open(root..'\\target.json','r'));local text=f:read('*a');f:close()
        local info={}
        for k,v in text:gmatch('"([^"]+)":(%d+)') do info[k]=tonumber(v) end
        openProcess(info.pid)
        check(getOpenedProcessID()==info.pid,'native CE opens target without driver')
        check(writeQword(info.data,0x11223344) and readQword(info.data)==0x11223344,'native CE read/write remains usable')
        check(writeQword(info.control,9),'target clean exit')
        record('FINAL PASS CE Connected')
    end)
    if not ok then record('FAIL '..tostring(err)) end
    log:close()
end
timer.Enabled=true
_G.KSwordConnectedTest=timer
