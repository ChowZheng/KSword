#include "TestSupport.h"
#include "../KswordARKLight/Features/Kernel/CallbackEnumeration.h"
#include "../KswordARKLight/Features/Kernel/KernelPageLayout.h"
#include <algorithm>

namespace {
using namespace Ksword::Features::Kernel;
using Entry = ksword::ark::CallbackEnumEntry;
Entry Process() {
    Entry entry;
    entry.callbackClass = KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS;
    entry.status = KSWORD_ARK_CALLBACK_ENUM_STATUS_OK;
    entry.source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_NOTIFY_ARRAY;
    entry.fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE |
        KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTITY_HASH | KSWORD_ARK_CALLBACK_ENUM_FIELD_ENUMERATION_GENERATION;
    entry.removeBehavior = KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_PUBLIC_API | KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_REQUIRE_REVALIDATION;
    entry.callbackAddress = 0xFFFF800000001000ULL;
    entry.identityHash = 0x1234567890ABCDEFULL;
    entry.generation = 42U;
    entry.registrationType = KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_PROCESS_EX2;
    return entry;
}
} // namespace

int RunCallbackEnumerationTests() {
    KswordTests::Suite s(L"Light callback synchronization");
    using namespace Ksword::Features::Kernel;
    const auto columns = CanonicalColumnNames(KernelFeatureId::CallbackEnumeration);
    s.expect(std::find(columns.begin(), columns.end(), L"注册类型") != columns.end() &&
        std::find(columns.begin(), columns.end(), L"可移除") != columns.end(),
        L"Callback table exposes registration subtype and removal indication");
    s.expect(ColumnAliases(KernelFeatureId::CallbackEnumeration, L"注册类型") ==
        std::vector<std::wstring>{L"RegistrationTypeText", L"RegistrationType"},
        L"Visible registration column resolves the actual enumeration subtype rather than an empty cell");
    s.expect(ColumnAliases(KernelFeatureId::CallbackEnumeration, L"可移除") ==
        std::vector<std::wstring>{L"RemovePolicyGlyph"},
        L"Visible removal column resolves the policy indication");
    s.expect(CallbackRemovalType(2U) == 1U && CallbackRemovalType(1U) == 5U,
        L"Process and registry enumeration IDs map to distinct removal IDs");
    for (std::uint32_t value : { 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U, 18U, 19U, 20U, 21U, 23U }) {
        s.expect(CallbackRemovalType(value) == value && CallbackClassLabel(value).find(L"未知") == std::wstring::npos,
            L"October 1 extended unregister categories have labels and request mappings");
    }
    s.expect(CallbackRemovalType(17U) == 0U && CallbackRemovalType(22U) == 0U && CallbackRemovalType(999U) == 0U,
        L"Legacy FsRtl, EMP and unknown classes have no unregister mapping");
    s.expect(CallbackRegistrationLabel(3U, 2U) == L"进程 Notify（Ex2）",
        L"Process Ex2 registration remains distinguishable in the table");
    s.expect(CallbackRegistrationLabel(0U, 6U) == L"Minifilter（未分类）",
        L"Unknown subtype retains its known basic class when the category column is hidden");
    auto process = Process();
    KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_REQUEST packet{};
    std::wstring error;
    s.expect(BuildCallbackRemovalRequest(process, packet, error) && packet.callbackClass == 1U &&
            packet.callbackAddress == 0xFFFF800000001000ULL && packet.enumerationGeneration == 42U &&
            packet.identityHash == 0x1234567890ABCDEFULL,
        L"Ex2 removal carries the function and unchanged enumeration identity to the existing EX protocol");
    s.expect(packet.flags == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_FLAG_REQUIRE_REVALIDATION &&
            packet.removeBehavior == (KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_PUBLIC_API |
                KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_REQUIRE_REVALIDATION),
        L"Public removal never forwards experimental unlink behavior");
    auto child = process;
    child.callbackClass = 6U;
    child.source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_PATTERN_SCAN;
    child.trustFlags = KSWORD_ARK_CALLBACK_TRUST_FALLBACK_PATTERN;
    child.contextAddress = 0xFFFF800000008000ULL;
    child.registrationAddress = 0xFFFF800000004000ULL;
    child.rawStorageValue = 0xFFFF800000004010ULL;
    child.operationMask = 0x10U;
    s.expect(CallbackRemovalPolicyFor(child) == CallbackRemovalPolicy::Candidate &&
            std::wstring(CallbackRemovalGlyph(child)) == L"!",
        L"A privately discovered minifilter child remains a candidate despite public owner unloading");
    s.expect(BuildCallbackRemovalRequest(child, packet, error) && packet.callbackClass == 6U &&
            packet.registrationAddress == 0xFFFF800000008000ULL && packet.rawStorageValue == 0xFFFF800000004000ULL &&
            packet.callbackAddress == 0xFFFF800000001000ULL && packet.operationMask == 0x10U,
        L"Minifilter child EX packet carries owner FilterObject and exact operation record in separate fields");
    s.expect(child.registrationAddress == 0xFFFF800000004000ULL && child.rawStorageValue == 0xFFFF800000004010ULL,
        L"Building the owner unload request does not alter displayed callback storage identity");
    child.contextAddress = 0U;
    s.expect(!BuildCallbackRemovalRequest(child, packet, error), L"A minifilter child without owner identity cannot unload");
    auto parent = process;
    parent.callbackClass = 6U;
    parent.callbackAddress = 0U;
    parent.registrationAddress = 0xFFFF800000008000ULL;
    parent.fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTIFIER;
    parent.source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_FLTMGR_ENUMERATION;
    s.expect(BuildCallbackRemovalRequest(parent, packet, error) &&
            packet.callbackAddress == 0xFFFF800000008000ULL && packet.registrationAddress == 0xFFFF800000008000ULL,
        L"A public minifilter parent can unload without a child callback function");
    auto object = process;
    object.callbackClass = 5U;
    object.source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PDB_PROFILE;
    object.registrationAddress = 0xFFFF800000007000ULL;
    object.rawStorageValue = 0xFFFF800000005000ULL;
    object.fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE | KSWORD_ARK_CALLBACK_ENUM_FIELD_VERIFIED_REMOVE;
    object.trustFlags = KSWORD_ARK_CALLBACK_TRUST_PDB_PROFILE | KSWORD_ARK_CALLBACK_TRUST_PROFILE_GATED |
        KSWORD_ARK_CALLBACK_TRUST_STORAGE_VALIDATED | KSWORD_ARK_CALLBACK_TRUST_STRUCTURE_SIGNATURE;
    s.expect(CallbackRemovalPolicyFor(object) == CallbackRemovalPolicy::Verified &&
            BuildCallbackRemovalRequest(object, packet, error) && packet.registrationAddress == 0xFFFF800000007000ULL &&
            packet.rawStorageValue == 0xFFFF800000005000ULL,
        L"Verified Object removal uses the actual RegistrationHandle rather than callback or list node");
    object.fieldFlags &= ~KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE;
    s.expect(!BuildCallbackRemovalRequest(object, packet, error), L"Object public API flags alone do not prove a registration handle");
    object.source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_OBJECT_TYPE_LIST;
    object.fieldFlags &= ~KSWORD_ARK_CALLBACK_ENUM_FIELD_VERIFIED_REMOVE;
    object.fieldFlags |= KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS;
    object.trustFlags = KSWORD_ARK_CALLBACK_TRUST_FALLBACK_PATTERN;
    s.expect(CallbackRemovalPolicyFor(object) == CallbackRemovalPolicy::Candidate && BuildCallbackRemovalRequest(object, packet, error),
        L"Heuristic Object candidates require complete identity and remain explicitly unverified");
    auto registry = process;
    registry.callbackClass = 1U;
    registry.source = KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_REGISTRY_LIST;
    registry.registrationAddress = 0x1235U;
    s.expect(CallbackRemovalPolicyFor(registry) == CallbackRemovalPolicy::Candidate &&
            BuildCallbackRemovalRequest(registry, packet, error) && packet.callbackClass == 5U && packet.registrationAddress == 0x1235U,
        L"Registry removal preserves a Cookie value without kernel-pointer alignment checks");
    auto extended = process;
    extended.callbackClass = 16U;
    s.expect(CallbackRemovalPolicyFor(extended) == CallbackRemovalPolicy::Candidate && BuildCallbackRemovalRequest(extended, packet, error),
        L"New public unregister categories remain candidates until driver revalidation");
    auto unsupported = process;
    unsupported.callbackClass = 8U;
    s.expect(!BuildCallbackRemovalRequest(unsupported, packet, error), L"ETW provider nodes cannot substitute for registration handles");
    unsupported = process;
    unsupported.removeBehavior = KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_EXPERIMENTAL_UNLINK;
    unsupported.fieldFlags = KSWORD_ARK_CALLBACK_ENUM_FIELD_EXPERIMENTAL_REMOVE;
    s.expect(!BuildCallbackRemovalRequest(unsupported, packet, error) && std::wstring(CallbackRemovalGlyph(unsupported)) == L"×",
        L"Unlink-only rows are unavailable rather than removable candidates");
    unsupported = process;
    unsupported.identityHash = 0U;
    s.expect(!BuildCallbackRemovalRequest(unsupported, packet, error), L"Old driver rows lacking V3 identity cannot reach EX removal");
    unsupported = process;
    unsupported.status = KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED;
    s.expect(!BuildCallbackRemovalRequest(unsupported, packet, error), L"A failed enumeration cannot authorize a removal");
    std::vector<std::pair<std::wstring, std::wstring>> fields{
        {L"Class", L"2"}, {L"StatusCode", L"1"}, {L"Source", L"5"}, {L"FieldFlags", std::to_wstring(process.fieldFlags)},
        {L"Trust", L"0"}, {L"Remove", std::to_wstring(process.removeBehavior)}, {L"Callback", L"0xFFFF800000001000"},
        {L"Registration", L"0"}, {L"RawStorageValue", L"0"}, {L"Context", L"0"}, {L"Generation", L"42"},
        {L"IdentityHash", L"0x1234567890ABCDEF"}, {L"OperationMask", L"0"}, {L"ObjectTypeMask", L"0"},
        {L"Status", L"Unsupported OK (1)"}, {L"RemovePolicy", L"not removable"}
    };
    Entry decoded;
    s.expect(ParseCallbackRemovalFields(fields, decoded, error) && decoded.callbackAddress == 0xFFFF800000001000ULL &&
            BuildCallbackRemovalRequest(decoded, packet, error),
        L"Action mapping uses numeric identity cells rather than translated status or policy text");
    fields.front().second = L"4294967296";
    s.expect(!ParseCallbackRemovalFields(fields, decoded, error), L"A class wider than the protocol field is rejected without truncation");
    fields.front().second = L"-2";
    s.expect(!ParseCallbackRemovalFields(fields, decoded, error), L"Signed protocol fields are rejected");
    fields.front().second = L"2";
    fields.erase(fields.begin() + 1);
    s.expect(!ParseCallbackRemovalFields(fields, decoded, error), L"A missing numeric status cannot fall back to a display label");
    KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_RESPONSE response{};
    s.expect(!CallbackRemovalResponseValid(response, 0U), L"An empty response with default zero NTSTATUS is not success");
    response.size = sizeof(response);
    response.version = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION;
    s.expect(CallbackRemovalResponseValid(response, sizeof(response)), L"Complete current EX responses pass the layout check");
    s.expect(!CallbackRemovalResponseValid(response, sizeof(response) - 1U), L"Truncated EX responses are rejected");
    response.version = 99U;
    s.expect(!CallbackRemovalResponseValid(response, sizeof(response)), L"Unknown EX response versions are rejected");
    s.report();
    return s.failures();
}
