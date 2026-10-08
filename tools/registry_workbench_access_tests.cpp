// Offline recording mocks run the actual access service and R0 client bodies.
// No real registry query or mutation is issued by any access-service method.
#define NOMINMAX
#include <Windows.h>
#include <QCoreApplication>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "ArkDriverClient/ArkDriverClient.h"

namespace fixture
{
struct Value { DWORD type = REG_BINARY; QByteArray data; };
struct Key { std::map<std::wstring, Value> values; std::vector<std::wstring> children; };
struct Handle { std::wstring path; REGSAM view; };
std::map<std::wstring, Key> keys;
std::vector<REGSAM> masks;
std::vector<std::wstring> deleted;
unsigned cases, opens, writes, r0Calls, r0Writes;
bool denied, growing, changed, r0Denied;
DWORD enumFailure = ERROR_SUCCESS;
DWORD r0Status = KSWORD_ARK_REGISTRY_ENUM_STATUS_SUCCESS;
std::wstring writtenName;
QByteArray writtenData;
DWORD writtenType;

void reset()
{
    keys.clear(); masks.clear(); deleted.clear(); opens = writes = r0Calls = r0Writes = 0;
    denied = growing = changed = r0Denied = false; enumFailure = ERROR_SUCCESS;
    r0Status = KSWORD_ARK_REGISTRY_ENUM_STATUS_SUCCESS;
    keys[L"Software\\Test"].values[L"v"] = {REG_BINARY, QByteArray("abcd", 4)};
}

LONG open(HKEY, LPCWSTR path, DWORD options, REGSAM access, PHKEY result)
{
    ++opens; masks.push_back(access);
    assert(options == 0 || options == REG_OPTION_OPEN_LINK);
    if (denied) return ERROR_ACCESS_DENIED;
    const std::wstring name(path ? path : L"");
    if (!keys.count(name)) return ERROR_FILE_NOT_FOUND;
    *result = reinterpret_cast<HKEY>(new Handle{name, access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY)});
    return ERROR_SUCCESS;
}

LONG close(HKEY key) { delete reinterpret_cast<Handle*>(key); return ERROR_SUCCESS; }

LONG query(HKEY raw, LPCWSTR name, LPDWORD, LPDWORD type, LPBYTE output, LPDWORD bytes)
{
    auto& key = keys.at(reinterpret_cast<Handle*>(raw)->path);
    const auto iterator = key.values.find(name ? name : L"");
    if (iterator == key.values.end()) return ERROR_FILE_NOT_FOUND;
    const Value& value = iterator->second;
    *type = value.type;
    if (!output)
    {
        *bytes = growing ? 1 : static_cast<DWORD>(value.data.size());
        growing = false;
        return ERROR_SUCCESS;
    }
    if (*bytes < static_cast<DWORD>(value.data.size()))
    {
        *bytes = static_cast<DWORD>(value.data.size());
        return ERROR_MORE_DATA;
    }
    *bytes = static_cast<DWORD>(value.data.size());
    std::memcpy(output, value.data.constData(), *bytes);
    return ERROR_SUCCESS;
}

LONG enumValue(HKEY raw, DWORD index, LPWSTR name, LPDWORD nameChars, LPDWORD,
    LPDWORD type, LPBYTE output, LPDWORD bytes)
{
    const auto& values = keys.at(reinterpret_cast<Handle*>(raw)->path).values;
    if (enumFailure) return static_cast<LONG>(enumFailure);
    if (index >= values.size()) return ERROR_NO_MORE_ITEMS;
    auto iterator = values.begin(); std::advance(iterator, index);
    const auto& rawName = iterator->first;
    const auto& value = iterator->second;
    *type = value.type;
    if (*nameChars <= rawName.size() || *bytes < static_cast<DWORD>(value.data.size()))
    {
        *nameChars = static_cast<DWORD>(rawName.size());
        *bytes = static_cast<DWORD>(value.data.size());
        return ERROR_MORE_DATA;
    }
    std::copy(rawName.begin(), rawName.end(), name);
    name[rawName.size()] = L'\0';
    *nameChars = static_cast<DWORD>(rawName.size());
    *bytes = static_cast<DWORD>(value.data.size());
    std::memcpy(output, value.data.constData(), *bytes);
    return ERROR_SUCCESS;
}

