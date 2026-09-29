#include "HyperVMemoryEvidence.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ComputeCore.h>
#include <Wbemidl.h>
#include <atlbase.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <cwchar>
#include <functional>
#include <limits>
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace ksword::hyperv {
namespace {
constexpr std::uint64_t page = 4096;
constexpr std::size_t maxObjects = 512;
constexpr std::size_t maxJsonCharacters = 2 * 1024 * 1024;
struct ComThread {
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComThread() { if (SUCCEEDED(status)) { CoUninitialize(); } }
};
QString stringProperty(IWbemClassObject* object, const wchar_t* name)
{
    CComVariant value;
    if (SUCCEEDED(object->Get(name, 0, &value, nullptr, nullptr)) && value.vt == VT_BSTR && value.bstrVal) { return QString::fromWCharArray(value.bstrVal); }
    return {};
}
Bytes numericProperty(IWbemClassObject* object, const wchar_t* name)
{
    CComVariant value;
    if (FAILED(object->Get(name, 0, &value, nullptr, nullptr))) { return {}; }
    switch (value.vt) {
    case VT_UI8: return value.ullVal;
    case VT_UI4: return value.ulVal;
    case VT_UI2: return value.uiVal;
    case VT_I4: return value.lVal >= 0 ? Bytes(static_cast<std::uint64_t>(value.lVal)) : Bytes{};
    case VT_I8: return value.llVal >= 0 ? Bytes(static_cast<std::uint64_t>(value.llVal)) : Bytes{};
    case VT_I2: return value.iVal >= 0 ? Bytes(static_cast<std::uint64_t>(value.iVal)) : Bytes{};
    case VT_BSTR: {
        bool valid = false;
        const auto result = QString::fromWCharArray(value.bstrVal ? value.bstrVal : L"").toULongLong(&valid);
        return valid ? Bytes(result) : Bytes{};
    }
    default: return {};
    }
}
HRESULT connect(const wchar_t* path, CComPtr<IWbemServices>& service)
{
    CComPtr<IWbemLocator> locator;
    auto status = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator));
    if (FAILED(status)) { return status; }
    status = locator->ConnectServer(CComBSTR(path), nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &service);
    if (FAILED(status)) { return status; }
    return CoSetProxyBlanket(service, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
}
void query(Snapshot& out, const std::shared_ptr<Job>& job, IWbemServices* service, const wchar_t* wql, const QString& name,
    const std::function<void(IWbemClassObject*)>& consume)
{
    Source source;
    source.name = name;
    CComPtr<IEnumWbemClassObject> enumeration;
    auto status = service->ExecQuery(CComBSTR(L"WQL"), CComBSTR(wql), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumeration);
    if (SUCCEEDED(status)) {
        for (;;) {
            if (job->stopped()) { status = HRESULT_FROM_WIN32(job->cancel.load() ? ERROR_CANCELLED : ERROR_TIMEOUT); break; }
            if (source.rows >= maxObjects) { status = HRESULT_FROM_WIN32(ERROR_MORE_DATA); break; }
            CComPtr<IWbemClassObject> object;
            ULONG count = 0;
            status = enumeration->Next(200, 1, &object, &count);
            if (FAILED(status)) { break; }
            if (count && object) { consume(object); ++source.rows; }
            if (status == WBEM_S_FALSE) { source.complete = true; status = S_OK; break; }
        }
    }
    source.status = static_cast<std::uint32_t>(status);
    out.sources.push_back(std::move(source));
}
Bytes jsonNumber(const QJsonValue& value)
{
    if (value.isString()) { bool ok = false; const auto number = value.toString().toULongLong(&ok); return ok ? Bytes(number) : Bytes{}; }
    if (!value.isDouble()) { return {}; }
    const auto number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > 9007199254740991.0 || std::floor(number) != number) { return {}; }
    return static_cast<std::uint64_t>(number);
}
struct HcsApi {
    HMODULE module = LoadLibraryExW(L"computecore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    decltype(&HcsCreateOperation) create = nullptr;
    decltype(&HcsCloseOperation) close = nullptr;
    decltype(&HcsWaitForOperationResult) wait = nullptr;
    decltype(&HcsCancelOperation) cancel = nullptr;
    decltype(&HcsEnumerateComputeSystems) enumerate = nullptr;
    decltype(&HcsOpenComputeSystem) open = nullptr;
    decltype(&HcsCloseComputeSystem) closeSystem = nullptr;
    decltype(&HcsGetComputeSystemProperties) properties = nullptr;
    HcsApi() {
        if (!module) { return; }
        create = reinterpret_cast<decltype(create)>(GetProcAddress(module, "HcsCreateOperation"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(module, "HcsCloseOperation"));
        wait = reinterpret_cast<decltype(wait)>(GetProcAddress(module, "HcsWaitForOperationResult"));
        cancel = reinterpret_cast<decltype(cancel)>(GetProcAddress(module, "HcsCancelOperation"));
        enumerate = reinterpret_cast<decltype(enumerate)>(GetProcAddress(module, "HcsEnumerateComputeSystems"));
        open = reinterpret_cast<decltype(open)>(GetProcAddress(module, "HcsOpenComputeSystem"));
        closeSystem = reinterpret_cast<decltype(closeSystem)>(GetProcAddress(module, "HcsCloseComputeSystem"));
        properties = reinterpret_cast<decltype(properties)>(GetProcAddress(module, "HcsGetComputeSystemProperties"));
    }
    ~HcsApi() { if (module) { FreeLibrary(module); } }
    bool valid() const { return create && close && wait && cancel && enumerate && open && closeSystem && properties; }
};
struct Operation {
    HcsApi& api;
    HCS_OPERATION handle;
    explicit Operation(HcsApi& value) : api(value), handle(api.create(nullptr, nullptr)) {}
    ~Operation() { if (handle) { api.close(handle); } }
};
struct System {
    HcsApi& api;
    HCS_SYSTEM handle = nullptr;
    ~System() { if (handle) { api.closeSystem(handle); } }
};
HRESULT waitFor(Operation& operation, const std::shared_ptr<Job>& job, QJsonDocument& document)
{
    const auto until = std::min(job->deadline, std::chrono::steady_clock::now() + std::chrono::seconds(8));
    for (;;) {
        if (job->cancel.load() || std::chrono::steady_clock::now() >= until) {
            operation.api.cancel(operation.handle);
            return HRESULT_FROM_WIN32(job->cancel.load() ? ERROR_CANCELLED : ERROR_TIMEOUT);
        }
        PWSTR text = nullptr;
        const auto status = operation.api.wait(operation.handle, 200, &text);
        if (status == HCS_E_OPERATION_TIMEOUT) { if (text) { LocalFree(text); } continue; }
        bool parseOk = true;
        if (text) {
            const auto length = wcsnlen_s(text, maxJsonCharacters);
            if (length < maxJsonCharacters) {
                QJsonParseError parse;
                document = QJsonDocument::fromJson(QString::fromWCharArray(text, static_cast<qsizetype>(length)).toUtf8(), &parse);
                parseOk = parse.error == QJsonParseError::NoError;
            } else { parseOk = false; }
            LocalFree(text);
        } else { parseOk = false; }
        return SUCCEEDED(status) && !parseOk ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : status;
    }
}
QJsonObject property(const QJsonObject& object, const char* name)
{
    const auto key = QString::fromLatin1(name);
    if (object.value(key).isObject()) { return object.value(key).toObject(); }
    return object.value(QStringLiteral("PropertyResponses")).toObject().value(key).toObject().value(QStringLiteral("Response")).toObject();
}
void memoryFields(Inventory& row, const QJsonObject& object)
{
    const auto memory = property(object, "Memory");
    const auto nodes = memory.value(QStringLiteral("VirtualNodes")).toArray();
    std::uint64_t total = 0;
    bool valid = !nodes.empty();
    const auto nodeCount = jsonNumber(memory.value(QStringLiteral("VirtualNodeCount")));
    if (nodeCount && *nodeCount != static_cast<std::uint64_t>(nodes.size())) { valid = false; }
    for (const auto& node : nodes) {
        const auto pages = jsonNumber(node.toObject().value(QStringLiteral("MemoryUsageInPages")));
        if (!pages || *pages > (std::numeric_limits<std::uint64_t>::max() - total) / page) { valid = false; break; }
        total += *pages * page;
    }
    if (valid) { row.hcsNodeBytes = total; }
    const auto stats = property(object, "Statistics").value(QStringLiteral("Memory")).toObject();
    row.hcsPrivateWs = jsonNumber(stats.value(QStringLiteral("MemoryUsagePrivateWorkingSetBytes")));
    row.hcsCommit = jsonNumber(stats.value(QStringLiteral("MemoryUsageCommitBytes")));
    // Retain AssignedMemory/ReservedMemory verbatim. The schema does not state
    // their units; they are not silently treated as resident physical bytes.
    row.memoryEvidence = object;
}
}

void collectWmi(Snapshot& out, const std::shared_ptr<Job>& job)
{
    ComThread thread;
    if (FAILED(thread.status)) { out.sources.push_back({QStringLiteral("COM"), static_cast<std::uint32_t>(thread.status), 0, false}); return; }
    CComPtr<IWbemServices> service;
    auto status = connect(L"ROOT\\virtualization\\v2", service);
    if (SUCCEEDED(status)) {
        query(out, job, service, L"SELECT Name,ElementName,EnabledState,ProcessID FROM Msvm_ComputerSystem", QStringLiteral("Hyper-V WMI inventory"), [&](IWbemClassObject* object) {
            Inventory row;
            row.id = canonicalGuid(stringProperty(object, L"Name"));
            if (row.id.isEmpty()) { return; } // The host is not a child VM.
            row.name = stringProperty(object, L"ElementName"); row.type = QStringLiteral("VirtualMachine"); row.wmi = true;
            if (const auto state = numericProperty(object, L"EnabledState")) { row.state = QString::number(*state); }
            if (const auto pid = numericProperty(object, L"ProcessID"); pid && *pid <= MAXDWORD) { row.workerPid = static_cast<std::uint32_t>(*pid); }
            out.inventory.push_back(std::move(row));
        });
        if (!job->stopped()) {
            query(out, job, service, L"SELECT SystemName,BlockSize,NumberOfBlocks FROM Msvm_Memory", QStringLiteral("Hyper-V WMI memory"), [&](IWbemClassObject* object) {
                const auto id = canonicalGuid(stringProperty(object, L"SystemName"));
                const auto block = numericProperty(object, L"BlockSize"), count = numericProperty(object, L"NumberOfBlocks");
                if (id.isEmpty() || !block || !count || !*block || *count > std::numeric_limits<std::uint64_t>::max() / *block) { return; }
                for (auto& row : out.inventory) { if (canonicalGuid(row.id) == id) { row.wmiCapacity = *block * *count; } }
            });
        }
    } else { out.sources.push_back({QStringLiteral("Hyper-V WMI inventory"), static_cast<std::uint32_t>(status), 0, false}); }
    service.Release();
    if (job->stopped()) { return; }
    status = connect(L"ROOT\\Microsoft\\Windows\\DeviceGuard", service);
    if (FAILED(status)) { out.sources.push_back({QStringLiteral("Device Guard WMI"), static_cast<std::uint32_t>(status), 0, false}); return; }
    query(out, job, service, L"SELECT VirtualizationBasedSecurityStatus,SecurityServicesRunning FROM Win32_DeviceGuard", QStringLiteral("Device Guard WMI"), [&](IWbemClassObject* object) {
        if (const auto value = numericProperty(object, L"VirtualizationBasedSecurityStatus")) { out.vbsKnown = true; out.vbsState = static_cast<unsigned>(*value); }
        CComVariant services;
        if (SUCCEEDED(object->Get(L"SecurityServicesRunning", 0, &services, nullptr, nullptr)) && (services.vt == (VT_ARRAY | VT_I4) || services.vt == (VT_ARRAY | VT_UI4)) && services.parray) {
            LONG first = 0, last = -1;
            if (SUCCEEDED(SafeArrayGetLBound(services.parray, 1, &first)) && SUCCEEDED(SafeArrayGetUBound(services.parray, 1, &last)) && last >= first && last - first < 64) {
                for (LONG i = first; i <= last; ++i) { ULONG value = 0; if (SUCCEEDED(SafeArrayGetElement(services.parray, &i, &value))) { out.securityServices.push_back(value); } }
            }
        }
    });
}

void collectHcs(Snapshot& out, const std::shared_ptr<Job>& job)
{
    HcsApi api;
    if (!api.valid()) { out.sources.push_back({QStringLiteral("HCS inventory"), static_cast<std::uint32_t>(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)), 0, false}); return; }
    Operation enumeration(api);
    if (!enumeration.handle) { out.sources.push_back({QStringLiteral("HCS inventory"), static_cast<std::uint32_t>(E_OUTOFMEMORY), 0, false}); return; }
    QJsonDocument list;
    auto status = api.enumerate(nullptr, enumeration.handle);
    if (SUCCEEDED(status)) { status = waitFor(enumeration, job, list); }
    if (SUCCEEDED(status) && !list.isArray()) { status = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
    if (FAILED(status)) { out.sources.push_back({QStringLiteral("HCS inventory"), static_cast<std::uint32_t>(status), 0, false}); return; }
    const auto items = list.array();
    std::uint32_t retained = 0;
    for (const auto& item : items) {
        if (job->stopped() || retained >= maxObjects) { break; }
        const auto object = item.toObject();
        Inventory row;
        row.id = object.value(QStringLiteral("Id")).toString();
        if (row.id.isEmpty()) { continue; }
        row.runtimeId = object.value(QStringLiteral("RuntimeId")).toString();
        row.name = object.value(QStringLiteral("Name")).toString();
        row.owner = object.value(QStringLiteral("Owner")).toString();
        row.type = object.value(QStringLiteral("SystemType")).toString();
        row.state = object.value(QStringLiteral("State")).toString();
        row.hostingSystemId = object.value(QStringLiteral("HostingSystemId")).toString();
        row.hcs = true;
        System system{api};
        // The documented HcsOpenComputeSystem contract requires GENERIC_ALL.
        // Only the property-query exports are loaded; no VM lifecycle mutation.
        const auto id = row.id.toStdWString();
        auto propertyStatus = api.open(id.c_str(), GENERIC_ALL, &system.handle);
        QJsonDocument details;
        if (SUCCEEDED(propertyStatus)) {
            Operation operation(api);
            if (!operation.handle) { propertyStatus = E_OUTOFMEMORY; }
            else {
                const bool vm = row.type.compare(QStringLiteral("VirtualMachine"), Qt::CaseInsensitive) == 0;
                propertyStatus = api.properties(system.handle, operation.handle, vm ? L"{\"PropertyTypes\":[\"Memory\"]}" : L"{\"PropertyTypes\":[\"Statistics\"]}");
                if (SUCCEEDED(propertyStatus)) { propertyStatus = waitFor(operation, job, details); }
                if (SUCCEEDED(propertyStatus) && !details.isObject()) { propertyStatus = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
                if (SUCCEEDED(propertyStatus)) { memoryFields(row, details.object()); }
            }
        }
        out.sources.push_back({QStringLiteral("HCS properties: ") + row.id, static_cast<std::uint32_t>(propertyStatus), SUCCEEDED(propertyStatus) ? 1U : 0U, SUCCEEDED(propertyStatus)});
        out.inventory.push_back(std::move(row)); ++retained;
    }
    out.sources.push_back({QStringLiteral("HCS inventory"), retained == static_cast<std::uint32_t>(items.size()) ? 0U : static_cast<std::uint32_t>(HRESULT_FROM_WIN32(ERROR_MORE_DATA)), retained,
        !job->stopped() && retained == static_cast<std::uint32_t>(items.size())});
}
}
