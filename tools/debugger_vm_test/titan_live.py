"""Real production Titan adapter session against DebugTarget inside the named VM."""
from pathlib import Path
import ctypes as C
from ctypes import wintypes as W
import json, os, subprocess, sys, time, traceback

root=Path(sys.argv[1]).resolve()
mode=sys.argv[2]
assert os.name=='nt' and mode in ('native','hvm')
os.environ['KSWORD_DEBUGGER_LOG_FILE']=str(root/f'titan-{mode}-backend.log')
stage=root/'x64dbg/bin/x64'
os.add_dll_directory(str(stage))
dll=C.CDLL(str(stage/'KSword/TitanEngine.dll'),use_last_error=True)
def fn(name,result,args):
    f=getattr(dll,name);f.restype=result;f.argtypes=args;return f
init=fn('KSwordTitanInitialize',W.DWORD,[])
assert init()==0
class Status(C.Structure):
    _fields_=[(n,W.DWORD) for n in ('version','size','driver','hvm','window','resident','owns','pid','ept','error')]
control=fn('KSwordTitanControl',W.DWORD,[W.DWORD,C.POINTER(Status)])
openprocess=fn('TitanOpenProcess',W.HANDLE,[W.DWORD,C.c_bool,W.DWORD])
openthread=fn('TitanOpenThread',W.HANDLE,[W.DWORD,C.c_bool,W.DWORD])
close=fn('TitanCloseHandle',C.c_bool,[W.HANDLE])
read=fn('MemoryReadSafe',C.c_bool,[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)])
write=fn('MemoryWriteSafe',C.c_bool,[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)])
getreg=fn('GetContextDataEx',C.c_size_t,[W.HANDLE,C.c_int])
setreg=fn('SetContextDataEx',C.c_bool,[W.HANDLE,C.c_int,C.c_size_t])
getfull=fn('GetFullContextDataEx',C.c_bool,[W.HANDLE,C.c_void_p])
getavx=fn('GetAVXContext',C.c_bool,[W.HANDLE,C.c_void_p])
event=fn('GetDebugData',C.c_void_p,[])
CB=C.CFUNCTYPE(None,C.c_void_p)
STEP=C.CFUNCTYPE(None)
sethandler=fn('SetCustomHandler',None,[C.c_int,CB])
sethw=fn('SetHardwareBreakPoint',C.c_bool,[C.c_size_t,W.DWORD,C.c_int,C.c_int,CB])
deletehw=fn('DeleteHardwareBreakPoint',C.c_bool,[W.DWORD])
stepinto=fn('StepInto',None,[STEP])
stop=fn('StopDebug',C.c_bool,[])
detach=fn('DetachDebuggerEx',C.c_bool,[W.DWORD])
attach=fn('AttachDebugger',C.c_bool,[W.DWORD,C.c_bool,C.c_void_p,STEP])
target=root/'DebugTarget.exe'
infofile=root/'target.json'
infofile.unlink(missing_ok=True)
process=subprocess.Popen([str(target),str(root)],creationflags=subprocess.CREATE_NO_WINDOW)
for i in range(100):
    if infofile.exists(): break
    time.sleep(.05)
info=json.loads(infofile.read_text())
assert info['pid']==process.pid
ph=openprocess(0x1fffff,False,process.pid)
th=openthread(0x1fffff,False,info['tid'])
assert ph and th
failures=[]; assertions=[]; hits=0; stepped=False
def check(value,message):
    assert value, f'{message}: Win32={C.get_last_error()}'
    assertions.append(message);print('PASS '+message,flush=True)
def read64(address):
    value=C.c_uint64();n=C.c_size_t()
    check(read(ph,address,C.byref(value),8,C.byref(n)) and n.value==8,'read 8 bytes')
    return value.value
def write64(address,value):
    v=C.c_uint64(value);n=C.c_size_t()
    check(write(ph,address,C.byref(v),8,C.byref(n)) and n.value==8,'write 8 bytes')