LONG enumKey(HKEY raw, DWORD index, LPWSTR name, LPDWORD nameChars, LPDWORD, LPWSTR, LPDWORD, PFILETIME)
{
    const auto& children = keys.at(reinterpret_cast<Handle*>(raw)->path).children;
    if (index >= children.size()) return ERROR_NO_MORE_ITEMS;
    if (*nameChars <= children[index].size()) return ERROR_MORE_DATA;
    std::copy(children[index].begin(), children[index].end(), name);
    *nameChars = static_cast<DWORD>(children[index].size()); name[*nameChars] = L'\0'; return ERROR_SUCCESS;
}

unsigned infoCalls;
LONG info(HKEY, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME time)
{
    time->dwHighDateTime = 0; time->dwLowDateTime = changed ? ++infoCalls : 1; return ERROR_SUCCESS;
}

LONG set(HKEY, LPCWSTR name, DWORD, DWORD type, const BYTE* data, DWORD bytes)
{
    ++writes; writtenName = name; writtenType = type;
    writtenData = QByteArray(reinterpret_cast<const char*>(data), bytes); return ERROR_SUCCESS;
}

LONG removeValue(HKEY, LPCWSTR name) { ++writes; writtenName = name; return ERROR_SUCCESS; }
LONG create(HKEY, LPCWSTR path, DWORD, LPWSTR, DWORD, REGSAM access, const SECURITY_ATTRIBUTES*, PHKEY result, LPDWORD)
{
    ++writes; masks.push_back(access); keys[path] = Key{};
    *result = reinterpret_cast<HKEY>(new Handle{path, access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY)});
    return ERROR_SUCCESS;
}
LONG removeKey(HKEY, LPCWSTR path, REGSAM view, DWORD)
{
    ++writes; masks.push_back(view); deleted.push_back(path); return ERROR_SUCCESS;
}

void checkView(REGSAM expected)
{
    assert(!masks.empty());
    for (const auto mask : masks) assert((mask & (KEY_WOW64_32KEY | KEY_WOW64_64KEY)) == expected);
}
}

#define RegOpenKeyExW fixture::open
#define RegCloseKey fixture::close
#define RegQueryValueExW fixture::query
#define RegEnumValueW fixture::enumValue
#define RegEnumKeyExW fixture::enumKey
#define RegQueryInfoKeyW fixture::info
#define RegSetValueExW fixture::set
#define RegDeleteValueW fixture::removeValue
#define RegCreateKeyExW fixture::create
#define RegDeleteKeyExW fixture::removeKey
#include "RegistryDock/RegistryWorkbenchAccess.cpp"
#include "ArkDriverClient/ArkDriverRegistry.cpp"

