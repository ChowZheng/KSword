#include "CallbackEnumeration.h"
#include "../../../shared/evidence/NumericTextParse.h"
#include <algorithm>
#include <limits>
#include <type_traits>

namespace Ksword::Features::Kernel {
namespace {
using Entry = ksword::ark::CallbackEnumEntry;
bool Has(const Entry& entry, std::uint32_t flags) { return (entry.fieldFlags & flags) == flags; }
bool PublicSource(std::uint32_t source) {
    return source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PUBLIC_API ||
        source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_FLTMGR_ENUMERATION ||
        source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_WFP_MGMT_API;
}
std::uint64_t PrimaryValue(const Entry& entry) {
    if (entry.callbackAddress != 0U && (entry.fieldFlags &
        (KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS | KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTIFIER |
         KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE | KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE)) != 0U) {
        return entry.callbackAddress;
    }
    return Has(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTIFIER) ? entry.registrationAddress : 0U;
}
} // namespace

std::uint32_t CallbackRemovalType(std::uint32_t callbackClass) {
    switch (callbackClass) {
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_THREAD;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_WFP_CALLOUT;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_GENERIC_KERNEL: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_GENERIC_KERNEL;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_BUGCHECK: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_BUGCHECK;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_BUGCHECK_REASON: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_BUGCHECK_REASON;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_SHUTDOWN: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_SHUTDOWN;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_FILE_SYSTEM: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_FILE_SYSTEM;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_LOGON_SESSION: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_LOGON_SESSION;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE_VERIFICATION: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE_VERIFICATION;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_NMI: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_NMI;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_POWER_SETTING: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_POWER_SETTING;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_COALESCING: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_COALESCING;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_PRIORITY: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PRIORITY;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_DEBUG_PRINT: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_DEBUG_PRINT;
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_PLUG_PLAY: return KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PLUG_PLAY;
    default: return 0U;
    }
}

std::wstring CallbackClassLabel(std::uint32_t callbackClass) {
    switch (callbackClass) {
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY: return L"注册表 CmCallback";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_PROCESS: return L"进程 Notify";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_THREAD: return L"线程 Notify";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE: return L"镜像加载 Notify";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT: return L"Object Callback";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER: return L"Minifilter";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_WFP_CALLOUT: return L"WFP Callout";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER: return L"ETW Provider/Consumer";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_GENERIC_KERNEL: return L"命名 CallbackObject";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_BUGCHECK: return L"BugCheck";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_BUGCHECK_REASON: return L"BugCheckReason";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_SHUTDOWN: return L"Shutdown";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_FILE_SYSTEM: return L"文件系统注册变化";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_LOGON_SESSION: return L"登录会话";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE_VERIFICATION: return L"镜像验证";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_NMI: return L"NMI";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_LEGACY_FS_FILTER: return L"传统文件系统过滤";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_POWER_SETTING: return L"电源设置";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_COALESCING: return L"Coalescing";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_PRIORITY: return L"Priority";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_DEBUG_PRINT: return L"Debug Print";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_EMP: return L"EMP";
    case KSWORD_ARK_CALLBACK_ENUM_CLASS_PLUG_PLAY: return L"PnP";
    default: return L"未知(" + std::to_wstring(callbackClass) + L")";
    }
}

std::wstring CallbackRegistrationLabel(std::uint32_t registrationType, std::uint32_t callbackClass) {
    switch (registrationType) {
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_PROCESS_LEGACY: return L"进程 Notify（Legacy）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_PROCESS_EX: return L"进程 Notify（Ex）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_PROCESS_EX2: return L"进程 Notify（Ex2）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_THREAD_LEGACY: return L"线程 Notify（Legacy）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_THREAD_EX_NON_SYSTEM: return L"线程 Notify（Ex/NonSystem）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_THREAD_EX_SUBSYSTEMS: return L"线程 Notify（Ex/Subsystems）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_IMAGE_LEGACY_OR_EX_DEFAULT: return L"镜像 Notify（Legacy/Ex 默认）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_IMAGE_EX_CONFLICTING_ARCHITECTURE: return L"镜像 Notify（Ex/冲突架构）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_BUGCHECK_CLASSIC: return L"BugCheck（经典）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_BUGCHECK_SECONDARY_DUMP: return L"BugCheckReason（SecondaryDump）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_BUGCHECK_DUMP_IO: return L"BugCheckReason（DumpIo）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_BUGCHECK_TRIAGE_DUMP: return L"BugCheckReason（TriageDump）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_BUGCHECK_REASON_OTHER: return L"BugCheckReason（其他）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_SHUTDOWN: return L"Shutdown 通知";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_FILE_SYSTEM_CHANGE: return L"文件系统注册变化";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LOGON_LEGACY: return L"登录会话（Legacy）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LOGON_EX: return L"登录会话（Ex）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_IMAGE_VERIFY_INFORMATIONAL: return L"镜像验证（Informational）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_IMAGE_VERIFY_BLOCK: return L"镜像验证（Block）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_GENERIC_CALLBACK_OBJECT: return L"命名 CallbackObject";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_NMI: return L"NMI";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_DESKTOP_OBJECT: return L"Desktop Object Callback";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LEGACY_FS_CLASS_INIT: return L"传统文件系统过滤（ClassInit）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LEGACY_FS_PRE: return L"传统文件系统过滤（Pre）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LEGACY_FS_POST: return L"传统文件系统过滤（Post）";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_POWER_SETTING: return L"电源设置";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_COALESCING: return L"Coalescing";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_PRIORITY: return L"Priority";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_DEBUG_PRINT: return L"Debug Print";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_EMP: return L"EMP";
    case KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_PLUG_PLAY: return L"PnP";
    default: return CallbackClassLabel(callbackClass) + L"（未分类）";
    }
}

CallbackRemovalPolicy CallbackRemovalPolicyFor(const Entry& entry) {
    if (entry.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_UNSUPPORTED ||
        entry.status != KSWORD_ARK_CALLBACK_ENUM_STATUS_OK || CallbackRemovalType(entry.callbackClass) == 0U ||
        entry.identityHash == 0U || entry.generation == 0U ||
        entry.callbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_ETW_PROVIDER) {
        return CallbackRemovalPolicy::Unavailable;
    }
    const bool publicApi = (entry.removeBehavior & KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_PUBLIC_API) != 0U;
    const bool candidate = Has(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE);
    if (entry.callbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_OBJECT) {
        const std::uint32_t trust = KSWORD_ARK_CALLBACK_TRUST_PDB_PROFILE | KSWORD_ARK_CALLBACK_TRUST_PROFILE_GATED |
            KSWORD_ARK_CALLBACK_TRUST_STORAGE_VALIDATED | KSWORD_ARK_CALLBACK_TRUST_STRUCTURE_SIGNATURE;
        const bool identity = Has(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_IDENTITY_HASH |
            KSWORD_ARK_CALLBACK_ENUM_FIELD_ENUMERATION_GENERATION) && entry.identityHash != 0U && entry.generation != 0U &&
            entry.registrationAddress != 0U && entry.rawStorageValue != 0U;
        const bool revalidation = publicApi &&
            (entry.removeBehavior & KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_REQUIRE_REVALIDATION) != 0U;
        if (identity && revalidation && entry.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PDB_PROFILE &&
            Has(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE | KSWORD_ARK_CALLBACK_ENUM_FIELD_VERIFIED_REMOVE) &&
            (entry.trustFlags & trust) == trust && (entry.trustFlags & KSWORD_ARK_CALLBACK_TRUST_FALLBACK_PATTERN) == 0U) {
            return CallbackRemovalPolicy::Verified;
        }
        if (identity && revalidation && candidate && Has(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_REGISTRATION_ADDRESS) &&
            entry.source == KSWORD_ARK_CALLBACK_ENUM_SOURCE_PRIVATE_OBJECT_TYPE_LIST &&
            (entry.trustFlags & KSWORD_ARK_CALLBACK_TRUST_FALLBACK_PATTERN) != 0U) {
            return CallbackRemovalPolicy::Candidate;
        }
        return CallbackRemovalPolicy::Unavailable;
    }
    if (entry.callbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER && entry.callbackAddress != 0U) {
        return entry.contextAddress != 0U && entry.registrationAddress != 0U && entry.identityHash != 0U &&
            entry.generation != 0U && candidate && publicApi ? CallbackRemovalPolicy::Candidate : CallbackRemovalPolicy::Unavailable;
    }
    if (entry.callbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_REGISTRY ||
        entry.callbackClass >= KSWORD_ARK_CALLBACK_ENUM_CLASS_GENERIC_KERNEL) {
        return entry.identityHash != 0U && candidate && publicApi ? CallbackRemovalPolicy::Candidate : CallbackRemovalPolicy::Unavailable;
    }
    if (PrimaryValue(entry) != 0U) {
        if (Has(entry, KSWORD_ARK_CALLBACK_ENUM_FIELD_VERIFIED_REMOVE) || publicApi || (candidate && PublicSource(entry.source))) {
            return CallbackRemovalPolicy::Verified;
        }
        if (candidate) return CallbackRemovalPolicy::Candidate;
    }
    return CallbackRemovalPolicy::Unavailable;
}

std::wstring CallbackRemovalLabel(const Entry& entry) {
    switch (CallbackRemovalPolicyFor(entry)) {
    case CallbackRemovalPolicy::Verified: return L"removable verified（公开 API 可验证）";
    case CallbackRemovalPolicy::Candidate: return L"removable candidate（注销前须重验证）";
    default: return L"not removable（不可移除）";
    }
}
const wchar_t* CallbackRemovalGlyph(const Entry& entry) {
    switch (CallbackRemovalPolicyFor(entry)) {
    case CallbackRemovalPolicy::Verified: return L"√";
    case CallbackRemovalPolicy::Candidate: return L"!";
    default: return L"×";
    }
}

bool ParseCallbackRemovalFields(const std::vector<std::pair<std::wstring, std::wstring>>& fields,
    Entry& entry, std::wstring& error) {
    entry = {};
    const auto parse = [&](const wchar_t* name, auto& value) {
        const auto found = std::find_if(fields.begin(), fields.end(), [name](const auto& field) { return field.first == name; });
        if (found == fields.end()) { error = L"当前回调行缺少协议字段：" + std::wstring(name); return false; }
        std::string ascii;
        for (wchar_t ch : found->second) {
            if (ch > 0x7F) { error = L"回调协议字段不是数值：" + std::wstring(name); return false; }
            ascii.push_back(static_cast<char>(ch));
        }
        const auto parsed = ksword::evidence::ParseNumericText(ascii, ksword::evidence::NumericTextDefaultRadix::Decimal);
        using Value = std::remove_reference_t<decltype(value)>;
        if (!parsed.ok || parsed.value > std::numeric_limits<Value>::max()) {
            error = L"回调协议字段无效或溢出：" + std::wstring(name); return false;
        }
        value = static_cast<Value>(parsed.value);
        return true;
    };
    const bool valid = parse(L"Class", entry.callbackClass) && parse(L"StatusCode", entry.status) &&
        parse(L"Source", entry.source) && parse(L"FieldFlags", entry.fieldFlags) && parse(L"Trust", entry.trustFlags) &&
        parse(L"Remove", entry.removeBehavior) && parse(L"Callback", entry.callbackAddress) &&
        parse(L"Registration", entry.registrationAddress) && parse(L"RawStorageValue", entry.rawStorageValue) &&
        parse(L"Context", entry.contextAddress) && parse(L"Generation", entry.generation) &&
        parse(L"IdentityHash", entry.identityHash) && parse(L"OperationMask", entry.operationMask) &&
        parse(L"ObjectTypeMask", entry.objectTypeMask);
    if (valid) error.clear();
    return valid;
}

bool BuildCallbackRemovalRequest(const Entry& entry, KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_REQUEST& packet,
    std::wstring& error) {
    packet = {};
    if (CallbackRemovalPolicyFor(entry) == CallbackRemovalPolicy::Unavailable) {
        error = L"当前回调行没有可用的公开 API 注销方式。"; return false;
    }
    if (entry.identityHash == 0U || entry.generation == 0U || PrimaryValue(entry) == 0U) {
        error = L"当前回调行缺少完整枚举身份，请使用支持 V3 枚举的驱动重新刷新。"; return false;
    }
    packet.size = sizeof(packet);
    packet.version = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION;
    packet.callbackClass = CallbackRemovalType(entry.callbackClass);
    packet.flags = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_FLAG_REQUIRE_REVALIDATION;
    packet.callbackAddress = PrimaryValue(entry);
    packet.registrationAddress = entry.registrationAddress;
    packet.rawStorageValue = entry.rawStorageValue;
    if (entry.callbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_MINIFILTER && entry.callbackAddress != 0U) {
        packet.registrationAddress = entry.contextAddress;
        packet.rawStorageValue = entry.registrationAddress;
    }
    packet.enumerationGeneration = entry.generation;
    packet.identityHash = entry.identityHash;
    packet.source = entry.source;
    packet.operationMask = entry.operationMask;
    packet.objectTypeMask = entry.objectTypeMask;
    packet.trustFlags = entry.trustFlags;
    packet.removeBehavior = KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_PUBLIC_API | KSWORD_ARK_CALLBACK_REMOVE_BEHAVIOR_REQUIRE_REVALIDATION;
    error.clear();
    return true;
}
bool CallbackRemovalResponseValid(const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_RESPONSE& response,
    std::size_t bytesReturned) {
    return bytesReturned >= sizeof(response) && response.size == sizeof(response) &&
        response.version == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION;
}
} // namespace Ksword::Features::Kernel
