#include "PoolTraceReader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <dbghelp.h>
#include "../ksword/dbghelp_serialization.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace ks::evidence::pool {
namespace {
// Installed MSNT_SystemTrace MOF: PoolTrace v2; WmiDataId 1..5.
// Type/Tag are ULONG, NumberOfBytes is SizeT, Entry is a pointer,
// SessionId is an optional ULONG. This is not Kernel-Memory's schema.
constexpr GUID poolGuid{0x0268a8b6,0x74fd,0x4302,{0x9d,0xd0,0x6e,0x8f,0x17,0x95,0xc0,0xcf}};
constexpr GUID stackGuid{0xdef2fe46,0x7bd6,0x4b80,{0xbd,0x94,0xf5,0x7f,0xe2,0x0d,0x0c,0xe3}};
constexpr GUID imageGuid{0x2cb15d1d,0x5fc1,0x11d2,{0xab,0xe1,0x00,0xa0,0xc9,0x11,0xf5,0x18}};
constexpr GUID lostGuid{0x6a399ae0,0x4bc6,0x4de9,{0x87,0x0b,0x36,0x57,0xf8,0x94,0x7e,0x7e}};
constexpr auto forever = (std::numeric_limits<std::uint64_t>::max)();
bool kernelAddress(std::uint64_t address) {
    return (address>=0x80000000ULL && address<=UINT32_MAX) || address>=0xff00000000000000ULL;
}

bool sameGuid(const GUID& a, const GUID& b) { return std::memcmp(&a,&b,sizeof(a)) == 0; }
bool classic(const EVENT_RECORD& r) { return (r.EventHeader.Flags & EVENT_HEADER_FLAG_CLASSIC_HEADER) != 0; }
std::size_t pointerBytes(const EVENT_RECORD& r) {
    const auto width = r.EventHeader.Flags & (EVENT_HEADER_FLAG_32_BIT_HEADER | EVENT_HEADER_FLAG_64_BIT_HEADER);
    return width == EVENT_HEADER_FLAG_32_BIT_HEADER ? 4 : (width == EVENT_HEADER_FLAG_64_BIT_HEADER ? 8 : 0);
}
bool integer(const void* data, std::size_t length, std::size_t offset, std::size_t width, std::uint64_t& value) {
    value = 0;
    if (!data || (width != 4 && width != 8) || offset > length || width > length - offset) { return false; }
    std::memcpy(&value, static_cast<const unsigned char*>(data) + offset, width);
    return true;
}
bool isPool(const EVENT_RECORD& r) {
    const auto op = r.EventHeader.EventDescriptor.Opcode;
    return sameGuid(r.EventHeader.ProviderId,poolGuid) && op >= 32 && op <= 35;
}
bool poolCandidate(const EVENT_RECORD& r) {
    const auto op=r.EventHeader.EventDescriptor.Opcode;
    // Known PoolSnapshot opcodes describe snapshots, not allocation lifetimes.
    return sameGuid(r.EventHeader.ProviderId,poolGuid) && !(op>=40 && op<=47);
}
bool decodePool(const EVENT_RECORD& r, Event& e) {
    if (!isPool(r) || !classic(r) || r.EventHeader.EventDescriptor.Version != 2) { return false; }
    const auto p = pointerBytes(r);
    const auto op = r.EventHeader.EventDescriptor.Opcode;
    const bool session = op == 33 || op == 35;
    if (!p || r.UserDataLength != 8 + 2*p + (session ? 4 : 0)) { return false; }
    std::uint64_t type = 0, tag = 0, size = 0, address = 0, sessionId = UINT32_MAX;
    if (!integer(r.UserData,r.UserDataLength,0,4,type) || !integer(r.UserData,r.UserDataLength,4,4,tag)
        || !integer(r.UserData,r.UserDataLength,8,p,size) || !integer(r.UserData,r.UserDataLength,8+p,p,address)
        || (session && !integer(r.UserData,r.UserDataLength,8+2*p,4,sessionId))) { return false; }
    if (r.EventHeader.TimeStamp.QuadPart < 0 || !address || (op <= 33 && !size)
        || (session && sessionId == UINT32_MAX)) { return false; }
    e.kind = op <= 33 ? EventKind::Allocate : EventKind::Free;
    e.address = address; e.size = size; e.tag = static_cast<std::uint32_t>(tag);
    e.poolType = static_cast<std::uint32_t>(type); e.sessionId = static_cast<std::uint32_t>(sessionId);
    e.timestamp = static_cast<std::uint64_t>(r.EventHeader.TimeStamp.QuadPart);
    e.pid = r.EventHeader.ProcessId; e.tid = r.EventHeader.ThreadId;
    return true;
}

// TDH adds information when available. Only the exact known fixed layout is
// accepted; unavailable MOF metadata uses the documented v2 layout above.
bool poolSchemaMatches(EVENT_RECORD& r) {
    ULONG size = 0;
    auto status = TdhGetEventInformation(&r,0,nullptr,nullptr,&size);
    if (status == ERROR_NOT_FOUND || status == ERROR_WMI_GUID_NOT_FOUND) { return true; }
    if (status != ERROR_INSUFFICIENT_BUFFER || size < sizeof(TRACE_EVENT_INFO) || size > 65536) { return false; }
    std::vector<unsigned char> bytes(size);
    auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(bytes.data());
    if (TdhGetEventInformation(&r,0,nullptr,info,&size) != ERROR_SUCCESS) { return false; }
    const bool session = r.EventHeader.EventDescriptor.Opcode == 33 || r.EventHeader.EventDescriptor.Opcode == 35;
    const ULONG expected = session ? 5 : 4;
    const auto fixed = offsetof(TRACE_EVENT_INFO,EventPropertyInfoArray);
    if (info->PropertyCount != expected || info->TopLevelPropertyCount != expected
        || fixed + expected*sizeof(EVENT_PROPERTY_INFO) > size) { return false; }
    const wchar_t* names[]{L"Type",L"Tag",L"NumberOfBytes",L"Entry",L"SessionId"};
    // Documented TDH_IN_TYPE values (also build with MinGW's older tdh.h).
    const USHORT types[]{8,8,308,16,8};
    for (ULONG i=0;i<expected;++i) {
        const auto& property = info->EventPropertyInfoArray[i];
        if (property.Flags != 0 || property.count != 1 || property.nonStructType.InType != types[i]
            || property.NameOffset >= size || (property.NameOffset & 1) != 0) { return false; }
        const auto maxChars = (size-property.NameOffset)/sizeof(wchar_t);
        const auto* name = reinterpret_cast<const wchar_t*>(bytes.data()+property.NameOffset);
        std::size_t chars=0; while (chars < maxChars && name[chars]) { ++chars; }
        if (chars == maxChars || std::wstring(name,chars) != names[i]) { return false; }
    }
    return true;
}

using StackKey = std::pair<std::uint64_t,std::uint32_t>; // raw QPC, payload TID; PID is recording context
struct StackRecord {
    std::vector<std::uint64_t> frames;
    unsigned poolEvents = 0, stackEvents = 0;
    bool ambiguous = false;
};
bool decodeStack(const EVENT_RECORD& r, StackKey& key, std::vector<std::uint64_t>& frames, std::size_t maxFrames) {
    const auto p = pointerBytes(r);
    if (!sameGuid(r.EventHeader.ProviderId,stackGuid) || !classic(r)
        || r.EventHeader.EventDescriptor.Version != 2 || r.EventHeader.EventDescriptor.Opcode != 32
        || !p || r.UserDataLength < 16+p || (r.UserDataLength-16)%p != 0
        || (r.UserDataLength-16)/p > maxFrames) { return false; }
    std::uint64_t stamp=0,pid=0,tid=0;
    if (!integer(r.UserData,r.UserDataLength,0,8,stamp) || !integer(r.UserData,r.UserDataLength,8,4,pid)
        || !integer(r.UserData,r.UserDataLength,12,4,tid) || stamp > INT64_MAX) { return false; }
    key = {stamp,static_cast<std::uint32_t>(tid)};
    frames.clear(); frames.reserve((r.UserDataLength-16)/p);
    for (std::size_t off=16;off<r.UserDataLength;off+=p) {
        std::uint64_t frame=0; if (!integer(r.UserData,r.UserDataLength,off,p,frame)) { return false; }
        frames.push_back(frame);
    }
    return true;
}
enum class ExtendedStack { Missing, Valid, Invalid };
ExtendedStack extendedStack(const EVENT_RECORD& r, std::vector<std::uint64_t>& frames, std::size_t maxFrames) {
    frames.clear();
    if (!r.ExtendedDataCount) { return ExtendedStack::Missing; }
    if (!r.ExtendedData || r.ExtendedDataCount > 256) { return ExtendedStack::Invalid; }
    bool seen=false;
    for (USHORT i=0;i<r.ExtendedDataCount;++i) {
        const auto& x = r.ExtendedData[i];
        const auto p = x.ExtType == EVENT_HEADER_EXT_TYPE_STACK_TRACE32 ? 4u
            : (x.ExtType == EVENT_HEADER_EXT_TYPE_STACK_TRACE64 ? 8u : 0u);
        if (!p) { continue; }
        // Multiple items can be fragments; no ordering contract is available
        // here, so never concatenate fragments or choose an arbitrary one.
        if (seen || !x.DataPtr || x.DataSize < 8+p || (x.DataSize-8)%p != 0
            || (x.DataSize-8)/p > maxFrames) { frames.clear(); return ExtendedStack::Invalid; }
        seen=true;
        const auto* data = reinterpret_cast<const void*>(static_cast<ULONG_PTR>(x.DataPtr));
        for (std::size_t off=8;off<x.DataSize;off+=p) {
            std::uint64_t frame=0; if (!integer(data,x.DataSize,off,p,frame)) { frames.clear(); return ExtendedStack::Invalid; }
            frames.push_back(frame);
        }
    }
    return seen ? ExtendedStack::Valid : ExtendedStack::Missing;
}

bool decodeImage(const EVENT_RECORD& r, TraceImage& image) {
    const auto v=r.EventHeader.EventDescriptor.Version,op=r.EventHeader.EventDescriptor.Opcode;
    const auto p=pointerBytes(r);
    if (!sameGuid(r.EventHeader.ProviderId,imageGuid) || !classic(r) || !p || v < 1 || v > 3
        || (op != 10 && op != 2 && op != 3 && op != 4) || r.EventHeader.TimeStamp.QuadPart < 0) { return false; }
    const std::size_t nameOffset = v==1 ? 12+2*(p-4) : 44+3*(p-4);
    std::uint64_t pid=0;
    if (!integer(r.UserData,r.UserDataLength,0,p,image.base) || !integer(r.UserData,r.UserDataLength,p,p,image.size)
        || !integer(r.UserData,r.UserDataLength,2*p,4,pid) || !image.size || image.base > forever-image.size
        || nameOffset >= r.UserDataLength || (r.UserDataLength-nameOffset)%2 != 0) { return false; }
    image.pid=static_cast<std::uint32_t>(pid);
    if (v>=2) {
        std::uint64_t checksum=0,stamp=0;
        if (!integer(r.UserData,r.UserDataLength,2*p+4,4,checksum) || !integer(r.UserData,r.UserDataLength,2*p+8,4,stamp)) { return false; }
        image.checksum=static_cast<std::uint32_t>(checksum); image.timeDateStamp=static_cast<std::uint32_t>(stamp);
        image.imageIdentityKnown=true;
    }
    const auto* bytes = static_cast<const unsigned char*>(r.UserData);
    image.path.clear();
    bool terminated=false;
    for (auto offset=nameOffset;offset+2<=r.UserDataLength;offset+=2) {
        wchar_t c=0; std::memcpy(&c,bytes+offset,2);
        if (!c) { terminated=true; break; }
        image.path.push_back(c);
    }
    if (!terminated || image.path.empty()) { return false; }
    image.firstTimestamp=static_cast<std::uint64_t>(r.EventHeader.TimeStamp.QuadPart);
    image.lastTimestamp=forever;
    return true;
}
struct FileCloser { void operator()(void* h) const { if (h && h!=INVALID_HANDLE_VALUE) { CloseHandle(h); } } };
using FileHandle=std::unique_ptr<void,FileCloser>;
bool fileInfo(HANDLE h, BY_HANDLE_FILE_INFORMATION& info) {
    return GetFileInformationByHandle(h,&info) != FALSE && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}
bool sameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh
        && a.nFileIndexLow==b.nFileIndexLow && a.nFileSizeHigh==b.nFileSizeHigh && a.nFileSizeLow==b.nFileSizeLow
        && a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime
        && a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;
}