def safe(action):
    def wrapped(*args):
        try: action(*args)
        except BaseException:
            failures.append(traceback.format_exc()); print(failures[-1],flush=True);stop()
    return wrapped
@STEP
@safe
def afterstep():
    global stepped
    print('STEP_ACTUAL='+json.dumps({'rip':getreg(th,25),'expected':info['code']+6,'rax':getreg(th,17),'event':C.string_at(event(),16).hex()}),flush=True)
    check(getreg(th,25)==info['code']+6,'StepInto retired exactly one 3-byte instruction')
    check(getreg(th,17)==0x1235,'edited RAX participated in resumed instruction')
    stepped=True
    write64(info['control'],1)
@CB
@safe
def onhit(_):
    global hits
    hits+=1
    raw=C.string_at(event(),16)
    check(int.from_bytes(raw[4:8],'little')==process.pid and int.from_bytes(raw[8:12],'little')==info['tid'],'held Windows event belongs to exact target thread')
    check(getreg(th,25)==info['code']+3,'execute hardware breakpoint stopped at exact RIP')
    if mode=='hvm':
        st=Status();check(control(0,C.byref(st))==170,'HVM disable rejected with live EPT bindings')
    context=C.create_string_buffer(1088)
    check(getfull(th,context),'full GPR flags x87 SSE context read')
    check(getavx(th,context),'AVX context read')
    if hits==1:
        check(getreg(th,17)==0x100,'instruction has not executed before stop')
        check(setreg(th,17,0x1234) and getreg(th,17)==0x1234,'GPR edit readback')
        stepinto(afterstep)
    else:
        check(stepped,'step callback observed')
        check(read64(info['data'])==0x1235,'register edit and continuation reached target memory')
        check(deletehw(11),'delete hardware breakpoint')
        st=Status();check(control(0,C.byref(st))==0,'mode disabled after retirement')
        check(st.resident==0 and st.owns==0,'owned HVM residency released')
        check(detach(process.pid),'detach debugger leaves target alive')
@CB
@safe
def onsystem(_):
    st=Status();err=control(1 if mode=='hvm' else 0,C.byref(st))
    print('STATUS='+json.dumps({n:getattr(st,n) for n,_ in st._fields_})+' ERROR='+str(err),flush=True)
    check(err==0,'requested backend selected')
    if mode=='hvm':
        check(st.window==1,'HVM direct memory protocol available')
        check(not sethw(info['code']+3,11,5,7,onhit) and C.get_last_error()==50,'generic HVM data hardware breakpoint explicitly unsupported')
        check(sethw(info['code'],11,4,7,onhit),'initial ShadowPage hardware slot')
    check(sethw(info['code']+3,11,4,7,onhit),'install execute hardware breakpoint')
    if mode=='hvm':
        check(sethw(info['code']+3,11,4,7,onhit),'replace ShadowPage slot callback at same address')
        check(sethw(info['code']+6,12,4,7,onhit),'merge second breakpoint on same ShadowPage')
        check(deletehw(12),'remove second ShadowPage breakpoint preserves first')
        original=C.c_ubyte();n=C.c_size_t()
        check(read(ph,info['code']+3,C.byref(original),1,C.byref(n)) and original.value==0x48,
            'ShadowPage INT3 remains hidden from frontend memory reads')
    write64(info['control'],1)
@STEP
def attached(): onsystem(None)
try:
    sethandler(23,onsystem)
    check(attach(process.pid,False,None,attached),'real native debug loop attach and detach completed')
    check(hits==2 and stepped and not failures,'repeat EPT hit with one exact step completed')
    write64(info['control'],9)
    process.wait(timeout=10)
finally:
    close(th);close(ph)
    if process.poll() is None: process.terminate();process.wait(timeout=5)
    (root/f'titan-{mode}-result.json').write_text(json.dumps({'mode':mode,'hits':hits,'stepped':stepped,'assertions':assertions,'failures':failures},indent=2))
if failures: raise SystemExit(1)
print('FINAL PASS '+mode,flush=True)
