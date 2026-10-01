"""Pinned ShadowPage cleanup on owner/target death in the disposable VM."""
from pathlib import Path
import ctypes as C, struct, json, os, sys, subprocess, time

root=Path(sys.argv[1]).resolve()
os.add_dll_directory(str(root/'x64dbg/bin/x64'))
dll=C.CDLL(str(root/'x64dbg/bin/x64/KSword/TitanEngine.dll'))
class Call(C.Structure):
    _fields_=[(n,C.c_uint32) for n in ('version','size','command','reserved')]+[
        ('input',C.c_uint64),('output',C.c_uint64)]+[(n,C.c_uint32) for n in ('inputBytes','outputBytes','error','returned')]
api=dll.KSwordDebuggerCall;api.argtypes=[C.POINTER(Call)];api.restype=C.c_uint32
assert dll.KSwordTitanInitialize()==0
def invoke(command,request,size):
    inp=C.create_string_buffer(request,len(request));out=C.create_string_buffer(size)
    call=Call(1,48,command,0,C.addressof(inp),C.addressof(out),len(request),size,0,0)
    assert api(C.byref(call))==0,(command,call.error)
    assert call.returned==size
    return out.raw
def send(command,fields):
    _,_,ins,outs,version,_=struct.unpack('<6I',invoke(3,struct.pack('<I',command),24))
    data=bytearray(ins);struct.pack_into('<II',data,0,version,ins)
    for offset,fmt,value in fields:struct.pack_into('<'+fmt,data,offset,value)
    return invoke(command,bytes(data),outs)
def ctl(command,flags=1):
    result=send(17,[(8,'I',command),(12,'I',flags),(16,'I',0x48564d43)])
    assert struct.unpack_from('<I',result,8)[0]==0,result[:40].hex()
def shadow(pid,address):
    result=send(32,[(8,'I',8),(12,'I',1),(16,'I',pid),(32,'Q',address),(40,'Q',1)])
    assert struct.unpack_from('<i',result,8)[0]>=0,result[:64].hex()
    assert struct.unpack_from('<Q',result,48)[0]!=0,'Shadow view ID missing'
def view_count():
    result=send(21,[(8,'I',4)])
    assert struct.unpack_from('<I',result,8)[0]==0
    return struct.unpack_from('<I',result,16)[0]
def resident():
    result=send(16,[])
    # Exact v6 shared/driver/KswordArkHvmIoctl.h prefix, including SVM evidence.
    assert struct.unpack_from('<I',result,0)[0]==6
    return struct.unpack_from('<I',result,152)[0]
if len(sys.argv)>2 and sys.argv[2]=='owner':
    info=json.loads((root/'target.json').read_text())
    shadow(info['pid'],info['code']+3)
    (root/'shadow-owner.ready').write_text('ready')
    time.sleep(60)
    raise SystemExit(0)

assert resident()==0 and view_count()==0,'Other HVM users are active'
for case in ('owner','target'):
    infofile=root/'target.json';infofile.unlink(missing_ok=True)
    ready=root/'shadow-owner.ready';ready.unlink(missing_ok=True)
    target=subprocess.Popen([str(root/'DebugTarget.exe'),str(root)],creationflags=subprocess.CREATE_NO_WINDOW)
    owner=None;prepared=False
    try:
        for _ in range(100):
            if infofile.exists():break
            time.sleep(.05)
        info=json.loads(infofile.read_text());assert info['pid']==target.pid
        ctl(1,1|4|0x400);prepared=True
        ctl(2,1|2|4)
        if case=='owner':
            owner=subprocess.Popen([sys.executable,__file__,str(root),'owner'],creationflags=subprocess.CREATE_NO_WINDOW)
            for _ in range(100):
                if ready.exists():break
                if owner.poll() is not None:raise AssertionError('Owner exited before installation')
                time.sleep(.05)
            assert ready.exists()
        else:shadow(target.pid,info['code']+3)
        assert view_count()==1,'Pinned shadow not installed'
        ctl(5,1|2|4|16)
        assert resident()==2,'Both vCPUs must be resident'
        dying=owner if case=='owner' else target
        dying.terminate();dying.wait(timeout=15)
        for _ in range(100):
            if resident()==0 and view_count()==0:break
            time.sleep(.05)
        assert resident()==0 and view_count()==0,'Process death retained HVM execution views'
        print('PASS '+case+' death revokes pinned ShadowPage and both resident processors',flush=True)
    finally:
        for process in (owner,target):
            if process is not None and process.poll() is None:process.terminate();process.wait(timeout=15)
        if resident()!=0:ctl(6)
        if prepared:ctl(3)
    assert resident()==0 and view_count()==0
print('FINAL PASS ShadowPage owner/target cleanup',flush=True)
