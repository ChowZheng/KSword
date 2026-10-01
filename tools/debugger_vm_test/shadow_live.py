"""Real Windows debug events plus the KSword EPTP ShadowPage backend."""
from pathlib import Path
import ctypes as C, ctypes.wintypes as W, struct, json, os, sys, subprocess, time

root = Path(sys.argv[1])
os.add_dll_directory(str(root / 'x64dbg/bin/x64'))
dll = C.CDLL(str(root / 'x64dbg/bin/x64/KSword/TitanEngine.dll'))
class Call(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('version','size','command','reserved')] + [
        ('input',C.c_uint64),('output',C.c_uint64)] + [(n,C.c_uint32) for n in ('inputBytes','outputBytes','error','returned')]
api = dll.KSwordDebuggerCall; api.argtypes = [C.POINTER(Call)]; api.restype = W.DWORD
def invoke(command, request, output_size):
    inp = C.create_string_buffer(request, len(request)); out = C.create_string_buffer(output_size)
    call = Call(1,48,command,0,C.addressof(inp),C.addressof(out),len(request),output_size,0,0)
    error = api(C.byref(call)); assert error == 0, (command,error)
    assert call.returned == output_size, (command,call.returned,output_size)
    return out.raw
def packet(command, fields):
    layout = invoke(3,struct.pack('<I',command),24)
    _,_,ins,outs,version,_ = struct.unpack('<6I',layout)
    data = bytearray(ins); struct.pack_into('<II',data,0,version,ins)
    for offset,fmt,value in fields: struct.pack_into('<'+fmt,data,offset,value)
    return data,outs
def send(command,fields):
    data,outs = packet(command,fields); return invoke(command,bytes(data),outs)
def status(): return send(16,[])
generation = 0
def control(command,flags=1):
    global generation
    result=send(17,[(8,'I',command),(12,'I',flags),(16,'I',0x48564d43),(20,'I',generation)])
    code=struct.unpack_from('<I',result,8)[0]
    print('CONTROL',command,code,result[:40].hex(),flush=True)
    assert code==0,(command,code,result[:40].hex())
    generation=struct.unpack_from('<I',result,24)[0]
    return result
k=C.WinDLL('kernel32',use_last_error=True)
def bind(name,result,args):
    f=getattr(k,name);f.restype=result;f.argtypes=args;return f
attach=bind('DebugActiveProcess',W.BOOL,[W.DWORD]); detach=bind('DebugActiveProcessStop',W.BOOL,[W.DWORD])
wait=bind('WaitForDebugEvent',W.BOOL,[C.c_void_p,W.DWORD]);cont=bind('ContinueDebugEvent',W.BOOL,[W.DWORD,W.DWORD,W.DWORD])
op=bind('OpenProcess',W.HANDLE,[W.DWORD,W.BOOL,W.DWORD]);ot=bind('OpenThread',W.HANDLE,[W.DWORD,W.BOOL,W.DWORD])
read=bind('ReadProcessMemory',W.BOOL,[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p])
write=bind('WriteProcessMemory',W.BOOL,[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p])
get=bind('GetThreadContext',W.BOOL,[W.HANDLE,C.c_void_p]);setctx=bind('SetThreadContext',W.BOOL,[W.HANDLE,C.c_void_p])
close=bind('CloseHandle',W.BOOL,[W.HANDLE])
def rd(address,length):
    b=C.create_string_buffer(length);assert read(ph,address,b,length,None),C.get_last_error();return b.raw
def wr(address,value):
    b=C.c_uint64(value);assert write(ph,address,C.byref(b),8,None),C.get_last_error()
def context():
    b=C.create_string_buffer(1248);p=(C.addressof(b)+15)&~15
    C.c_uint32.from_address(p+48).value=0x10001f
    assert get(th,p),C.get_last_error();return b,p
def add_view():
    req,outs=packet(21,[(8,'I',1),(12,'I',2),(16,'I',1),(20,'I',0x48564d43),(32,'Q',physical)])
    req[40:40+4096]=source;req[43]=0xcc
    result=invoke(21,bytes(req),outs)
    assert struct.unpack_from('<I',result,8)[0]==0,result[:40].hex()
    return struct.unpack_from('<I',result,12)[0]