std::wstring localImagePath(std::wstring path) {
    if (path.size()>32760) { return {}; }
    if (path.compare(0,12,L"\\SystemRoot\\")==0) {
        wchar_t windows[MAX_PATH]{}; const auto n=GetWindowsDirectoryW(windows,MAX_PATH);
        if (!n || n>=MAX_PATH) { return {}; }
        path=std::wstring(windows,n)+path.substr(11);
    } else if (path.compare(0,8,L"\\Device\\")==0) {
        bool found=false;
        for (wchar_t letter=L'A';letter<=L'Z';++letter) {
            const wchar_t drive[]{letter,L':',0}; wchar_t device[1024]{};
            if (!QueryDosDeviceW(drive,device,1024)) { continue; }
            const std::wstring target(device);
            if (path.size()>target.size() && path.compare(0,target.size(),target)==0 && path[target.size()]==L'\\') {
                path=std::wstring(drive)+path.substr(target.size()); found=true; break;
            }
        }
        if (!found) { return {}; }
    } else if (path.compare(0,4,L"\\??\\")==0 || path.compare(0,4,L"\\\\?\\")==0) { path.erase(0,4); }
    if (path.size()<3 || path[1]!=L':' || path[2]!=L'\\'
        || !((path[0]>=L'A' && path[0]<=L'Z') || (path[0]>=L'a' && path[0]<=L'z'))
        || path.find(L':',2)!=std::wstring::npos || path.find_first_of(L"*?\"|<>")!=std::wstring::npos) { return {}; }
    wchar_t root[]{path[0],L':',L'\\',0};
    if (GetDriveTypeW(root)!=DRIVE_FIXED) { return {}; }
    std::vector<wchar_t> full(32768);
    const auto n=GetFullPathNameW(path.c_str(),static_cast<DWORD>(full.size()),full.data(),nullptr);
    return n && n<full.size() ? std::wstring(full.data(),n) : std::wstring{};
}
struct ViewCloser { void operator()(const void* p) const { if (p) { UnmapViewOfFile(p); } } };
struct LocalPe {
    std::wstring path,pdb;
    GUID pdbGuid{};
    DWORD pdbAge=0;
    FileHandle file;
};
bool range(std::size_t length,std::size_t offset,std::size_t count) { return offset<=length && count<=length-offset; }
bool noReparsePath(const std::wstring& path) {
    std::size_t components=0;
    for (std::size_t off=3;off<=path.size();++off) {
        if (off!=path.size() && path[off]!=L'\\') { continue; }
        if (++components>256) { return false; }
        const auto attributes=GetFileAttributesW(path.substr(0,off).c_str());
        if (attributes==INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)!=0) { return false; }
    }
    return true;
}
bool localFileHandle(HANDLE file) {
    std::vector<wchar_t> path(32768);
    const auto n=GetFinalPathNameByHandleW(file,path.data(),static_cast<DWORD>(path.size()),FILE_NAME_NORMALIZED);
    return n && n<path.size() && !localImagePath(std::wstring(path.data(),n)).empty();
}
bool inspectLocalPe(const TraceImage& image, LocalPe& pe) {
    if (!image.imageIdentityKnown) { return false; }
    pe.path=localImagePath(image.path); if (pe.path.empty() || !noReparsePath(pe.path)) { return false; }
    pe.file.reset(CreateFileW(pe.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (pe.file.get()==INVALID_HANDLE_VALUE || !localFileHandle(pe.file.get()) || !fileInfo(pe.file.get(),info)
        || info.nFileSizeHigh || info.nFileSizeLow>512*1024*1024) { return false; }
    const auto length=static_cast<std::size_t>(info.nFileSizeLow);
    FileHandle mapping(CreateFileMappingW(pe.file.get(),nullptr,PAGE_READONLY,0,0,nullptr));
    if (!mapping) { return false; }
    std::unique_ptr<const void,ViewCloser> view(MapViewOfFile(mapping.get(),FILE_MAP_READ,0,0,0));
    if (!view || !range(length,0,sizeof(IMAGE_DOS_HEADER))) { return false; }
    const auto* bytes=static_cast<const unsigned char*>(view.get());
    IMAGE_DOS_HEADER dos{}; std::memcpy(&dos,bytes,sizeof(dos));
    if (dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>1024*1024) { return false; }
    const auto nt=static_cast<std::size_t>(dos.e_lfanew);
    DWORD signature=0; if (!range(length,nt,4+sizeof(IMAGE_FILE_HEADER))) { return false; }
    std::memcpy(&signature,bytes+nt,4); if (signature!=IMAGE_NT_SIGNATURE) { return false; }
    IMAGE_FILE_HEADER fh{}; std::memcpy(&fh,bytes+nt+4,sizeof(fh));
    const auto optional=nt+4+sizeof(fh);
    if (fh.TimeDateStamp!=image.timeDateStamp || fh.NumberOfSections>96 || !range(length,optional,fh.SizeOfOptionalHeader)
        || fh.SizeOfOptionalHeader<sizeof(WORD)) { return false; }
    WORD magic=0; std::memcpy(&magic,bytes+optional,2);
    IMAGE_DATA_DIRECTORY debug{};
    if (magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC && fh.SizeOfOptionalHeader>=sizeof(IMAGE_OPTIONAL_HEADER64)) {
        IMAGE_OPTIONAL_HEADER64 oh{}; std::memcpy(&oh,bytes+optional,sizeof(oh));
        if (oh.SizeOfImage!=image.size || oh.CheckSum!=image.checksum || oh.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_DEBUG) { return false; }
        debug=oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    } else if (magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC && fh.SizeOfOptionalHeader>=sizeof(IMAGE_OPTIONAL_HEADER32)) {
        IMAGE_OPTIONAL_HEADER32 oh{}; std::memcpy(&oh,bytes+optional,sizeof(oh));
        if (oh.SizeOfImage!=image.size || oh.CheckSum!=image.checksum || oh.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_DEBUG) { return false; }
        debug=oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    } else { return false; }
    const auto sectionOffset=optional+fh.SizeOfOptionalHeader;
    if (!range(length,sectionOffset,fh.NumberOfSections*sizeof(IMAGE_SECTION_HEADER))
        || !debug.Size || debug.Size>256*sizeof(IMAGE_DEBUG_DIRECTORY) || debug.Size%sizeof(IMAGE_DEBUG_DIRECTORY)!=0) { return false; }
    std::size_t debugOffset=length;
    for (unsigned i=0;i<fh.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER section{}; std::memcpy(&section,bytes+sectionOffset+i*sizeof(section),sizeof(section));
        if (debug.VirtualAddress>=section.VirtualAddress && debug.VirtualAddress-section.VirtualAddress<section.SizeOfRawData) {
            const auto relative=debug.VirtualAddress-section.VirtualAddress;
            if (debug.Size>section.SizeOfRawData-relative) { return false; }
            debugOffset=static_cast<std::size_t>(section.PointerToRawData)+relative; break;
        }
    }
    if (!range(length,debugOffset,debug.Size)) { return false; }
    for (std::size_t off=0;off<debug.Size;off+=sizeof(IMAGE_DEBUG_DIRECTORY)) {
        IMAGE_DEBUG_DIRECTORY entry{}; std::memcpy(&entry,bytes+debugOffset+off,sizeof(entry));
        if (entry.Type!=IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData<25 || entry.SizeOfData>4096
            || !range(length,entry.PointerToRawData,entry.SizeOfData)) { continue; }
        const auto* cv=bytes+entry.PointerToRawData;
        if (std::memcmp(cv,"RSDS",4)!=0) { continue; }
        const auto* name=reinterpret_cast<const char*>(cv+24);
        std::size_t n=0; while (n<entry.SizeOfData-24 && name[n]) { ++n; }
        if (!n || n==entry.SizeOfData-24) { return false; }
        std::string pdb(name,n); const auto slash=pdb.find_last_of("/\\"); if (slash!=std::string::npos) { pdb.erase(0,slash+1); }
        if (pdb.size()<5 || pdb.size()>240 || pdb.find_first_of(":*?\"|<>")!=std::string::npos
            || _stricmp(pdb.c_str()+pdb.size()-4,".pdb")!=0) { return false; }
        std::wstring basename;
        for (const unsigned char c:pdb) { if (c<32 || c>126) { return false; } basename.push_back(static_cast<wchar_t>(c)); }
        pe.pdb=pe.path.substr(0,pe.path.find_last_of(L'\\')+1)+basename;
        std::memcpy(&pe.pdbGuid,cv+4,sizeof(GUID)); std::memcpy(&pe.pdbAge,cv+20,4);
        return pe.pdbAge!=0;
    }
    return false;
}

struct Replay {
    TraceReadResult& result;
    const TraceReadOptions& options;
    const std::atomic_bool& cancel;
    Analyzer& analyzer;
    std::chrono::steady_clock::time_point deadline;
    std::map<StackKey,StackRecord> stacks;
    std::map<unsigned,bool> schemas;
    struct ImageIndex { std::vector<std::size_t> entries; std::vector<std::uint64_t> maxEnd; };
    std::map<std::uint32_t,ImageIndex> imageIndex;
    std::size_t storedFrames=0;
    std::uint64_t count=0,lastImageTimestamp=0,imageChecks=0;
    bool first=true,stop=false,stackTruncated=false;

    Replay(TraceReadResult& r,const TraceReadOptions& o,const std::atomic_bool& c,Analyzer& a)
        : result(r),options(o),cancel(c),analyzer(a),deadline(std::chrono::steady_clock::now()+std::chrono::milliseconds(o.timeoutMs)) {}

    bool shouldStop() {
        if (stop) { return true; }
        if (cancel.load(std::memory_order_relaxed)) {
            result.cancelled=true; result.status=ERROR_CANCELLED; stop=true; analyzer.Cancel();
        } else if (std::chrono::steady_clock::now() >= deadline) {
            result.timedOut=true; result.status=ERROR_TIMEOUT; stop=true; analyzer.NoteGap(GapReason::EventLimit);
        }
        return stop;
    }
    StackRecord* stackRecord(const StackKey& key) {
        auto found=stacks.find(key); if (found!=stacks.end()) { return &found->second; }
        if (stacks.size()>=options.maxStackRecords) { stackTruncated=true; return nullptr; }
        return &stacks.emplace(key,StackRecord{}).first->second;
    }
    void addImage(const EVENT_RECORD& r) {
        TraceImage image;
        if (!decodeImage(r,image)) { result.imageHistoryTruncated=true; return; }
        const auto local=localImagePath(image.path); if (!local.empty()) { image.path=local; }
        if (image.firstTimestamp<lastImageTimestamp) { result.imageHistoryTruncated=true; return; }
        lastImageTimestamp=image.firstTimestamp;
        const auto op=r.EventHeader.EventDescriptor.Opcode;
        auto existing=result.images.rend();
        for (auto it=result.images.rbegin();it!=result.images.rend();++it) {
            if (it->pid==image.pid && it->base==image.base && it->lastTimestamp==forever) { existing=it; break; }
        }
        if (op==2) {
            if (existing!=result.images.rend() && existing->path==image.path && existing->size==image.size
                && existing->timeDateStamp==image.timeDateStamp && existing->checksum==image.checksum) {
                existing->lastTimestamp=image.firstTimestamp;
            } else { result.imageHistoryTruncated=true; }
            return;
        }
        if (op==3 || op==4) {
            if (existing!=result.images.rend() && existing->path==image.path && existing->size==image.size
                && existing->timeDateStamp==image.timeDateStamp && existing->checksum==image.checksum) { return; }
            // DCStart describes images present at trace start. DCEnd describes
            // images present at trace end; never backdate an end-only image.
            if (op==3) { image.firstTimestamp=0; }
        }
        if (existing!=result.images.rend()) {
            // A new image without the old image's unload is a lifecycle gap.
            // A late DCStart must not retroactively replace a known lifetime.
            result.imageHistoryTruncated=true; existing->lastTimestamp=image.firstTimestamp;
        }
        if (result.images.size()>=options.maxImageRecords) { result.imageHistoryTruncated=true; return; }
        image.id=result.images.size()+1; result.images.push_back(std::move(image));
    }
    void indexImages() {
        for (std::size_t i=0;i<result.images.size();++i) {
            // No process-epoch evidence is collected. A reused user PID must
            // not inherit an old user image that lacked an unload event.
            if (result.images[i].pid==0 && kernelAddress(result.images[i].base)) { imageIndex[0].entries.push_back(i); }
        }
        for (auto& pair:imageIndex) {
            auto& index=pair.second;
            std::sort(index.entries.begin(),index.entries.end(),[this](std::size_t a,std::size_t b) { return result.images[a].base<result.images[b].base; });
            std::uint64_t end=0;
            for (auto i:index.entries) { end=(std::max)(end,result.images[i].base+result.images[i].size); index.maxEnd.push_back(end); }
        }
    }
    bool findImage(std::uint32_t pid,std::uint64_t frame,std::uint64_t stamp,std::uint64_t& id) {
        auto found=imageIndex.find(pid); if (found==imageIndex.end()) { return true; }
        const auto& index=found->second;
        auto pos=std::upper_bound(index.entries.begin(),index.entries.end(),frame,[this](std::uint64_t address,std::size_t i) { return address<result.images[i].base; });
        auto n=static_cast<std::size_t>(pos-index.entries.begin());
        while (n && index.maxEnd[n-1]>frame) {
            if (++imageChecks>8000000) { result.imageHistoryTruncated=true; return false; }
            const auto& image=result.images[index.entries[--n]];
            if (image.firstTimestamp<=stamp && stamp<image.lastTimestamp && frame>=image.base && frame-image.base<image.size) {
                if (id) { id=0; return false; }
                id=image.id;
            }
        }
        return true;
    }
    void frameImages(Event& e) {
        if (result.imageHistoryTruncated) { e.frameImageIds.assign(e.stack.size(),0); return; }
        e.frameImageIds.reserve(e.stack.size());
        for (const auto frame:e.stack) {
            std::uint64_t id=0;
            if (!kernelAddress(frame) || !findImage(0,frame,e.timestamp,id)) { id=0; }
            e.frameImageIds.push_back(id);
        }
    }
    void event(EVENT_RECORD& r) {
        if (shouldStop()) { return; }
        if (++count > options.maxTraceEvents) {
            stop=true; result.status=ERROR_MORE_DATA; analyzer.NoteGap(GapReason::EventLimit); return;
        }
        if (sameGuid(r.EventHeader.ProviderId,lostGuid)) {
            if (first) { ++result.lossMarkers; result.imageHistoryTruncated=true; }
            // Markers do not contain an event count. The first pass already
            // disables pairing for the unknown loss position globally.
            return;
        }
        if (first && sameGuid(r.EventHeader.ProviderId,imageGuid)) { addImage(r); return; }
        if (first && sameGuid(r.EventHeader.ProviderId,stackGuid)) {
            StackKey key; std::vector<std::uint64_t> frames;
            if (!decodeStack(r,key,frames,options.analysisLimits.maxStackFrames)) { stackTruncated=true; return; }
            auto* record=stackRecord(key); if (!record) { return; }
            ++record->stackEvents;
            if (record->stackEvents!=1) { record->ambiguous=true; return; }
            if (frames.size()>options.analysisLimits.maxStoredStackFrames-storedFrames) { stackTruncated=true; return; }
            storedFrames+=frames.size(); record->frames=std::move(frames); return;
        }
        if (!poolCandidate(r)) { return; }
        Event e;
        bool valid=decodePool(r,e);
        if (valid) {
            const auto schemaKey=static_cast<unsigned>(r.EventHeader.EventDescriptor.Opcode)*16u+static_cast<unsigned>(pointerBytes(r));
            auto found=schemas.find(schemaKey);
            if (found==schemas.end()) { found=schemas.emplace(schemaKey,poolSchemaMatches(r)).first; }
            valid=found->second;
        }
        if (first) {
            // Count even rejected Pool records: a second event at the same
            // timestamp/thread must never steal the first event's stack.
            if (r.EventHeader.TimeStamp.QuadPart>=0) {
                auto* record=stackRecord({static_cast<std::uint64_t>(r.EventHeader.TimeStamp.QuadPart),r.EventHeader.ThreadId});
                if (record && ++record->poolEvents!=1) { record->ambiguous=true; }
            }
            return;
        }
        if (!valid) { ++result.unsupportedPoolEvents; analyzer.NoteGap(GapReason::DecodeFailure); return; }
        e.sequence=count;
        if (e.kind==EventKind::Allocate) {
            const auto ext=extendedStack(r,e.stack,options.analysisLimits.maxStackFrames);
            if (ext==ExtendedStack::Missing) {
                const auto found=stacks.find({e.timestamp,e.tid});
                if (found!=stacks.end() && found->second.ambiguous) { ++result.ambiguousStackEvents; }
                else if (found!=stacks.end() && found->second.poolEvents==1 && found->second.stackEvents==1) { e.stack=found->second.frames; }
            } else if (ext==ExtendedStack::Invalid) { ++result.ambiguousStackEvents; }
            if (e.stack.empty()) { ++result.missingStackEvents; }
            frameImages(e);
        }
        analyzer.Feed(e);
    }
};
void WINAPI eventCallback(EVENT_RECORD* r) noexcept {
    if (!r || !r->UserContext) { return; }
    auto& replay=*static_cast<Replay*>(r->UserContext);
    try { replay.event(*r); }
    catch (...) {
        replay.stop=true; replay.result.status=ERROR_NOT_ENOUGH_MEMORY;
        // No exception, including an allocator exception, crosses ETW's ABI.
        try { replay.analyzer.NoteGap(GapReason::DecodeFailure); } catch (...) {}
    }
}
ULONG WINAPI bufferCallback(EVENT_TRACE_LOGFILEW* log) noexcept {
    if (!log || !log->Context) { return FALSE; }
    auto& replay=*static_cast<Replay*>(log->Context);
    try { return replay.shouldStop() ? FALSE : TRUE; }
    catch (...) { replay.stop=true; replay.result.status=ERROR_NOT_ENOUGH_MEMORY; return FALSE; }
}
ULONG pass(const std::wstring& path, Replay& replay) {
    EVENT_TRACE_LOGFILEW log{};
    log.LogFileName=const_cast<LPWSTR>(path.c_str());
    log.ProcessTraceMode=PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback=eventCallback; log.BufferCallback=bufferCallback; log.Context=&replay;
    const auto handle=OpenTraceW(&log);
    if (handle==INVALID_PROCESSTRACE_HANDLE) { return GetLastError(); }
    // StackWalk payload timestamps use QPC. CPU-cycle/system-time logs cannot
    // safely be joined with this raw QPC key.
    if (log.LogfileHeader.ReservedFlags!=1) { CloseTrace(handle); return ERROR_NOT_SUPPORTED; }
    if (replay.first) {
        replay.result.eventsLost=log.LogfileHeader.EventsLost;
        replay.result.buffersLost=log.LogfileHeader.BuffersLost;
        replay.result.lossCountsKnown=true;
    }
    auto h=handle;
    const auto status=ProcessTrace(&h,1,nullptr,nullptr);
    CloseTrace(handle);
    return replay.stop ? replay.result.status : status;
}
}

TraceReadResult ReadPoolAllocationTrace(const std::wstring& path, const std::atomic_bool& cancel, const TraceReadOptions& options) {
    TraceReadResult result;
    Analyzer analyzer(options.analysisLimits);
    bool stackCoverageComplete=true;
    try {
        FileHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        BY_HANDLE_FILE_INFORMATION before{},after{};
        if (file.get()==INVALID_HANDLE_VALUE) { result.status=GetLastError(); }
        else if (!fileInfo(file.get(),before)) { result.status=ERROR_INVALID_DATA; }
        else if ((static_cast<std::uint64_t>(before.nFileSizeHigh)<<32 | before.nFileSizeLow)>options.maxFileBytes) { result.status=ERROR_FILE_TOO_LARGE; }
        else {
            std::vector<wchar_t> name(32768);
            const auto length=GetFinalPathNameByHandleW(file.get(),name.data(),static_cast<DWORD>(name.size()),FILE_NAME_NORMALIZED);
            if (!length || length>=name.size()) { result.status=ERROR_INVALID_NAME; }
            else {
                const std::wstring actualPath(name.data(),length);
                Replay replay(result,options,cancel,analyzer);
                result.status=pass(actualPath,replay);
                if (result.status!=ERROR_SUCCESS || replay.stop) {
                    if (!replay.stop) { analyzer.NoteGap(GapReason::DecodeFailure); }
                } else if (!fileInfo(file.get(),after) || !sameFile(before,after)) {
                    result.fileChanged=true; result.status=ERROR_FILE_INVALID; analyzer.NoteGap(GapReason::DecodeFailure);
                } else {
                    if (result.eventsLost || result.buffersLost || result.lossMarkers) {
                        analyzer.NoteUnknownGap(GapReason::LostEvents,result.eventsLost);
                        result.imageHistoryTruncated=true;
                    }
                    replay.indexImages();
                    replay.first=false; replay.count=0;
                    result.status=pass(actualPath,replay);
                    if (result.status!=ERROR_SUCCESS || replay.stop) {
                        if (!replay.stop) { analyzer.NoteGap(GapReason::DecodeFailure); }
                    } else { result.completed=true; }
                }
                // ProcessTrace reports cancellation when BufferCallback stops;
                // retain the reason set by the callback (timeout/capacity/OOM).
                if (replay.stop && !result.cancelled && !result.timedOut && result.status==ERROR_CANCELLED) { result.status=ERROR_MORE_DATA; }
                if (!fileInfo(file.get(),after) || !sameFile(before,after)) {
                    result.fileChanged=true; result.completed=false; result.status=ERROR_FILE_INVALID; analyzer.NoteUnknownGap(GapReason::DecodeFailure);
                }
                stackCoverageComplete=!replay.stackTruncated && !result.imageHistoryTruncated;
            }
        }
        if (!result.completed && !result.cancelled) { analyzer.NoteGap(GapReason::DecodeFailure); }
        if (cancel.load(std::memory_order_relaxed)) { result.cancelled=true; result.completed=false; result.status=ERROR_CANCELLED; analyzer.Cancel(); }
        result.analysis=analyzer.Finish();
        result.analysis.stackCoverageComplete=result.analysis.stackCoverageComplete && stackCoverageComplete;
        if (result.imageHistoryTruncated) {
            for (auto& group:result.analysis.groups) { group.frameImageIds.assign(group.stack.size(),0); }
        }
    } catch (...) {
        result.completed=false; result.status=ERROR_NOT_ENOUGH_MEMORY;
        try { analyzer.NoteGap(GapReason::DecodeFailure); result.analysis=analyzer.Finish(); } catch (...) {
            result.analysis.coverageComplete=false; result.analysis.gapReasons|=static_cast<std::uint32_t>(GapReason::DecodeFailure);
        }
    }
    return result;
}

std::vector<std::wstring> ResolvePoolTraceStack(const TraceReadResult& trace, const Group& group, const std::atomic_bool& cancel) {
    std::vector<std::wstring> names(group.stack.size());
    if (group.frameImageIds.size()!=group.stack.size() || trace.imageHistoryTruncated || cancel.load(std::memory_order_relaxed)) { return names; }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    std::unique_lock<std::mutex> lock(ks::dbghelp::SerializationMutex(),std::defer_lock);
    while (!lock.try_lock()) {
        if (cancel.load(std::memory_order_relaxed) || std::chrono::steady_clock::now()>=deadline) { return names; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FileHandle session(CreateEventW(nullptr,TRUE,FALSE,nullptr)); if (!session) { return names; }
    const auto oldOptions=SymGetOptions();
    std::map<std::uint64_t,bool> loaded;
    std::vector<LocalPe> openImages; // Outlives Cleanup, including early returns.
    // Direct local PDB loading bypasses the PE's embedded build-machine path.
    // No symbol server path, environment fallback or automatic image search.
    SymSetOptions((oldOptions & ~(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_ANYTHING))
        | SYMOPT_EXACT_SYMBOLS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS
        | SYMOPT_IGNORE_NT_SYMPATH | SYMOPT_NO_IMAGE_SEARCH);
    struct Cleanup {
        HANDLE session; DWORD options; bool initialized=false;
        ~Cleanup() { if (initialized) { SymCleanup(session); } SymSetOptions(options); }
    } cleanup{session.get(),oldOptions};
    wchar_t windows[MAX_PATH]{}; const auto n=GetWindowsDirectoryW(windows,MAX_PATH);
    if (!n || n>=MAX_PATH || localImagePath(std::wstring(windows,n)+L"\\system32").empty()
        || !SymInitializeW(session.get(),windows,FALSE)) { return names; }
    cleanup.initialized=true;
    for (std::size_t i=0;i<group.stack.size();++i) {
        if (cancel.load(std::memory_order_relaxed) || std::chrono::steady_clock::now()>=deadline) { break; }
        const auto id=group.frameImageIds[i]; if (!id || id>trace.images.size()) { continue; }
        const auto& image=trace.images[static_cast<std::size_t>(id-1)];
        if (image.id!=id || group.stack[i]<image.base || group.stack[i]-image.base>=image.size
            || image.firstTimestamp>group.representativeTimestamp || group.representativeTimestamp>=image.lastTimestamp
            || image.pid!=0 || !kernelAddress(image.base) || image.size>UINT32_MAX) { continue; }
        auto state=loaded.find(id);
        if (state==loaded.end()) {
            bool valid=false; LocalPe pe;
            if (inspectLocalPe(image,pe) && noReparsePath(pe.pdb)) {
                FileHandle pdb(CreateFileW(pe.pdb.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
                BY_HANDLE_FILE_INFORMATION pdbInfo{};
                if (pdb.get()!=INVALID_HANDLE_VALUE && localFileHandle(pdb.get()) && fileInfo(pdb.get(),pdbInfo)
                    && !pdbInfo.nFileSizeHigh && pdbInfo.nFileSizeLow<=512*1024*1024) {
                    const auto base=SymLoadModuleExW(session.get(),pdb.get(),pe.pdb.c_str(),nullptr,image.base,static_cast<DWORD>(image.size),nullptr,0);
                    IMAGEHLP_MODULEW64 module{}; module.SizeOfStruct=sizeof(module);
                    valid=base==image.base && SymGetModuleInfoW64(session.get(),base,&module)
                        && sameGuid(module.PdbSig70,pe.pdbGuid) && module.PdbAge==pe.pdbAge && !module.PdbUnmatched;
                    if (!valid && base) { SymUnloadModule64(session.get(),base); }
                    // DbgHelp may load the PDB lazily; keep its checked handle
                    // alive until this private session has been cleaned up.
                    if (valid) { pe.file=std::move(pdb); openImages.push_back(std::move(pe)); }
                }
            }
            state=loaded.emplace(id,valid).first;
        }
        if (!state->second) { continue; }
        std::vector<unsigned char> bytes(sizeof(SYMBOL_INFOW)+1024*sizeof(wchar_t));
        auto* symbol=reinterpret_cast<SYMBOL_INFOW*>(bytes.data()); symbol->SizeOfStruct=sizeof(SYMBOL_INFOW); symbol->MaxNameLen=1024;
        DWORD64 displacement=0;
        if (SymFromAddrW(session.get(),group.stack[i],&displacement,symbol) && symbol->NameLen<1024) {
            names[i].assign(symbol->Name,symbol->NameLen);
            if (displacement) { wchar_t offset[40]{}; swprintf_s(offset,L" + 0x%llx",static_cast<unsigned long long>(displacement)); names[i]+=offset; }
        }
    }
    return names;
}
}
