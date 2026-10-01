from pathlib import Path
import ctypes as C, struct, json, os, sys
root=Path(sys.argv[1]);os.add_dll_directory(str(root/'x64dbg/bin/x64'))
os.environ['KSWORD_DEBUGGER_LOG_FILE']=str(root/'protocol-backend.log')
dll=C.CDLL(str(root/'x64dbg/bin/x64/KSword/TitanEngine.dll'),use_last_error=True)
class Call(C.Structure):
    _fields_=[('version',C.c_uint32),('size',C.c_uint32),('command',C.c_uint32),('reserved',C.c_uint32),('input',C.c_uint64),('output',C.c_uint64),('inputBytes',C.c_uint32),('outputBytes',C.c_uint32),('error',C.c_uint32),('returned',C.c_uint32)]
api=dll.KSwordDebuggerCall;api.restype=C.c_uint32;api.argtypes=[C.POINTER(Call)]
def invoke(command,req,size):
    inp=C.create_string_buffer(req,len(req));out=C.create_string_buffer(size)
    call=Call(1,48,command,0,C.addressof(inp),C.addressof(out),len(req),size,0,0)
    err=api(C.byref(call));return err,call.returned,out.raw[:call.returned]
operations={16:None,17:0,18:6,19:4,20:1,21:4,22:3,23:4,24:4,25:0,26:0,27:None,28:None,30:0,31:0,32:0}
results=[]
for command,op in operations.items():
    err,returned,layout=invoke(3,struct.pack('<I',command),24)
    assert err==0 and returned==24
    _,_,ins,outs,version,_=struct.unpack('<6I',layout)
    req=bytearray(ins);struct.pack_into('<II',req,0,version,ins)
    if op is not None:struct.pack_into('<I',req,8,op)
    err,returned,data=invoke(command,bytes(req),outs)
    item={'command':command,'version':version,'input':ins,'output':outs,'transport':err,'returned':returned,'prefix':data[:64].hex()}
    results.append(item);print(json.dumps(item),flush=True)
info=json.loads((root/'target.json').read_text())
req=bytearray(1296);struct.pack_into('<8I',req,0,1,1296,1,0,info['pid'],info['tid'],0x100017,0)
struct.pack_into('<I',req,64+48,0x100017)
err,returned,data=invoke(32,bytes(req),1296)
print('GET_CONTEXT='+json.dumps({'transport':err,'returned':returned,'status':hex(struct.unpack_from('<I',data,8)[0]),'contextflags':hex(struct.unpack_from('<I',data,64+48)[0]),'rip':hex(struct.unpack_from('<Q',data,64+248)[0])}),flush=True)
(root/'protocol-results.json').write_text(json.dumps(results,indent=2))