def remove_view():
    result=send(21,[(8,'I',2),(16,'I',1),(20,'I',0x48564d43),(24,'I',view)])
    assert struct.unpack_from('<I',result,8)[0]==0,result[:40].hex()
assert dll.KSwordTitanInitialize()==0
infofile=root/'target.json';infofile.unlink(missing_ok=True)
target=subprocess.Popen([str(root/'DebugTarget.exe'),str(root)],creationflags=subprocess.CREATE_NO_WINDOW)
for _ in range(100):
    if infofile.exists():break
    time.sleep(.05)
info=json.loads(infofile.read_text());assert info['pid']==target.pid
ph=op(0x1fffff,False,target.pid);th=ot(0x1fffff,False,info['tid']);assert ph and th
view=0;prepared=False;resident=False;hits=0;passed=False;held=None
try:
    assert attach(target.pid),C.get_last_error()
    bind('DebugSetProcessKillOnExit',W.BOOL,[W.BOOL])(False)
    deadline=time.monotonic()+45
    while time.monotonic()<deadline:
        event=C.create_string_buffer(176)
        if not wait(event,1000):continue
        code,pid,tid=struct.unpack_from('<3I',event.raw);held=(pid,tid)
        exception=struct.unpack_from('<I',event.raw,16)[0] if code==1 else 0
        address=struct.unpack_from('<Q',event.raw,32)[0] if code==1 else 0
        print('EVENT',code,pid,tid,hex(exception),hex(address),flush=True)
        if exception==0x80000003 and not prepared:
            control(1,1|4|0x400);prepared=True
            control(2,1|2|4)
            source=rd(info['code'],4096)
            translated=send(18,[(8,'I',5),(12,'I',3),(16,'I',0x48564d43),(24,'I',pid),(32,'Q',info['code'])])
            assert struct.unpack_from('<I',translated,8)[0]==0,translated[:32].hex()
            physical=struct.unpack_from('<Q',translated,16)[0]&~4095
            view=add_view();control(5,1|2|4|0x10);resident=True
            assert rd(info['code']+3,1)==source[3:4],'INT3 leaked into read view'
            print('PASS hidden byte: ordinary reads still see original instruction',flush=True)
            wr(info['control'],1)
        elif exception==0x80000003 and address==info['code']+3:
            hits+=1;print('PASS real ShadowPage INT3 hit',hits,flush=True)
            control(6,1);resident=False;remove_view();view=0
            b,p=context();assert C.c_uint64.from_address(p+248).value==info['code']+4
            C.c_uint64.from_address(p+248).value=info['code']+3
            if hits==1:
                C.c_uint64.from_address(p+120).value=0x1234
                C.c_uint32.from_address(p+68).value|=0x100
            assert setctx(th,p),C.get_last_error()
            if hits==2:
                assert detach(pid),C.get_last_error();held=None
                assert rd(info['code'],4096)==source
                control(3,1);prepared=False
                wr(info['control'],9);target.wait(timeout=10);passed=True;break
        elif exception==0x80000004 and hits==1:
            b,p=context()
            assert C.c_uint64.from_address(p+248).value==info['code']+6
            assert C.c_uint64.from_address(p+120).value==0x1235
            print('PASS register edit and exact one-instruction step',flush=True)
            C.c_uint32.from_address(p+68).value&=~0x100;assert setctx(th,p)
            view=add_view();control(5,1|2|4|0x10);resident=True;wr(info['control'],1)
        assert cont(pid,tid,0x10002),C.get_last_error();held=None
    assert passed,'ShadowPage deadline exceeded'
finally:
    if resident:control(6,1)
    if view:remove_view()
    if prepared:control(3,1)
    if held:cont(*held,0x10002)
    detach(target.pid);close(th);close(ph)
    if target.poll() is None:target.terminate();target.wait(timeout=5)
print('FINAL PASS ShadowPage EPTP fallback without MTF',flush=True)
