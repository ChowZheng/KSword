#include "../../Ksword5.1/Ksword5.1/MemoryDock/PoolTraceReader.cpp"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace ks::evidence::pool;
namespace {
int checks=0;
void check(bool ok) { ++checks; if (!ok) { std::fprintf(stderr,"Pool reader check failed: %d\n",checks); throw std::runtime_error("Pool reader regression failure"); } }
void put(std::vector<unsigned char>& b,std::size_t off,std::uint64_t value,std::size_t width) { std::memcpy(b.data()+off,&value,width); }
EVENT_RECORD record(const GUID& guid,unsigned opcode,unsigned version,unsigned width,std::vector<unsigned char>& data) {
    EVENT_RECORD r{}; r.EventHeader.ProviderId=guid;
    r.EventHeader.Flags=EVENT_HEADER_FLAG_CLASSIC_HEADER | (width==4 ? EVENT_HEADER_FLAG_32_BIT_HEADER : EVENT_HEADER_FLAG_64_BIT_HEADER);
    r.EventHeader.EventDescriptor.Version=static_cast<UCHAR>(version); r.EventHeader.EventDescriptor.Opcode=static_cast<UCHAR>(opcode);
    r.EventHeader.TimeStamp.QuadPart=12345; r.EventHeader.ProcessId=123; r.EventHeader.ThreadId=456;
    r.UserData=data.data(); r.UserDataLength=static_cast<USHORT>(data.size()); return r;
}
struct PrivateTraceState { TRACEHANDLE logger=0; };
ULONG WINAPI privateProviderCallback(WMIDPREQUESTCODE request,void* context,ULONG*,void* buffer) {
    auto& state=*static_cast<PrivateTraceState*>(context);
    if (request==WMI_ENABLE_EVENTS) { state.logger=GetTraceLoggerHandle(buffer); }
    else if (request==WMI_DISABLE_EVENTS) { state.logger=0; }
    return ERROR_SUCCESS;
}
void nativeIntegration() {
    // This logger records only this process's fake classic-provider events.
    // It never subscribes to a system/kernel provider or a shared session.
    GUID provider{}; HMODULE module=LoadLibraryW(L"ole32.dll");
    using CreateGuid=HRESULT (WINAPI*)(GUID*); CreateGuid createGuid=nullptr;
    auto procedure=module ? GetProcAddress(module,"CoCreateGuid") : nullptr;
    static_assert(sizeof(createGuid)==sizeof(procedure)); std::memcpy(&createGuid,&procedure,sizeof(createGuid));
    const bool guidKnown=createGuid && SUCCEEDED(createGuid(&provider)); if (module) { FreeLibrary(module); }
    if (!guidKnown) { std::puts("POOL_NATIVE_ETL_INTEGRATION=SKIP GUID_UNAVAILABLE"); return; }
    wchar_t name[128]{};
    swprintf_s(name,L"KSwordPoolReader_%08lx_%04x_%04x_%02x%02x%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned long>(provider.Data1),provider.Data2,provider.Data3,
        provider.Data4[0],provider.Data4[1],provider.Data4[2],provider.Data4[3],provider.Data4[4],provider.Data4[5],provider.Data4[6],provider.Data4[7]);
    PrivateTraceState state; TRACEHANDLE registration=0;
    GUID poolClass=poolGuid,stackClass=stackGuid;
    TRACE_GUID_REGISTRATION classes[]{{&poolClass,nullptr},{&stackClass,nullptr}};
    auto status=RegisterTraceGuidsW(privateProviderCallback,&state,&provider,2,classes,nullptr,nullptr,&registration);
    if (status!=ERROR_SUCCESS) { std::printf("POOL_NATIVE_ETL_INTEGRATION=SKIP REGISTER_STATUS=%lu\n",static_cast<unsigned long>(status)); return; }
    wchar_t tmp[MAX_PATH]{},path[MAX_PATH]{}; check(GetTempPathW(MAX_PATH,tmp)!=0); check(GetTempFileNameW(tmp,L"KPE",0,path)!=0);
    std::vector<unsigned char> storage(sizeof(EVENT_TRACE_PROPERTIES)+2*1024*sizeof(wchar_t));
    auto* properties=reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
    properties->Wnode.BufferSize=static_cast<ULONG>(storage.size()); properties->Wnode.Guid=provider;
    properties->Wnode.ClientContext=1; properties->Wnode.Flags=WNODE_FLAG_TRACED_GUID;
    properties->BufferSize=64; properties->MinimumBuffers=2; properties->MaximumBuffers=2; properties->MaximumFileSize=1;
    properties->LogFileMode=EVENT_TRACE_PRIVATE_LOGGER_MODE | EVENT_TRACE_PRIVATE_IN_PROC | EVENT_TRACE_FILE_MODE_SEQUENTIAL;
    properties->LoggerNameOffset=sizeof(EVENT_TRACE_PROPERTIES); properties->LogFileNameOffset=sizeof(EVENT_TRACE_PROPERTIES)+1024*sizeof(wchar_t);
    std::memcpy(storage.data()+properties->LoggerNameOffset,name,(std::wcslen(name)+1)*sizeof(wchar_t));
    std::memcpy(storage.data()+properties->LogFileNameOffset,path,(std::wcslen(path)+1)*sizeof(wchar_t));
    TRACEHANDLE trace=0; status=StartTraceW(&trace,name,properties);
    if (status!=ERROR_SUCCESS) {
        UnregisterTraceGuids(registration); check(DeleteFileW(path)!=FALSE);
        std::printf("POOL_NATIVE_ETL_INTEGRATION=SKIP START_STATUS=%lu\n",static_cast<unsigned long>(status)); return;
    }
    struct Cleanup {
        TRACEHANDLE trace,registration; const wchar_t* name; EVENT_TRACE_PROPERTIES* properties; const wchar_t* path;
        ~Cleanup() { if (trace) { ControlTraceW(trace,name,properties,EVENT_TRACE_CONTROL_STOP); } UnregisterTraceGuids(registration); DeleteFileW(path); }
    } cleanup{trace,registration,name,properties,path};
    status=EnableTrace(TRUE,0,TRACE_LEVEL_VERBOSE,&provider,trace);
    if (status!=ERROR_SUCCESS || !state.logger || state.logger==INVALID_PROCESSTRACE_HANDLE) {
        std::printf("POOL_NATIVE_ETL_INTEGRATION=SKIP ENABLE_STATUS=%lu\n",static_cast<unsigned long>(status)); return;
    }
    const auto emit=[&](const GUID& guid,unsigned opcode,std::uint64_t timestamp,const std::vector<unsigned char>& data) {
        std::vector<unsigned char> packet(sizeof(EVENT_TRACE_HEADER)+data.size());
        auto* header=reinterpret_cast<EVENT_TRACE_HEADER*>(packet.data()); header->Size=static_cast<USHORT>(packet.size());
        header->Guid=guid; header->Class.Type=static_cast<UCHAR>(opcode); header->Class.Version=2;
        header->Flags=WNODE_FLAG_TRACED_GUID | WNODE_FLAG_USE_TIMESTAMP;
        header->ThreadId=GetCurrentThreadId(); header->ProcessId=GetCurrentProcessId(); header->TimeStamp.QuadPart=static_cast<LONGLONG>(timestamp);
        std::memcpy(packet.data()+sizeof(EVENT_TRACE_HEADER),data.data(),data.size()); check(TraceEvent(state.logger,header)==ERROR_SUCCESS);
    };
    const auto poolPayload=[](std::uint64_t address,std::uint64_t size,std::uint32_t session=UINT32_MAX) {
        std::vector<unsigned char> data(24+(session!=UINT32_MAX ? 4 : 0)); put(data,4,0x74736554,4); put(data,8,size,8); put(data,16,address,8);
        if (session!=UINT32_MAX) { put(data,24,session,4); } return data;
    };
    const auto stackPayload=[](std::uint64_t stamp) {
        std::vector<unsigned char> data(24); put(data,0,stamp,8); put(data,8,GetCurrentProcessId(),4); put(data,12,GetCurrentThreadId(),4); put(data,16,0x12345000,8); return data;
    };
    LARGE_INTEGER qpc{}; check(QueryPerformanceCounter(&qpc)!=FALSE); const auto start=static_cast<std::uint64_t>(qpc.QuadPart);
    emit(poolGuid,32,start+10,poolPayload(0x1000,64)); emit(stackGuid,32,start+11,stackPayload(start+10));
    emit(poolGuid,33,start+20,poolPayload(0x1000,128,7)); emit(stackGuid,32,start+21,stackPayload(start+20));
    emit(poolGuid,33,start+30,poolPayload(0x1000,256,8)); emit(poolGuid,34,start+40,poolPayload(0x1000,64));
    emit(poolGuid,35,start+50,poolPayload(0x1000,128,7)); emit(poolGuid,32,start+60,poolPayload(0x2000,512));
    check(ControlTraceW(trace,name,properties,EVENT_TRACE_CONTROL_STOP)==ERROR_SUCCESS); cleanup.trace=0;
    std::atomic_bool cancel{false}; const auto result=ReadPoolAllocationTrace(path,cancel);
    if (!result.completed) { std::printf("Native ETL reader status=%lu accepted=%llu\n",static_cast<unsigned long>(result.status),static_cast<unsigned long long>(result.analysis.stats.eventsAccepted)); }
    check(result.completed && result.status==ERROR_SUCCESS); check(result.lossCountsKnown && result.eventsLost==0 && result.buffersLost==0 && result.lossMarkers==0);
    check(result.analysis.stats.eventsAccepted==6 && result.analysis.groups.size()==4); check(result.missingStackEvents==2);
    std::uint64_t allocations=0,freed=0,outstanding=0;
    for (const auto& group:result.analysis.groups) { allocations+=group.allocatedBytes; freed+=group.pairedFreedBytes; outstanding+=group.outstandingBytes; }
    check(allocations==960 && freed==192 && outstanding==768);
    check(result.analysis.gapReasons==static_cast<std::uint32_t>(GapReason::MissingStack));
    check(result.analysis.lifetimePairingAvailable && !result.analysis.stackCoverageComplete);
    std::puts("POOL_NATIVE_ETL_INTEGRATION=PASS PRIVATE_IN_PROC_SYNTHETIC_PROVIDER");
}
}
int main() {
    try {
    for (unsigned p : {4u,8u}) for (unsigned op : {32u,33u,34u,35u}) {
        const bool session=op==33 || op==35;
        std::vector<unsigned char> b(8+2*p+(session ? 4 : 0));
        put(b,0,512,4); put(b,4,0x74736554,4); put(b,8,p==4 ? 64 : 0x100000040ULL,p);
        put(b,8+p,p==4 ? 0x81234560 : 0xffffc00112345670ULL,p); if (session) { put(b,8+2*p,7,4); }
        auto r=record(poolGuid,op,2,p,b); Event e;
        check(decodePool(r,e)); check(e.poolType==512 && e.tag==0x74736554);
        check(e.kind==(op<=33 ? EventKind::Allocate : EventKind::Free));
        check(e.size==(p==4 ? 64 : 0x100000040ULL)); check(e.sessionId==(session ? 7u : UINT32_MAX));
        check(e.timestamp==12345 && e.pid==123 && e.tid==456); check(poolSchemaMatches(r));
        --r.UserDataLength; check(!decodePool(r,e)); ++r.UserDataLength;
        ++r.UserDataLength; check(!decodePool(r,e)); --r.UserDataLength;
        r.EventHeader.EventDescriptor.Version=3; check(!decodePool(r,e)); r.EventHeader.EventDescriptor.Version=2;
        r.EventHeader.Flags=EVENT_HEADER_FLAG_CLASSIC_HEADER; check(!decodePool(r,e));
        r.EventHeader.Flags=EVENT_HEADER_FLAG_CLASSIC_HEADER | EVENT_HEADER_FLAG_32_BIT_HEADER | EVENT_HEADER_FLAG_64_BIT_HEADER;
        check(!decodePool(r,e)); r=record(poolGuid,op,2,p,b);
        r.UserData=nullptr; check(!decodePool(r,e)); r.UserData=b.data();
        r.EventHeader.TimeStamp.QuadPart=-1; check(!decodePool(r,e));
    }
    for (unsigned p : {4u,8u}) {
        std::vector<unsigned char> b(16+3*p); put(b,0,98765,8); put(b,8,42,4); put(b,12,9,4);
        put(b,16,0x12345000,p); put(b,16+p,0x12346000,p); put(b,16+2*p,0x12347000,p);
        auto r=record(stackGuid,32,2,p,b); StackKey key; std::vector<std::uint64_t> frames;
        check(decodeStack(r,key,frames,192)); check(key==StackKey(98765,9)); check(frames.size()==3 && frames[1]==0x12346000);
        check(!decodeStack(r,key,frames,2)); --r.UserDataLength; check(!decodeStack(r,key,frames,192));
        r=record(stackGuid,32,3,p,b); check(!decodeStack(r,key,frames,192));
    }
    for (unsigned p : {4u,8u}) {
        std::vector<unsigned char> b(8+2*p); put(b,0,10,8); put(b,8,0x1010,p); put(b,8+p,0x2020,p);
        EVENT_RECORD r{}; EVENT_HEADER_EXTENDED_DATA_ITEM ext{};
        ext.ExtType=p==4 ? EVENT_HEADER_EXT_TYPE_STACK_TRACE32 : EVENT_HEADER_EXT_TYPE_STACK_TRACE64;
        ext.DataPtr=reinterpret_cast<ULONG_PTR>(b.data()); ext.DataSize=static_cast<USHORT>(b.size());
        r.ExtendedData=&ext; r.ExtendedDataCount=1; std::vector<std::uint64_t> frames;
        check(extendedStack(r,frames,192)==ExtendedStack::Valid); check(frames.size()==2 && frames[1]==0x2020);
        check(extendedStack(r,frames,1)==ExtendedStack::Invalid); ext.DataSize=9; check(extendedStack(r,frames,192)==ExtendedStack::Invalid);
        r.ExtendedDataCount=0; check(extendedStack(r,frames,192)==ExtendedStack::Missing);
    }
    for (unsigned p : {4u,8u}) for (unsigned v : {1u,2u,3u}) {
        const auto off=v==1 ? 12+2*(p-4) : 44+3*(p-4); const std::wstring path=L"\\SystemRoot\\system32\\driver.sys";
        std::vector<unsigned char> b(off+(path.size()+1)*2); put(b,0,p==4 ? 0x80000000 : 0xfffff80000000000ULL,p);
        put(b,p,0x4000,p); put(b,2*p,0,4); std::memcpy(b.data()+off,path.c_str(),(path.size()+1)*2);
        auto r=record(imageGuid,3,v,p,b); TraceImage image;
        check(decodeImage(r,image)); check(image.pid==0 && image.path==path && image.size==0x4000);
        b.resize(b.size()-2); r.UserData=b.data(); r.UserDataLength=static_cast<USHORT>(b.size()); check(!decodeImage(r,image));
    }
    const auto poolData=[] {
        std::vector<unsigned char> b(24); put(b,0,0,4); put(b,4,0x74736554,4); put(b,8,64,8); put(b,16,0xffffc00012345000ULL,8); return b;
    };
    // Stack PID and record-header timestamp are deliberately different from
    // the Pool context; only the embedded raw QPC + TID may match.
    {
        TraceReadResult result; TraceReadOptions options; std::atomic_bool cancel{false}; Analyzer analyzer;
        Replay replay(result,options,cancel,analyzer); auto b=poolData(); auto pool=record(poolGuid,32,2,8,b);
        std::vector<unsigned char> s(24); put(s,0,12345,8); put(s,8,42,4); put(s,12,456,4); put(s,16,0x12345000,8);
        auto stack=record(stackGuid,32,2,8,s); stack.EventHeader.TimeStamp.QuadPart=99999;
        replay.event(pool); replay.event(stack); replay.first=false; replay.count=0; replay.event(pool);
        const auto& analysis=analyzer.Finish(); check(result.missingStackEvents==0); check(analysis.groups.size()==1);
        check(analysis.groups[0].stack==std::vector<std::uint64_t>{0x12345000});
    }
    {
        TraceReadResult result; TraceReadOptions options; std::atomic_bool cancel{false}; Analyzer analyzer;
        Replay replay(result,options,cancel,analyzer); auto b=poolData(); auto pool=record(poolGuid,32,2,8,b);
        std::vector<unsigned char> s(24); put(s,0,12345,8); put(s,8,42,4); put(s,12,456,4); put(s,16,0x12345000,8);
        auto stack=record(stackGuid,32,2,8,s); replay.event(pool); pool.EventHeader.ProcessId=999;
        pool.EventHeader.EventDescriptor.Opcode=34; replay.event(pool); replay.event(stack);
        replay.first=false; replay.count=0; pool.EventHeader.EventDescriptor.Opcode=32; replay.event(pool);
        check(result.ambiguousStackEvents==1 && result.missingStackEvents==1); check(analyzer.Finish().groups[0].stack.empty());
    }
    const auto imageData=[](const std::wstring& path) {
        std::vector<unsigned char> b(56+(path.size()+1)*2); put(b,0,0xfffff80010000000ULL,8); put(b,8,0x4000,8);
        put(b,16,0,4); put(b,20,0xabcd,4); put(b,24,0x12345678,4); std::memcpy(b.data()+56,path.c_str(),(path.size()+1)*2); return b;
    };
    {
        TraceReadResult result; TraceReadOptions options; std::atomic_bool cancel{false}; Analyzer analyzer;
        Replay replay(result,options,cancel,analyzer); auto b=imageData(L"A.sys"); auto a=record(imageGuid,10,3,8,b);
        a.EventHeader.TimeStamp.QuadPart=100; replay.event(a); a.EventHeader.TimeStamp.QuadPart=200; a.EventHeader.EventDescriptor.Opcode=2; replay.event(a);
        auto c=imageData(L"B.sys"); auto other=record(imageGuid,10,3,8,c); other.EventHeader.TimeStamp.QuadPart=200; replay.event(other); replay.indexImages();
        Event event; event.stack={0xfffff80010001000ULL}; event.timestamp=150; replay.frameImages(event); check(event.frameImageIds[0]==1);
        event.timestamp=250; event.frameImageIds.clear(); replay.frameImages(event); check(event.frameImageIds[0]==2);
        check(result.images[0].lastTimestamp==200 && result.images[1].timeDateStamp==0x12345678 && result.images[1].imageIdentityKnown);
    }
    for (bool capacity : {false,true}) {
        TraceReadResult result; TraceReadOptions options; if (capacity) { options.maxImageRecords=1; }
        std::atomic_bool cancel{false}; Analyzer analyzer; Replay replay(result,options,cancel,analyzer);
        auto b=imageData(L"B.sys"); auto image=record(imageGuid,10,3,8,b); image.EventHeader.TimeStamp.QuadPart=200; replay.event(image);
        auto a=imageData(L"A.sys"); image=record(imageGuid,10,3,8,a); image.EventHeader.TimeStamp.QuadPart=capacity ? 300 : 100; replay.event(image);
        replay.indexImages(); Event event; event.stack={0xfffff80010001000ULL}; event.timestamp=350; replay.frameImages(event);
        check(result.imageHistoryTruncated); check(event.frameImageIds==std::vector<std::uint64_t>{0}); check(event.stack[0]==0xfffff80010001000ULL);
    }
    {
        TraceReadResult result; TraceReadOptions options; std::atomic_bool cancel{false}; Analyzer analyzer;
        TraceImage old; old.id=1; old.base=0x180000000ULL; old.size=0x4000; old.pid=123; old.lastTimestamp=forever; old.path=L"old-user.dll";
        result.images.push_back(old); Replay replay(result,options,cancel,analyzer); replay.indexImages();
        Event reused; reused.pid=123; reused.timestamp=500; reused.stack={0x180001000ULL}; replay.frameImages(reused);
        check(reused.frameImageIds==std::vector<std::uint64_t>{0}); check(reused.stack[0]==0x180001000ULL);
    }
    {
        TraceReadResult result; TraceReadOptions options; options.timeoutMs=0; std::atomic_bool cancel{false}; Analyzer analyzer;
        Replay replay(result,options,cancel,analyzer); check(replay.shouldStop()); check(result.timedOut && result.status==ERROR_TIMEOUT);
    }
    {
        TraceReadResult result; TraceReadOptions options; std::atomic_bool cancel{false}; Analyzer analyzer; Replay replay(result,options,cancel,analyzer);
        std::vector<unsigned char> b; auto loss=record(lostGuid,32,2,8,b); replay.event(loss);
        check(result.lossMarkers==1 && result.eventsLost==0 && result.buffersLost==0 && result.imageHistoryTruncated);
        replay.first=false; auto unknown=record(poolGuid,36,2,8,b); replay.event(unknown);
        check(result.unsupportedPoolEvents==1 && !analyzer.Finish().coverageComplete);
    }
    check(localImagePath(L"\\\\server\\driver.sys").empty()); check(localImagePath(L"\\\\?\\UNC\\server\\driver.sys").empty());
    check(localImagePath(L"C:\\a:stream").empty()); check(localImagePath(L"srv*https://server/x").empty());
    {
        // A local PE with a remote build-machine CV path must use only the
        // basename in the checked local image directory, never that CV path.
        wchar_t tmp[MAX_PATH]{},path[MAX_PATH]{}; check(GetTempPathW(MAX_PATH,tmp)!=0); check(GetTempFileNameW(tmp,L"KPR",0,path)!=0);
        std::vector<unsigned char> b(1024); IMAGE_DOS_HEADER dos{}; dos.e_magic=IMAGE_DOS_SIGNATURE; dos.e_lfanew=64; std::memcpy(b.data(),&dos,sizeof(dos));
        put(b,64,IMAGE_NT_SIGNATURE,4); IMAGE_FILE_HEADER file{}; file.NumberOfSections=1; file.TimeDateStamp=0x12345678; file.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);
        std::memcpy(b.data()+68,&file,sizeof(file)); IMAGE_OPTIONAL_HEADER64 optional{}; optional.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage=0x4000; optional.CheckSum=0xabcd; optional.NumberOfRvaAndSizes=16;
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]={0x1000,sizeof(IMAGE_DEBUG_DIRECTORY)};
        std::memcpy(b.data()+68+sizeof(file),&optional,sizeof(optional)); IMAGE_SECTION_HEADER section{}; section.VirtualAddress=0x1000; section.PointerToRawData=512; section.SizeOfRawData=512;
        std::memcpy(b.data()+68+sizeof(file)+sizeof(optional),&section,sizeof(section));
        const char* pdb="\\\\server\\symbols\\ksword-reader-fixture.pdb"; IMAGE_DEBUG_DIRECTORY debug{}; debug.Type=IMAGE_DEBUG_TYPE_CODEVIEW;
        debug.PointerToRawData=576; debug.SizeOfData=static_cast<DWORD>(24+std::strlen(pdb)+1); std::memcpy(b.data()+512,&debug,sizeof(debug));
        std::memcpy(b.data()+576,"RSDS",4); put(b,596,7,4); std::memcpy(b.data()+600,pdb,std::strlen(pdb)+1);
        HANDLE h=CreateFileW(path,GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,0,nullptr); check(h!=INVALID_HANDLE_VALUE);
        DWORD written=0; check(WriteFile(h,b.data(),static_cast<DWORD>(b.size()),&written,nullptr)!=FALSE && written==b.size()); CloseHandle(h);
        TraceImage image; image.path=path; image.size=0x4000; image.timeDateStamp=0x12345678; image.checksum=0xabcd; image.imageIdentityKnown=true;
        LocalPe pe; const bool inspected=inspectLocalPe(image,pe);
        if (!inspected) {
            wchar_t finalPath[32768]{};
            const auto n=pe.file.get() && pe.file.get()!=INVALID_HANDLE_VALUE
                ? GetFinalPathNameByHandleW(pe.file.get(),finalPath,32768,FILE_NAME_NORMALIZED) : 0;
            std::fwprintf(stderr,L"Fixture local PE inspection failed: final=%ls (length=%lu), error=%lu\n",finalPath,static_cast<unsigned long>(n),static_cast<unsigned long>(GetLastError()));
        }
        check(inspected); check(pe.pdb.find(L"server")==std::wstring::npos && pe.pdbAge==7); pe.file.reset();
        image.checksum=1; check(!inspectLocalPe(image,pe)); pe.file.reset(); image.checksum=0xabcd; image.imageIdentityKnown=false;
        check(!inspectLocalPe(image,pe)); pe.file.reset(); check(DeleteFileW(path)!=FALSE);
    }
    std::atomic_bool cancel{true}; auto result=ReadPoolAllocationTrace(L"C:\\path-that-does-not-exist\\pool.etl",cancel);
    check(!result.completed && result.cancelled && !result.analysis.coverageComplete);
    nativeIntegration();
    std::printf("Pool trace reader: %d checks passed\n",checks);
    return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