namespace ksword::ark
{
IoResult DriverClient::deviceIoControl(unsigned long code, void*, unsigned long,
    void* output, unsigned long bytes, DriverHandle*) const
{
    ++fixture::r0Calls;
    if (fixture::r0Denied)
    {
        IoResult result; result.ok = false; result.win32Error = ERROR_ACCESS_DENIED;
        return result;
    }
    std::memset(output, 0, bytes);
    if (code == IOCTL_KSWORD_ARK_READ_REGISTRY_VALUE)
    {
        auto* response = static_cast<KSWORD_ARK_READ_REGISTRY_VALUE_RESPONSE*>(output);
        response->version = 1; response->status = KSWORD_ARK_REGISTRY_READ_STATUS_SUCCESS;
        response->valueType = REG_BINARY; response->dataBytes = 4;
        response->requiredBytes = fixture::r0Status == KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL ? 8192 : 4;
        std::memcpy(response->data, "abcd", 4);
    }
    else if (code == IOCTL_KSWORD_ARK_ENUM_REGISTRY_KEY)
    {
        auto* response = static_cast<KSWORD_ARK_ENUM_REGISTRY_KEY_RESPONSE*>(output);
        response->version = 1; response->status = fixture::r0Status;
    }
    else
    {
        ++fixture::r0Writes;
        auto* response = static_cast<KSWORD_ARK_REGISTRY_OPERATION_RESPONSE*>(output);
        response->version = 1; response->status = KSWORD_ARK_REGISTRY_OPERATION_STATUS_SUCCESS;
    }
    IoResult result; result.ok = true; result.bytesReturned = bytes; return result;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    using namespace fixture;
    const QString path = QStringLiteral("HKLM\\Software\\Test");
    QString error;
    RegistryValueState state;
    RegistryKeyListing listing;
    for (const int bits : {0, 32, 64})
    {
        const RegistryAccessContext context{bits, false};
        const REGSAM expected = bits == 32 ? KEY_WOW64_32KEY : bits == 64 ? KEY_WOW64_64KEY : 0;
        reset(); growing = true;
        assert(RegistryWorkbenchAccess::read(path, QStringLiteral("v"), context, &state, &error));
        assert(state.exists && state.complete && state.data == "abcd" && state.requiredBytes == 4);
        assert(r0Calls == 0 && opens == 1 && error.isEmpty()); checkView(expected); ++cases;
        reset(); keys[L"Software\\Test"].values[std::wstring(512, L'V')] = {REG_BINARY, QByteArray(5000, 'X')};
        assert(RegistryWorkbenchAccess::enumerate(path, context, &listing, &error));
        assert(listing.complete && listing.values.size() == 2 && listing.values[0].data.size() == 5000); checkView(expected); ++cases;
        reset(); keys[L"Software\\Test"].values[L"  (默认)  "] = {REG_SZ, QByteArray("X\0\0\0", 4)};
        assert(RegistryWorkbenchAccess::read(path, QStringLiteral("  (默认)  "), context, &state, &error));
        assert(state.exists && state.name == QStringLiteral("  (默认)  "));
        assert(RegistryWorkbenchAccess::write(path, state, context, &error));
        assert(writtenName == L"  (默认)  " && writtenData == state.data && writtenType == REG_SZ); checkView(expected); ++cases;
        reset(); assert(RegistryWorkbenchAccess::removeValue(path, QStringLiteral(" (默认) "), context, &error));
        assert(writtenName == L" (默认) " && writes == 1); checkView(expected); ++cases;
        reset(); assert(RegistryWorkbenchAccess::createKey(QStringLiteral("HKLM\\NewKey"), context, &error));
        assert(writes == 1 && opens == 0); ++cases;
        checkView(expected);
    }
    reset(); assert(RegistryWorkbenchAccess::read(path, QStringLiteral("missing"), {}, &state, &error));
    assert(!state.exists && state.complete); ++cases;
    reset(); assert(RegistryWorkbenchAccess::read(QStringLiteral("HKLM\\missing"), QStringLiteral("v"), {}, &state, &error));
    assert(!state.exists); ++cases;
    reset(); denied = true;
    assert(!RegistryWorkbenchAccess::read(path, QStringLiteral("v"), {}, &state, &error));
    assert(r0Calls == 0 && !error.isEmpty()); ++cases;
    for (const QString& invalid : {QStringLiteral("HKLMoops\\Test"), QStringLiteral("HKLM\\"), QStringLiteral(" HKLM\\Test"), QStringLiteral("HKLM\\a\\\\b")})
    {
        reset(); assert(!RegistryWorkbenchAccess::read(invalid, QStringLiteral("v"), {}, &state, &error));
        assert(opens == 0 && r0Calls == 0); ++cases;
    }
    for (const int bits : {32, 64, 99})
    {
        reset(); assert(!RegistryWorkbenchAccess::read(path, QStringLiteral("v"), {bits, true}, &state, &error));
        assert(opens == 0 && r0Calls == 0); ++cases;
    }
    reset(); assert(!RegistryWorkbenchAccess::read(QStringLiteral("HKCR\\Test"), QStringLiteral("v"), {0, true}, &state, &error));
    assert(r0Calls == 0 && opens == 0 && RegistryWorkbenchAccess::kernelPath(QStringLiteral("HKCR\\Test")).isEmpty()); ++cases;
    reset(); assert(RegistryWorkbenchAccess::read(path, QStringLiteral("v"), {0, true}, &state, &error));
    assert(r0Calls == 1 && opens == 0 && state.complete && state.data == "abcd"); ++cases;
    reset(); r0Denied = true;
    assert(!RegistryWorkbenchAccess::read(path, QStringLiteral("v"), {0, true}, &state, &error));
    assert(r0Calls == 1 && opens == 0 && !error.isEmpty()); ++cases;
    reset(); r0Status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
    assert(RegistryWorkbenchAccess::read(path, QStringLiteral("v"), {0, true}, &state, &error));
    assert(!state.complete && state.requiredBytes == 8192 && state.data.size() == 4); ++cases;
    assert(!RegistryWorkbenchAccess::write(path, state, {0, true}, &error));
    assert(r0Writes == 0 && writes == 0); ++cases;
    reset(); state.complete = true; state.data = "abcd"; state.requiredBytes = 3;
    assert(!RegistryWorkbenchAccess::write(path, state, {}, &error)); assert(writes == 0 && opens == 0); ++cases;
    reset(); state.complete = true; state.data = QByteArray(4097, 'X'); state.requiredBytes = 4097;
    assert(!RegistryWorkbenchAccess::write(path, state, {0, true}, &error));
    assert(r0Calls == 0 && writes == 0 && opens == 0); ++cases;
    reset(); state.data = QByteArray(16 * 1024 * 1024 + 1, 'X'); state.requiredBytes = static_cast<quint32>(state.data.size());
    assert(!RegistryWorkbenchAccess::write(path, state, {}, &error));
    assert(writes == 0 && opens == 0); ++cases;
    reset(); keys[L"Software\\Test"].values[L"large"] = {REG_BINARY, QByteArray(16 * 1024 * 1024 + 1, 'X')};
    assert(RegistryWorkbenchAccess::enumerate(path, {}, &listing, &error));
    assert(!listing.complete && !listing.warning.isEmpty()); ++cases;
    reset(); changed = true;
    assert(RegistryWorkbenchAccess::enumerate(path, {}, &listing, &error));
    assert(!listing.complete && !listing.warning.isEmpty()); ++cases;
    reset(); enumFailure = ERROR_ACCESS_DENIED;
    assert(RegistryWorkbenchAccess::enumerate(path, {}, &listing, &error));
    assert(!listing.complete && !listing.warning.isEmpty() && r0Calls == 0); ++cases;
    for (const auto context : {RegistryAccessContext{32, false}, RegistryAccessContext{0, true}})
    {
        reset(); assert(!RegistryWorkbenchAccess::removeTree(QStringLiteral("HKLM"), context, &error));
        assert(writes == 0 && r0Writes == 0 && opens == 0); ++cases;
        reset(); keys[L"Software"] = Key{};
        keys[L"Software\\Test"].values[L"SymbolicLinkValue"] = {REG_LINK, QByteArray("target")};
        assert(!RegistryWorkbenchAccess::removeTree(path, context, &error));
        assert(writes == 0 && r0Writes == 0); ++cases;
        reset(); keys[L"Software"].values[L"SymbolicLinkValue"] = {REG_LINK, QByteArray("target")};
        assert(!RegistryWorkbenchAccess::removeTree(path, context, &error));
        assert(writes == 0 && r0Writes == 0); ++cases;
        reset(); keys[L"Software"] = Key{}; denied = true;
        assert(!RegistryWorkbenchAccess::removeTree(path, context, &error));
        assert(writes == 0 && r0Writes == 0); ++cases;
    }
    reset(); keys[L"Software"] = Key{}; r0Status = KSWORD_ARK_REGISTRY_ENUM_STATUS_PARTIAL;
    assert(!RegistryWorkbenchAccess::removeTree(path, {0, true}, &error));
    assert(r0Writes == 0 && writes == 0); ++cases;
    reset(); keys[L"Software"] = Key{}; enumFailure = ERROR_ACCESS_DENIED;
    assert(!RegistryWorkbenchAccess::removeTree(path, {}, &error));
    assert(writes == 0 && r0Writes == 0); ++cases;
    reset(); keys[L"Software"] = Key{};
    keys[L"Software\\Test"].children.push_back(L"Child"); keys[L"Software\\Test\\Child"] = Key{};
    assert(RegistryWorkbenchAccess::removeTree(path, {64, false}, &error));
    assert(deleted.size() == 2 && deleted[0] == L"Software\\Test\\Child" && deleted[1] == L"Software\\Test");
    for (const auto mask : masks) assert((mask & (KEY_WOW64_32KEY | KEY_WOW64_64KEY)) == KEY_WOW64_64KEY);
    ++cases;
    std::printf("registry-workbench-access regression: %u cases passed; all registry APIs mocked\n", cases);
}
