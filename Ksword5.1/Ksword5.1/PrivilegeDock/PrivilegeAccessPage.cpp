#include "PrivilegeAccessPage.h"
#include "PrivilegeAccessSecurity.h"
#include "../Internationalization/LanguageManager.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/ThemeStatusRole.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPromise>
#include <QPushButton>
#include <QSplitter>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <Sddl.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Authz.lib")

namespace ks::privilege
{
namespace
{
    using namespace access;

    QString text(const char* key, const char* source)
    {
        return ks::i18n::contextText(QString::fromLatin1(key), QString::fromUtf8(source));
    }

    QString hex(const DWORD value)
    {
        return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
    }

    QString sidText(PSID sid)
    {
        if (sid == nullptr || !IsValidSid(sid)) return {};
        LPWSTR string = nullptr;
        if (!ConvertSidToStringSidW(sid, &string)) return {};
        const QString result = QString::fromWCharArray(string);
        LocalFree(string);
        return result;
    }

    QString sidAccount(PSID sid)
    {
        if (sid == nullptr || !IsValidSid(sid)) return {};
        DWORD accountSize = 0;
        DWORD domainSize = 0;
        SID_NAME_USE use{};
        LookupAccountSidW(nullptr, sid, nullptr, &accountSize, nullptr, &domainSize, &use);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || accountSize > 32768 || domainSize > 32768)
            return {};
        std::vector<wchar_t> account(accountSize + 1);
        std::vector<wchar_t> domain(domainSize + 1);
        if (!LookupAccountSidW(nullptr, sid, account.data(), &accountSize,
            domain.data(), &domainSize, &use)) return {};
        const QString name = QString::fromWCharArray(account.data());
        const QString prefix = QString::fromWCharArray(domain.data());
        return prefix.isEmpty() ? name : prefix + QLatin1Char('\\') + name;
    }

    struct Handle
    {
        HANDLE value = nullptr;
        explicit Handle(HANDLE handle = nullptr) : value(handle) {}
        ~Handle() { if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    struct Descriptor
    {
        PSECURITY_DESCRIPTOR value = nullptr;
        ~Descriptor() { if (value != nullptr) LocalFree(value); }
        Descriptor() = default;
        Descriptor(const Descriptor&) = delete;
        Descriptor& operator=(const Descriptor&) = delete;
    };

    struct KeyHandle
    {
        HKEY value = nullptr;
        ~KeyHandle() { if (value != nullptr) RegCloseKey(value); }
    };

    struct ServiceHandle
    {
        SC_HANDLE value = nullptr;
        ~ServiceHandle() { if (value != nullptr) CloseServiceHandle(value); }
    };

    struct SidEvidence
    {
        QString sid;
        DWORD attributes = 0;
        bool user = false;
        bool restricted = false;
    };

    struct Request
    {
        DWORD pid = 0;
        ObjectKind kind = ObjectKind::File;
        QString path;
        DWORD desired = GENERIC_READ;
        DWORD registryView = KEY_WOW64_64KEY;
    };

    // FILE_ID_INFO's documented ABI, also available when the project targets older SDK macros.
    struct FileIdentity { ULONGLONG volume = 0; BYTE id[16]{}; };
    constexpr auto FileIdentityClass = static_cast<FILE_INFO_BY_HANDLE_CLASS>(18);
    static_assert(sizeof(FileIdentity) == 24);

    enum class Stage
    {
        None, Cancelled, Process, ProcessIdentity, Token, TokenIdentity, Duplicate,
        InvalidTarget, Target, TargetIdentity, Descriptor, Label, Impersonate, Revert, Internal
    };

    struct Anchor
    {
        Request request;
        Handle process;
        Handle token;
        Handle impersonation;
        HANDLE file = INVALID_HANDLE_VALUE;
        HKEY key = nullptr;
        SC_HANDLE service = nullptr;
        SC_HANDLE manager = nullptr;
        ULONGLONG creation = 0;
        TOKEN_STATISTICS statistics{};
        FileIdentity fileIdentity{};
        bool fileIdentityKnown = false;
        QString canonicalPath;
        QString descriptorSddl;
        QString labelSddl;
        DWORD labelError = ERROR_SUCCESS;
        ~Anchor()
        {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (key != nullptr) RegCloseKey(key);
            if (service != nullptr) CloseServiceHandle(service);
            if (manager != nullptr) CloseServiceHandle(manager);
        }
        HANDLE objectHandle() const
        {
            if (request.kind == ObjectKind::Registry) return reinterpret_cast<HANDLE>(key);
            if (request.kind == ObjectKind::Service) return reinterpret_cast<HANDLE>(service);
            return file;
        }
        SE_OBJECT_TYPE objectType() const
        {
            if (request.kind == ObjectKind::Registry) return SE_REGISTRY_KEY;
            if (request.kind == ObjectKind::Service) return SE_SERVICE;
            return SE_FILE_OBJECT;
        }
    };

    struct AceEvidence
    {
        AceView ace;
        QString sid;
        QString account;
        std::vector<SidEvidence> matches;
    };

    struct Result
    {
        Request request;
        Stage stage = Stage::None;
        DWORD error = ERROR_SUCCESS;
        std::shared_ptr<Anchor> anchor;
        QString processPath;
        QString user;
        QString userSid;
        QString owner;
        QString ownerSid;
        QString groupSid;
        QString canonicalPath;
        QString descriptorSddl;
        QString labelSddl;
        bool daclPresent = false;
        bool nullDacl = false;
        bool aclValid = true;
        bool complex = false;
        bool daclProtected = false;
        bool labelKnown = false;
        bool labelExplicit = false;
        DWORD labelError = ERROR_SUCCESS;
        DWORD labelRid = 0;
        DWORD labelPolicy = 0;
        bool tokenIntegrityKnown = false;
        DWORD tokenRid = 0;
        bool tokenPolicyKnown = false;
        DWORD tokenPolicy = 0;
        bool appContainerKnown = false;
        DWORD appContainer = 0;
        DWORD groups = 0;
        DWORD denyOnlyGroups = 0;
        DWORD restrictedGroups = 0;
        bool sidEvidenceComplete = true;
        std::vector<AceEvidence> aces;
        NativeCheck native;
        NativeCheck authz;
        bool probe = false;
        bool probeOpened = false;
        bool probeIdentityVerified = false;
        bool reverted = true;
        DWORD probeError = ERROR_SUCCESS;
        std::vector<QString> usedPrivilegeNames;
    };

    bool isCancelled(const std::shared_ptr<std::atomic_bool>& flag, Result& result)
    {
        if (!flag->load(std::memory_order_relaxed)) return false;
        result.stage = Stage::Cancelled;
        result.error = ERROR_CANCELLED;
        return true;
    }

    bool queryTokenBytes(HANDLE token, TOKEN_INFORMATION_CLASS type, std::vector<BYTE>& bytes)
    {
        DWORD size = 0;
        GetTokenInformation(token, type, nullptr, 0, &size);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 1024 * 1024)
            return false;
        bytes.resize(size);
        return GetTokenInformation(token, type, bytes.data(), size, &size) != FALSE;
    }

    bool queryStatistics(HANDLE token, TOKEN_STATISTICS& stats)
    {
        DWORD size = sizeof(stats);
        return GetTokenInformation(token, TokenStatistics, &stats, size, &size) != FALSE;
    }

    bool sameLuid(const LUID left, const LUID right)
    {
        return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
    }

    bool sameStatistics(const TOKEN_STATISTICS& left, const TOKEN_STATISTICS& right)
    {
        return sameLuid(left.TokenId, right.TokenId) && sameLuid(left.ModifiedId, right.ModifiedId)
            && sameLuid(left.AuthenticationId, right.AuthenticationId);
    }

    bool creationTime(HANDLE process, ULONGLONG& value)
    {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return false;
        value = (ULONGLONG(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
        return true;
    }

    QString filePath(HANDLE file)
    {
        DWORD size = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (size == 0 || size > 32768) return {};
        std::vector<wchar_t> storage(size + 1);
        const DWORD written = GetFinalPathNameByHandleW(file, storage.data(), DWORD(storage.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        return written > 0 && written < storage.size() ? QString::fromWCharArray(storage.data()) : QString();
    }

    bool localDiskPath(const QString& value)
    {
        QString path = value;
        if (path.startsWith(QStringLiteral("\\\\?\\"))) path = path.mid(4);
        if (path.size() < 3 || !path[0].isLetter() || path[1] != QLatin1Char(':')
            || (path[2] != QLatin1Char('\\') && path[2] != QLatin1Char('/')))
            return false;
        // Named streams and device names can have object-specific side effects when opened.
        if (path.mid(2).contains(QLatin1Char(':'))) return false;
        const QStringList components = QString(path.mid(3)).replace(QLatin1Char('/'), QLatin1Char('\\'))
            .split(QLatin1Char('\\'));
        for (const QString& component : components)
        {
            const QString name = component.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
            if (name == QStringLiteral("CON") || name == QStringLiteral("PRN")
                || name == QStringLiteral("AUX") || name == QStringLiteral("NUL")) return false;
            if (name.size() == 4 && (name.startsWith(QStringLiteral("COM")) || name.startsWith(QStringLiteral("LPT")))
                && (name[3].isDigit() || name[3] == QChar(0x00b9)
                    || name[3] == QChar(0x00b2) || name[3] == QChar(0x00b3))) return false;
        }
        const std::wstring root = (path.left(2) + QLatin1Char('\\')).toStdWString();
        const UINT type = GetDriveTypeW(root.c_str());
        return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK || type == DRIVE_CDROM;
    }

    struct RegistryTarget { HKEY root = nullptr; QString subkey; };

    RegistryTarget registryTarget(const QString& input, const QString& userSid)
    {
        const QString normalized = QString(input).replace(QLatin1Char('/'), QLatin1Char('\\'));
        const int separator = normalized.indexOf(QLatin1Char('\\'));
        const QString prefix = (separator < 0 ? normalized : normalized.left(separator)).toUpper();
        QString subkey = separator < 0 ? QString() : normalized.mid(separator + 1);
        if (prefix == QStringLiteral("HKLM") || prefix == QStringLiteral("HKEY_LOCAL_MACHINE"))
            return {HKEY_LOCAL_MACHINE, subkey};
        if (prefix == QStringLiteral("HKU") || prefix == QStringLiteral("HKEY_USERS"))
            return {HKEY_USERS, subkey};
        if ((prefix == QStringLiteral("HKCU") || prefix == QStringLiteral("HKEY_CURRENT_USER"))
            && !userSid.isEmpty())
            return {HKEY_USERS, subkey.isEmpty() ? userSid : userSid + QLatin1Char('\\') + subkey};
        return {};
    }

    QString registryPath(HKEY key)
    {
        // NtQueryKey KeyNameInformation supplies the resolved native path, including symbolic links.
        using QueryKey = LONG (NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        QueryKey function = nullptr;
        const FARPROC address = ntdll != nullptr ? GetProcAddress(ntdll, "NtQueryKey") : nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        if (function == nullptr) return {};
        ULONG size = 0;
        function(key, 3, nullptr, 0, &size);
        if (size < sizeof(ULONG) || size > 1024 * 1024) return {};
        std::vector<BYTE> bytes(size);
        if (function(key, 3, bytes.data(), size, &size) < 0) return {};
        ULONG nameBytes = 0;
        std::memcpy(&nameBytes, bytes.data(), sizeof(nameBytes));
        if (nameBytes > bytes.size() - sizeof(ULONG) || nameBytes % sizeof(wchar_t) != 0) return {};
        return QString::fromWCharArray(reinterpret_cast<const wchar_t*>(bytes.data() + sizeof(ULONG)),
            int(nameBytes / sizeof(wchar_t)));
    }

    QString sddl(PSECURITY_DESCRIPTOR descriptor, const SECURITY_INFORMATION information)
    {
        LPWSTR storage = nullptr;
        if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1,
            information, &storage, nullptr)) return {};
        const QString result = QString::fromWCharArray(storage);
        LocalFree(storage);
        return result;
    }

    DWORD readDescriptor(const Anchor& anchor, Descriptor& descriptor)
    {
        return GetSecurityInfo(anchor.objectHandle(), anchor.objectType(),
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, nullptr, &descriptor.value);
    }

    DWORD readLabel(const Anchor& anchor, Descriptor& descriptor)
    {
        return GetSecurityInfo(anchor.objectHandle(), anchor.objectType(), LABEL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, nullptr, &descriptor.value);
    }

    void collectTokenEvidence(const Anchor& anchor, Result& result, std::vector<SidEvidence>& sids)
    {
        std::vector<BYTE> bytes;
        if (queryTokenBytes(anchor.token.value, TokenUser, bytes))
        {
            const auto sid = reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid;
            result.userSid = sidText(sid);
            result.user = sidAccount(sid);
            sids.push_back({result.userSid, SE_GROUP_ENABLED, true, false});
        }
        else result.sidEvidenceComplete = false;
        for (const auto type : {TokenGroups, TokenRestrictedSids})
        {
            if (!queryTokenBytes(anchor.token.value, type, bytes))
            {
                result.sidEvidenceComplete = false;
                continue;
            }
            const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(bytes.data());
            const size_t capacity = (bytes.size() - offsetof(TOKEN_GROUPS, Groups)) / sizeof(SID_AND_ATTRIBUTES);
            if (groups->GroupCount > capacity)
            {
                result.sidEvidenceComplete = false;
                continue;
            }
            for (DWORD index = 0; index < groups->GroupCount; ++index)
            {
                const auto& group = groups->Groups[index];
                const bool restricted = type == TokenRestrictedSids;
                sids.push_back({sidText(group.Sid), group.Attributes, false, restricted});
                if (restricted) ++result.restrictedGroups;
                else
                {
                    ++result.groups;
                    if (group.Attributes & SE_GROUP_USE_FOR_DENY_ONLY) ++result.denyOnlyGroups;
                }
            }
        }
        if (queryTokenBytes(anchor.token.value, TokenIntegrityLevel, bytes))
        {
            const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(bytes.data())->Label.Sid;
            if (IsValidSid(sid) && *GetSidSubAuthorityCount(sid) > 0)
            {
                result.tokenRid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
                result.tokenIntegrityKnown = true;
            }
        }
        TOKEN_MANDATORY_POLICY policy{};
        DWORD size = sizeof(policy);
        result.tokenPolicyKnown = GetTokenInformation(anchor.token.value, TokenMandatoryPolicy,
            &policy, size, &size) != FALSE;
        result.tokenPolicy = policy.Policy;
        size = sizeof(result.appContainer);
        result.appContainerKnown = GetTokenInformation(anchor.token.value, TokenIsAppContainer,
            &result.appContainer, size, &size) != FALSE;
    }

    bool openTarget(Anchor& anchor, Result& result)
    {
        const auto& request = anchor.request;
        if (request.kind == ObjectKind::File)
        {
            if (!localDiskPath(request.path))
            {
                result.stage = Stage::InvalidTarget;
                result.error = ERROR_INVALID_NAME;
                return false;
            }
            const auto path = request.path.toStdWString();
            anchor.file = CreateFileW(path.c_str(), READ_CONTROL,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (anchor.file != INVALID_HANDLE_VALUE)
            {
                if (GetFileType(anchor.file) != FILE_TYPE_DISK)
                {
                    result.stage = Stage::InvalidTarget;
                    result.error = ERROR_INVALID_NAME;
                    return false;
                }
                anchor.canonicalPath = filePath(anchor.file);
                anchor.fileIdentityKnown = GetFileInformationByHandleEx(anchor.file, FileIdentityClass,
                    &anchor.fileIdentity, sizeof(anchor.fileIdentity)) != FALSE;
                if (anchor.canonicalPath.isEmpty() || !localDiskPath(anchor.canonicalPath))
                {
                    result.stage = Stage::TargetIdentity;
                    result.error = ERROR_NOT_SUPPORTED;
                    return false;
                }
            }
            else result.error = GetLastError();
        }
        else if (request.kind == ObjectKind::Registry)
        {
            const auto parsed = registryTarget(request.path, result.userSid);
            if (parsed.root == nullptr)
            {
                result.stage = Stage::InvalidTarget;
                result.error = ERROR_INVALID_NAME;
                return false;
            }
            const auto subkey = parsed.subkey.toStdWString();
            result.error = RegOpenKeyExW(parsed.root, subkey.c_str(), 0,
                READ_CONTROL | request.registryView, &anchor.key);
            if (result.error == ERROR_SUCCESS)
            {
                anchor.canonicalPath = registryPath(anchor.key);
                if (anchor.canonicalPath.isEmpty())
                {
                    result.stage = Stage::TargetIdentity;
                    result.error = ERROR_NOT_SUPPORTED;
                    return false;
                }
            }
        }
        else
        {
            if (request.path.isEmpty() || request.path.contains(QLatin1Char('\\'))
                || request.path.contains(QLatin1Char('/')))
            {
                result.stage = Stage::InvalidTarget;
                result.error = ERROR_INVALID_NAME;
                return false;
            }
            anchor.manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (anchor.manager != nullptr)
            {
                const auto name = request.path.toStdWString();
                anchor.service = OpenServiceW(anchor.manager, name.c_str(), READ_CONTROL);
            }
            if (anchor.service == nullptr) result.error = GetLastError();
            else anchor.canonicalPath = request.path;
        }
        if (result.error != ERROR_SUCCESS)
        {
            result.stage = Stage::Target;
            return false;
        }
        result.canonicalPath = anchor.canonicalPath;
        return true;
    }

    Result diagnose(const Request& request, const std::shared_ptr<std::atomic_bool>& cancellation)
    {
        Result result;
        result.request = request;
        auto anchor = std::make_shared<Anchor>();
        anchor->request = request;
        if (isCancelled(cancellation, result)) return result;
        anchor->process.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, request.pid);
        if (anchor->process.value == nullptr)
        {
            result.stage = Stage::Process;
            result.error = GetLastError();
            return result;
        }
        if (!creationTime(anchor->process.value, anchor->creation)
            || WaitForSingleObject(anchor->process.value, 0) != WAIT_TIMEOUT)
        {
            result.stage = Stage::ProcessIdentity;
            result.error = ERROR_RETRY;
            return result;
        }
        wchar_t image[32768]{};
        DWORD imageSize = DWORD(std::size(image));
        if (QueryFullProcessImageNameW(anchor->process.value, 0, image, &imageSize))
            result.processPath = QString::fromWCharArray(image, int(imageSize));
        if (!OpenProcessToken(anchor->process.value, TOKEN_QUERY | TOKEN_DUPLICATE, &anchor->token.value))
        {
            result.stage = Stage::Token;
            result.error = GetLastError();
            return result;
        }
        if (!queryStatistics(anchor->token.value, anchor->statistics))
        {
            result.stage = Stage::TokenIdentity;
            result.error = GetLastError();
            return result;
        }
        if (!DuplicateTokenEx(anchor->token.value, TOKEN_QUERY | TOKEN_IMPERSONATE,
            nullptr, SecurityImpersonation, TokenImpersonation, &anchor->impersonation.value))
        {
            result.stage = Stage::Duplicate;
            result.error = GetLastError();
            return result;
        }
        std::vector<SidEvidence> sids;
        collectTokenEvidence(*anchor, result, sids);
        if (isCancelled(cancellation, result)) return result;
        if (!openTarget(*anchor, result)) return result;
        Descriptor descriptor;
        result.error = readDescriptor(*anchor, descriptor);
        if (result.error != ERROR_SUCCESS || descriptor.value == nullptr)
        {
            result.stage = Stage::Descriptor;
            return result;
        }
        PSID owner = nullptr;
        PSID group = nullptr;
        BOOL defaulted = FALSE;
        GetSecurityDescriptorOwner(descriptor.value, &owner, &defaulted);
        GetSecurityDescriptorGroup(descriptor.value, &group, &defaulted);
        result.ownerSid = sidText(owner);
        result.owner = sidAccount(owner);
        result.groupSid = sidText(group);
        result.descriptorSddl = sddl(descriptor.value,
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION);
        if (result.descriptorSddl.isEmpty())
        {
            result.stage = Stage::Descriptor;
            result.error = ERROR_INVALID_SECURITY_DESCR;
            return result;
        }
        anchor->descriptorSddl = result.descriptorSddl;
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        GetSecurityDescriptorControl(descriptor.value, &control, &revision);
        result.daclProtected = (control & SE_DACL_PROTECTED) != 0;
        PACL dacl = nullptr;
        BOOL present = FALSE;
        GetSecurityDescriptorDacl(descriptor.value, &present, &dacl, &defaulted);
        result.daclPresent = present != FALSE;
        result.nullDacl = !result.daclPresent || dacl == nullptr;
        if (dacl != nullptr)
        {
            const DWORD descriptorSize = GetSecurityDescriptorLength(descriptor.value);
            const size_t offset = reinterpret_cast<const BYTE*>(dacl)
                - static_cast<const BYTE*>(descriptor.value);
            const auto view = decodeAcl(dacl, offset <= descriptorSize ? descriptorSize - offset : 0);
            result.aclValid = view.valid;
            result.complex = view.complex;
            for (const auto& ace : view.aces)
            {
                if (isCancelled(cancellation, result)) return result;
                AceEvidence row;
                row.ace = ace;
                if (!ace.sid.empty())
                {
                    const auto sid = const_cast<BYTE*>(ace.sid.data());
                    row.sid = sidText(sid);
                    row.account = sidAccount(sid);
                    for (const auto& evidence : sids)
                        if (evidence.sid == row.sid) row.matches.push_back(evidence);
                }
                result.aces.push_back(std::move(row));
            }
        }
        if (isCancelled(cancellation, result)) return result;
        result.native = checkDescriptor(descriptor.value, anchor->impersonation.value,
            request.kind, request.desired);
        result.authz = checkDescriptorAuthz(descriptor.value, anchor->token.value,
            request.kind, request.desired);
        for (const auto& privilege : result.native.usedPrivileges)
        {
            DWORD size = 0;
            LUID luid = privilege.Luid;
            LookupPrivilegeNameW(nullptr, &luid, nullptr, &size);
            if (size == 0 || size > 32768) continue;
            std::vector<wchar_t> name(size + 1);
            if (LookupPrivilegeNameW(nullptr, &luid, name.data(), &size))
                result.usedPrivilegeNames.push_back(QString::fromWCharArray(name.data(), int(size)));
        }
        Descriptor label;
        result.labelError = readLabel(*anchor, label);
        anchor->labelError = result.labelError;
        if (result.labelError == ERROR_SUCCESS && label.value != nullptr)
        {
            result.labelKnown = true;
            result.labelRid = SECURITY_MANDATORY_MEDIUM_RID;
            result.labelPolicy = SYSTEM_MANDATORY_LABEL_NO_WRITE_UP;
            result.labelSddl = sddl(label.value, LABEL_SECURITY_INFORMATION);
            anchor->labelSddl = result.labelSddl;
            PACL sacl = nullptr;
            BOOL saclPresent = FALSE;
            if (!GetSecurityDescriptorSacl(label.value, &saclPresent, &sacl, &defaulted))
                result.labelKnown = false;
            if (saclPresent && sacl != nullptr)
            {
                const DWORD descriptorSize = GetSecurityDescriptorLength(label.value);
                const size_t offset = reinterpret_cast<const BYTE*>(sacl) - static_cast<const BYTE*>(label.value);
                const auto view = decodeAcl(sacl, offset <= descriptorSize ? descriptorSize - offset : 0);
                if (!view.valid) result.labelKnown = false;
                for (const auto& ace : view.aces)
                {
                    if (ace.type != SYSTEM_MANDATORY_LABEL_ACE_TYPE || ace.sid.empty()) continue;
                    const auto sid = const_cast<BYTE*>(ace.sid.data());
                    const SID_IDENTIFIER_AUTHORITY mandatory = SECURITY_MANDATORY_LABEL_AUTHORITY;
                    if (*GetSidSubAuthorityCount(sid) == 0
                        || std::memcmp(GetSidIdentifierAuthority(sid), &mandatory, sizeof(mandatory)) != 0)
                    {
                        result.labelKnown = false;
                        continue;
                    }
                    if (result.labelExplicit) result.labelKnown = false;
                    result.labelExplicit = true;
                    result.labelRid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
                    result.labelPolicy = ace.mask;
                    if (ace.flags & INHERIT_ONLY_ACE) result.labelKnown = false;
                }
            }
        }
        if (!result.labelKnown && result.labelError == ERROR_SUCCESS)
        {
            result.labelError = ERROR_INVALID_SECURITY_DESCR;
            // Keep the descriptor-read status separately for snapshot verification.
        }
        if (isCancelled(cancellation, result)) return result;
        TOKEN_STATISTICS finalStatistics{};
        if (!queryStatistics(anchor->token.value, finalStatistics)
            || !sameStatistics(finalStatistics, anchor->statistics))
        {
            result.stage = Stage::TokenIdentity;
            result.error = ERROR_RETRY;
            return result;
        }
        if (WaitForSingleObject(anchor->process.value, 0) != WAIT_TIMEOUT)
        {
            result.stage = Stage::ProcessIdentity;
            result.error = ERROR_RETRY;
            return result;
        }
        result.anchor = std::move(anchor);
        return result;
    }

    bool validateAnchor(const Anchor& anchor, Result& result)
    {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, anchor.request.pid));
        ULONGLONG creation = 0;
        if (process.value == nullptr || !creationTime(process.value, creation)
            || creation != anchor.creation || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
        {
            result.stage = Stage::ProcessIdentity;
            result.error = ERROR_RETRY;
            return false;
        }
        Handle token;
        TOKEN_STATISTICS stats{};
        TOKEN_STATISTICS retainedStats{};
        if (!OpenProcessToken(process.value, TOKEN_QUERY, &token.value)
            || !queryStatistics(token.value, stats) || !sameStatistics(stats, anchor.statistics)
            || !queryStatistics(anchor.token.value, retainedStats)
            || !sameStatistics(retainedStats, anchor.statistics))
        {
            result.stage = Stage::TokenIdentity;
            result.error = ERROR_RETRY;
            return false;
        }
        if (anchor.request.kind == ObjectKind::File)
        {
            FileIdentity identity{};
            if (!anchor.fileIdentityKnown || !GetFileInformationByHandleEx(anchor.file, FileIdentityClass,
                &identity, sizeof(identity))
                || std::memcmp(&identity, &anchor.fileIdentity, sizeof(identity)) != 0
                || filePath(anchor.file) != anchor.canonicalPath)
            {
                result.stage = Stage::TargetIdentity;
                result.error = ERROR_RETRY;
                return false;
            }
        }
        if (anchor.request.kind == ObjectKind::Registry && registryPath(anchor.key) != anchor.canonicalPath)
        {
            result.stage = Stage::TargetIdentity;
            result.error = ERROR_RETRY;
            return false;
        }
        Descriptor descriptor;
        const DWORD error = readDescriptor(anchor, descriptor);
        if (error != ERROR_SUCCESS || descriptor.value == nullptr
            || sddl(descriptor.value, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION
                | DACL_SECURITY_INFORMATION) != anchor.descriptorSddl)
        {
            result.stage = Stage::Descriptor;
            result.error = error != ERROR_SUCCESS ? error : ERROR_RETRY;
            return false;
        }
        Descriptor label;
        const DWORD labelError = readLabel(anchor, label);
        if (labelError != anchor.labelError || (labelError == ERROR_SUCCESS
            && (label.value == nullptr || sddl(label.value, LABEL_SECURITY_INFORMATION) != anchor.labelSddl)))
        {
            result.stage = Stage::Label;
            result.error = ERROR_RETRY;
            return false;
        }
        return true;
    }

    class ScopedImpersonation
    {
    public:
        explicit ScopedImpersonation(HANDLE token)
        {
            m_active = ImpersonateLoggedOnUser(token) != FALSE;
            m_error = m_active ? ERROR_SUCCESS : GetLastError();
        }
        ~ScopedImpersonation() { if (m_active) RevertToSelf(); }
        bool active() const { return m_active; }
        DWORD error() const { return m_error; }
        bool revert()
        {
            if (!m_active) return true;
            if (RevertToSelf()) { m_active = false; return true; }
            m_error = GetLastError();
            // A second cleanup attempt is contained to the disposable probe thread.
            if (SetThreadToken(nullptr, nullptr)) { m_active = false; return true; }
            return false;
        }
    private:
        bool m_active = false;
        DWORD m_error = ERROR_SUCCESS;
    };

    Result probeOnDedicatedThread(const Result& baseline,
        const std::shared_ptr<std::atomic_bool>& cancellation)
    {
        Result result = baseline;
        result.probe = true;
        result.probeOpened = false;
        result.probeIdentityVerified = false;
        result.probeError = ERROR_SUCCESS;
        result.stage = Stage::None;
        if (isCancelled(cancellation, result) || result.anchor == nullptr) return result;
        const auto& anchor = *result.anchor;
        if (!validateAnchor(anchor, result)) return result;
        if (isCancelled(cancellation, result)) return result;
        ScopedImpersonation impersonation(anchor.impersonation.value);
        if (!impersonation.active())
        {
            result.stage = Stage::Impersonate;
            result.error = impersonation.error();
            return result;
        }
        // The only object action under impersonation is OPEN_EXISTING/open-key/open-service.
        // No read/write/set-security, disposition, start/stop or deletion operation is issued.
        if (anchor.request.kind == ObjectKind::File)
        {
            const auto path = anchor.request.path.toStdWString();
            Handle handle(CreateFileW(path.c_str(), anchor.request.desired,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            result.probeOpened = handle.value != INVALID_HANDLE_VALUE;
            result.probeError = result.probeOpened ? ERROR_SUCCESS : GetLastError();
            if (result.probeOpened)
            {
                FileIdentity identity{};
                result.probeIdentityVerified = GetFileInformationByHandleEx(handle.value, FileIdentityClass,
                    &identity, sizeof(identity)) != FALSE
                    && std::memcmp(&identity, &anchor.fileIdentity, sizeof(identity)) == 0
                    && filePath(handle.value) == anchor.canonicalPath;
            }
        }
        else if (anchor.request.kind == ObjectKind::Registry)
        {
            const auto parsed = registryTarget(anchor.request.path, result.userSid);
            const auto subkey = parsed.subkey.toStdWString();
            KeyHandle key;
            result.probeError = RegOpenKeyExW(parsed.root, subkey.c_str(), 0,
                anchor.request.desired | anchor.request.registryView, &key.value);
            result.probeOpened = result.probeError == ERROR_SUCCESS;
            if (key.value != nullptr)
            {
                result.probeIdentityVerified = registryPath(key.value) == anchor.canonicalPath;
            }
        }
        else
        {
            // SCM connection rights are checked in the subject's context too.
            ServiceHandle manager;
            manager.value = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (manager.value != nullptr)
            {
                const auto name = anchor.request.path.toStdWString();
                ServiceHandle service;
                service.value = OpenServiceW(manager.value, name.c_str(), anchor.request.desired);
                result.probeOpened = service.value != nullptr;
                result.probeError = result.probeOpened ? ERROR_SUCCESS : GetLastError();
                // The retained service handle prevents deletion/recreation of this name.
                result.probeIdentityVerified = result.probeOpened && anchor.service != nullptr;
            }
            else result.probeError = GetLastError();
        }
        result.reverted = impersonation.revert();
        if (!result.reverted)
        {
            result.stage = Stage::Revert;
            result.error = impersonation.error();
            return result;
        }
        if (isCancelled(cancellation, result)) return result;
        // Refresh checks after closing the probe handle catch concurrent identity/ACL changes.
        if (!validateAnchor(anchor, result)) result.probeIdentityVerified = false;
        return result;
    }

    QString stageText(const Stage stage)
    {
        switch (stage)
        {
        case Stage::Cancelled: return text("privilege.workbench.access.stage.cancelled", "已取消");
        case Stage::Process: return text("privilege.workbench.access.stage.process", "打开目标进程");
        case Stage::ProcessIdentity: return text("privilege.workbench.access.stage.process_identity", "核验进程创建时间与存活状态");
        case Stage::Token: return text("privilege.workbench.access.stage.token", "读取进程主令牌");
        case Stage::TokenIdentity: return text("privilege.workbench.access.stage.token_identity", "核验令牌身份与修改序列");
        case Stage::Duplicate: return text("privilege.workbench.access.stage.duplicate", "复制模拟令牌");
        case Stage::InvalidTarget: return text("privilege.workbench.access.stage.invalid_target", "校验本地对象名称");
        case Stage::Target: return text("privilege.workbench.access.stage.target", "以 KSword 身份读取对象");
        case Stage::TargetIdentity: return text("privilege.workbench.access.stage.target_identity", "核验对象身份与规范路径");
        case Stage::Descriptor: return text("privilege.workbench.access.stage.descriptor", "读取或核验安全描述符");
        case Stage::Label: return text("privilege.workbench.access.stage.label", "核验完整性标签");
        case Stage::Impersonate: return text("privilege.workbench.access.stage.impersonate", "模拟目标进程主令牌");
        case Stage::Revert: return text("privilege.workbench.access.stage.revert", "恢复诊断线程身份");
        case Stage::Internal: return text("privilege.workbench.access.stage.internal", "后台诊断");
        default: return {};
        }
    }

    QString errorText(const DWORD error)
    {
        LPWSTR storage = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reinterpret_cast<LPWSTR>(&storage), 0, nullptr);
        const QString message = length != 0 ? QString::fromWCharArray(storage, int(length)).trimmed() : QString();
        if (storage != nullptr) LocalFree(storage);
        return message.isEmpty() ? QString::number(error) : QString::number(error) + QStringLiteral(" — ") + message;
    }

    QString verdict(const NativeCheck& result)
    {
        if (!result.succeeded) return text("privilege.workbench.access.unavailable", "无法评估");
        return result.allowed ? text("privilege.workbench.access.allowed", "描述符允许")
            : text("privilege.workbench.access.denied", "描述符拒绝");
    }

    QString aceType(const AceView& ace)
    {
        if (isAllowType(ace.type)) return ace.complex
            ? text("privilege.workbench.access.ace.allow_complex", "允许（对象或条件 ACE）")
            : text("privilege.workbench.access.ace.allow", "允许");
        if (isDenyType(ace.type)) return ace.complex
            ? text("privilege.workbench.access.ace.deny_complex", "拒绝（对象或条件 ACE）")
            : text("privilege.workbench.access.ace.deny", "拒绝");
        return text("privilege.workbench.access.ace.other", "其他 ACE，类型 %1").arg(ace.type);
    }

    QString maskText(const ObjectKind kind, DWORD mask)
    {
        QStringList parts;
        const auto append = [&](const DWORD bit, const char* name)
        {
            if ((mask & bit) != 0) { parts.push_back(QString::fromLatin1(name)); mask &= ~bit; }
        };
        append(GENERIC_READ, "GENERIC_READ");
        append(GENERIC_WRITE, "GENERIC_WRITE");
        append(GENERIC_EXECUTE, "GENERIC_EXECUTE");
        append(GENERIC_ALL, "GENERIC_ALL");
        append(MAXIMUM_ALLOWED, "MAXIMUM_ALLOWED");
        append(DELETE, "DELETE");
        append(READ_CONTROL, "READ_CONTROL");
        append(WRITE_DAC, "WRITE_DAC");
        append(WRITE_OWNER, "WRITE_OWNER");
        append(SYNCHRONIZE, "SYNCHRONIZE");
        append(ACCESS_SYSTEM_SECURITY, "ACCESS_SYSTEM_SECURITY");
        if (kind == ObjectKind::File)
        {
            append(FILE_READ_DATA, "FILE_READ_DATA / LIST_DIRECTORY");
            append(FILE_WRITE_DATA, "FILE_WRITE_DATA / ADD_FILE");
            append(FILE_APPEND_DATA, "FILE_APPEND_DATA / ADD_SUBDIRECTORY");
            append(FILE_READ_EA, "FILE_READ_EA");
            append(FILE_WRITE_EA, "FILE_WRITE_EA");
            append(FILE_EXECUTE, "FILE_EXECUTE / TRAVERSE");
            append(FILE_DELETE_CHILD, "FILE_DELETE_CHILD");
            append(FILE_READ_ATTRIBUTES, "FILE_READ_ATTRIBUTES");
            append(FILE_WRITE_ATTRIBUTES, "FILE_WRITE_ATTRIBUTES");
        }
        else if (kind == ObjectKind::Registry)
        {
            append(KEY_QUERY_VALUE, "KEY_QUERY_VALUE");
            append(KEY_SET_VALUE, "KEY_SET_VALUE");
            append(KEY_CREATE_SUB_KEY, "KEY_CREATE_SUB_KEY");
            append(KEY_ENUMERATE_SUB_KEYS, "KEY_ENUMERATE_SUB_KEYS");
            append(KEY_NOTIFY, "KEY_NOTIFY");
            append(KEY_CREATE_LINK, "KEY_CREATE_LINK");
        }
        else
        {
            append(SERVICE_QUERY_CONFIG, "SERVICE_QUERY_CONFIG");
            append(SERVICE_CHANGE_CONFIG, "SERVICE_CHANGE_CONFIG");
            append(SERVICE_QUERY_STATUS, "SERVICE_QUERY_STATUS");
            append(SERVICE_ENUMERATE_DEPENDENTS, "SERVICE_ENUMERATE_DEPENDENTS");
            append(SERVICE_START, "SERVICE_START");
            append(SERVICE_STOP, "SERVICE_STOP");
            append(SERVICE_PAUSE_CONTINUE, "SERVICE_PAUSE_CONTINUE");
            append(SERVICE_INTERROGATE, "SERVICE_INTERROGATE");
            append(SERVICE_USER_DEFINED_CONTROL, "SERVICE_USER_DEFINED_CONTROL");
        }
        if (mask != 0) parts.push_back(hex(mask));
        return parts.isEmpty() ? QStringLiteral("0") : parts.join(QStringLiteral(" | "));
    }

    QString aceFlags(const AceView& ace)
    {
        QStringList flags;
        if (ace.flags & INHERITED_ACE) flags << text("privilege.workbench.access.flag.inherited", "继承");
        else flags << text("privilege.workbench.access.flag.explicit", "显式");
        if (ace.flags & INHERIT_ONLY_ACE) flags << text("privilege.workbench.access.flag.inherit_only", "仅对子对象生效");
        if (ace.flags & OBJECT_INHERIT_ACE) flags << QStringLiteral("OI");
        if (ace.flags & CONTAINER_INHERIT_ACE) flags << QStringLiteral("CI");
        if (ace.flags & NO_PROPAGATE_INHERIT_ACE) flags << QStringLiteral("NP");
        if (ace.malformed) flags << text("privilege.workbench.access.flag.malformed", "格式无效");
        return flags.join(QStringLiteral(" | "));
    }

    QString matchesText(const AceEvidence& row, const Result& result)
    {
        QStringList matches;
        for (const auto& match : row.matches)
        {
            if (match.user) matches << text("privilege.workbench.access.match.user", "用户 SID");
            else if (match.restricted) matches << text("privilege.workbench.access.match.restricted", "限制 SID");
            else if (match.attributes & SE_GROUP_USE_FOR_DENY_ONLY)
                matches << text("privilege.workbench.access.match.deny_only", "仅拒绝组");
            else if (match.attributes & SE_GROUP_ENABLED)
                matches << text("privilege.workbench.access.match.enabled", "已启用组");
            else matches << text("privilege.workbench.access.match.disabled", "未启用组");
        }
        if (matches.isEmpty()) matches << text("privilege.workbench.access.match.none", "未匹配用户或常规组");
        const DWORD overlap = mappedAccess(result.request.kind, row.ace.mask)
            & mappedAccess(result.request.kind, result.request.desired);
        if (overlap != 0) matches << text("privilege.workbench.access.match.overlap", "请求交集 %1").arg(hex(overlap));
        if (row.ace.complex) matches << text("privilege.workbench.access.match.limited", "需要对象层级或条件上下文");
        return matches.join(QStringLiteral(" | "));
    }

    QString reportText(const Result& result)
    {
        QStringList lines;
        if (result.stage != Stage::None)
        {
            lines << text("privilege.workbench.access.report.failure", "阶段：%1\n错误：%2")
                .arg(stageText(result.stage), errorText(result.error));
            if (result.error == ERROR_RETRY)
                lines << text("privilege.workbench.access.report.stale", "进程、令牌、对象或描述符已变化，请重新评估。");
            if (!result.probe) return lines.join(QLatin1Char('\n'));
        }
        lines << text("privilege.workbench.access.report.subject", "主体：PID %1；%2\n用户：%3；%4")
            .arg(result.request.pid).arg(result.processPath, result.user, result.userSid);
        if (result.anchor != nullptr)
            lines << text("privilege.workbench.access.report.identity", "进程创建时间：%1；令牌 ID：%2:%3；修改序列：%4:%5")
                .arg(result.anchor->creation).arg(result.anchor->statistics.TokenId.HighPart)
                .arg(result.anchor->statistics.TokenId.LowPart)
                .arg(result.anchor->statistics.ModifiedId.HighPart)
                .arg(result.anchor->statistics.ModifiedId.LowPart);
        lines << text("privilege.workbench.access.report.groups", "常规组：%1；仅拒绝组：%2；限制 SID：%3")
            .arg(result.groups).arg(result.denyOnlyGroups).arg(result.restrictedGroups);
        if (!result.sidEvidenceComplete)
            lines << text("privilege.workbench.access.report.sids_incomplete", "部分 SID 证据读取失败；组计数及 ACE 匹配仅反映成功读取的部分，不能把未匹配解释为不属于令牌。");
        lines << text("privilege.workbench.access.report.target", "对象：%1\n规范路径：%2\n所有者：%3；%4\n主要组 SID：%5")
            .arg(result.request.path, result.canonicalPath, result.owner, result.ownerSid, result.groupSid);
        lines << text("privilege.workbench.access.report.request", "请求：%1 → %2\n具体权限：%3")
            .arg(hex(result.request.desired), hex(mappedAccess(result.request.kind, result.request.desired)),
                maskText(result.request.kind, mappedAccess(result.request.kind, result.request.desired)));
        if (result.nullDacl)
            lines << text("privilege.workbench.access.report.null_dacl", "DACL 未设置或为 NULL：不以 DACL 限制访问；仍受令牌限制、完整性及对象机制影响。");
        else if (result.aces.empty())
            lines << text("privilege.workbench.access.report.empty_dacl", "空 DACL：没有允许 ACE；所有者隐含权利或已启用特权仍可能影响特定请求。");
        else
            lines << text("privilege.workbench.access.report.dacl", "DACL：%1 条 ACE；保护继承：%2；格式：%3")
                .arg(result.aces.size()).arg(result.daclProtected ? QStringLiteral("1") : QStringLiteral("0"),
                    result.aclValid ? text("privilege.workbench.access.valid", "有效") : text("privilege.workbench.access.invalid", "无效"));
        lines << text("privilege.workbench.access.report.native", "AccessCheck：%1；授予掩码 %2；状态 %3")
            .arg(verdict(result.native), hex(result.native.granted), errorText(result.native.error));
        lines << text("privilege.workbench.access.report.authz", "AuthzAccessCheck：%1；授予掩码 %2；状态 %3")
            .arg(verdict(result.authz), hex(result.authz.granted), errorText(result.authz.error));
        if (result.native.succeeded && result.authz.succeeded && result.native.allowed != result.authz.allowed)
            lines << text("privilege.workbench.access.report.disagreement", "两种描述符评估结果不同：特权、受限令牌或条件上下文覆盖可能不同，不能合并为实际操作结论。");
        if (!result.usedPrivilegeNames.empty())
        {
            QStringList names;
            for (const auto& name : result.usedPrivilegeNames) names << name;
            lines << text("privilege.workbench.access.report.privileges", "AccessCheck 使用的令牌特权：%1").arg(names.join(QStringLiteral(", ")));
        }
        if (result.tokenIntegrityKnown)
            lines << text("privilege.workbench.access.report.token_integrity", "主体完整性 RID：%1；令牌强制策略：%2")
                .arg(hex(result.tokenRid), result.tokenPolicyKnown ? hex(result.tokenPolicy)
                    : text("privilege.workbench.access.unknown", "未知"));
        else lines << text("privilege.workbench.access.report.token_integrity_unknown", "主体完整性：未成功读取。");
        if (result.labelKnown)
        {
            lines << text("privilege.workbench.access.report.label", "对象完整性 RID：%1；标签策略：%2；来源：%3")
                .arg(hex(result.labelRid), hex(result.labelPolicy), result.labelExplicit
                    ? text("privilege.workbench.access.label.explicit", "显式强制标签")
                    : text("privilege.workbench.access.label.default", "未设置标签，按 Windows 默认中完整性解释"));
            if (result.tokenIntegrityKnown && result.tokenPolicyKnown)
            {
                const DWORD hint = integrityHintMask(result.request.kind, result.request.desired,
                    result.tokenRid, result.labelRid, result.labelPolicy, result.tokenPolicy);
                lines << (hint != 0
                    ? text("privilege.workbench.access.report.mic_hint", "完整性约束提示：请求与可能受向上访问限制的权限交集为 %1；这是按通用映射形成的证据提示，需实际打开验证。").arg(hex(hint))
                    : text("privilege.workbench.access.report.mic_no_hint", "完整性约束提示：已知标签和通用映射未发现向上访问交集；这不能证明实际操作成功。"));
            }
        }
        else lines << text("privilege.workbench.access.report.label_unknown", "对象完整性标签未知：%1；不能把读取失败解释为没有限制。").arg(errorText(result.labelError));
        if (result.complex)
            lines << text("privilege.workbench.access.report.complex", "包含对象、回调或其他复杂 ACE；未提供对象层级、资源属性或自定义回调，相关评估覆盖有限。");
        if (!result.appContainerKnown || result.appContainer != 0)
            lines << text("privilege.workbench.access.report.appcontainer", "AppContainer、能力 SID 和资源声明可能引入额外约束；常规 SID 匹配列仅用于解释证据。");
        lines << text("privilege.workbench.access.report.explanation", "ACE 表按原始顺序显示。SID 匹配和请求交集仅标注候选相关 ACE，不等同于完整决策轨迹；所有者、特权、仅拒绝组及限制 SID 由 Windows 评估接口处理。");
        lines << text("privilege.workbench.access.report.coverage", "描述符由 KSword 当前身份按固定对象句柄读取；主体是选定进程的主令牌，不包含该进程线程临时模拟身份。描述符评估不覆盖路径遍历、共享锁、文件系统过滤器、中央访问策略或后续操作条件。");
        if (!result.probe)
            lines << text("privilege.workbench.access.report.probe_not_run", "实际打开：尚未测试。可点击“测试实际打开”模拟主令牌，申请所选访问权并立即关闭句柄。");
        else if (result.stage != Stage::None)
            lines << text("privilege.workbench.access.report.probe_inconclusive", "实际打开测试未形成可确认的同一对象结论；请根据阶段错误重新评估。");
        else if (!result.probeOpened)
            lines << text("privilege.workbench.access.report.probe_failed", "实际打开：失败，%1。失败可来自对象权限、路径、共享模式、SCM 连接或其他系统约束。").arg(errorText(result.probeError));
        else if (!result.probeIdentityVerified)
            lines << text("privilege.workbench.access.report.probe_identity_unknown", "句柄打开成功，但对象身份无法核验或已变化；不能视为所评估对象的访问证明。");
        else lines << text("privilege.workbench.access.report.probe_opened", "实际打开：所选主令牌已获准打开同一对象并关闭句柄；这仅证明该时刻的句柄申请成功。");
        lines << text("privilege.workbench.access.report.no_mutation", "测试不执行写入、删除、修改 ACL 或服务控制；即使请求这些权限，也只申请并关闭句柄。未验证具体操作成功，并发变更仍可能发生。");
        lines << QStringLiteral("\nSDDL:\n") + result.descriptorSddl;
        if (!result.labelSddl.isEmpty()) lines << QStringLiteral("\nLABEL SDDL:\n") + result.labelSddl;
        return lines.join(QLatin1Char('\n'));
    }

    class AccessPage final : public QWidget
    {
    public:
        explicit AccessPage(QWidget* parent) : QWidget(parent)
        {
            setObjectName(QStringLiteral("privilege_access_page"));
            auto* layout = new QVBoxLayout(this);
            layout->setContentsMargins(8, 8, 8, 8);
            layout->setSpacing(6);
            auto* explanation = new QLabel(text("privilege.workbench.access.intro",
                "选择进程主令牌与对象，分别查看描述符评估、完整性约束和实际句柄打开结果。"), this);
            explanation->setWordWrap(true);
            layout->addWidget(explanation);

            auto* subjectRow = new QHBoxLayout;
            subjectRow->addWidget(new QLabel(text("privilege.workbench.access.pid", "进程 PID"), this));
            m_pid = new QLineEdit(QString::number(GetCurrentProcessId()), this);
            m_pid->setObjectName(QStringLiteral("privilege_access_pid"));
            m_pid->setMaximumWidth(150);
            subjectRow->addWidget(m_pid);
            auto* current = new QPushButton(text("privilege.workbench.access.current", "当前进程"), this);
            subjectRow->addWidget(current);
            subjectRow->addWidget(new QLabel(text("privilege.workbench.access.object_kind", "对象类型"), this));
            m_kind = new QComboBox(this);
            m_kind->setObjectName(QStringLiteral("privilege_access_kind"));
            m_kind->addItem(text("privilege.workbench.access.kind.file", "文件或目录"), int(ObjectKind::File));
            m_kind->addItem(text("privilege.workbench.access.kind.registry", "注册表项"), int(ObjectKind::Registry));
            m_kind->addItem(text("privilege.workbench.access.kind.service", "服务"), int(ObjectKind::Service));
            subjectRow->addWidget(m_kind);
            m_viewLabel = new QLabel(text("privilege.workbench.access.registry_view", "注册表视图"), this);
            m_view = new QComboBox(this);
            m_view->setObjectName(QStringLiteral("privilege_access_registry_view"));
            m_view->addItem(QStringLiteral("64-bit"), qulonglong(KEY_WOW64_64KEY));
            m_view->addItem(QStringLiteral("32-bit"), qulonglong(KEY_WOW64_32KEY));
            m_viewLabel->hide();
            m_view->hide();
            subjectRow->addStretch();
            layout->addLayout(subjectRow);

            auto* pathRow = new QHBoxLayout;
            pathRow->addWidget(new QLabel(text("privilege.workbench.access.path", "对象名称"), this));
            m_path = new QLineEdit(QCoreApplication::applicationFilePath(), this);
            m_path->setObjectName(QStringLiteral("privilege_access_path"));
            pathRow->addWidget(m_path, 1);
            m_browse = new QPushButton(text("privilege.workbench.access.browse", "选择文件"), this);
            pathRow->addWidget(m_browse);
            layout->addLayout(pathRow);
            m_pathHint = new QLabel(this);
            m_pathHint->setWordWrap(true);
            layout->addWidget(m_pathHint);
            updateKind();
            auto* viewRow = new QHBoxLayout;
            viewRow->addWidget(m_viewLabel);
            viewRow->addWidget(m_view);
            viewRow->addStretch();
            layout->addLayout(viewRow);

            auto* accessRow = new QHBoxLayout;
            accessRow->addWidget(new QLabel(text("privilege.workbench.access.operation", "请求权限"), this));
            m_preset = new QComboBox(this);
            m_preset->setObjectName(QStringLiteral("privilege_access_preset"));
            m_preset->addItem(text("privilege.workbench.access.preset.read", "读取（通用映射）"), qulonglong(GENERIC_READ));
            m_preset->addItem(text("privilege.workbench.access.preset.write", "写入（只评估）"), qulonglong(GENERIC_WRITE));
            m_preset->addItem(text("privilege.workbench.access.preset.execute", "执行或控制（只评估）"), qulonglong(GENERIC_EXECUTE));
            m_preset->addItem(text("privilege.workbench.access.preset.all", "全部访问（只评估）"), qulonglong(GENERIC_ALL));
            m_preset->addItem(text("privilege.workbench.access.preset.read_control", "读取安全描述符"), qulonglong(READ_CONTROL));
            m_preset->addItem(text("privilege.workbench.access.preset.delete", "删除权（只评估）"), qulonglong(DELETE));
            m_preset->addItem(text("privilege.workbench.access.preset.write_dac", "修改 DACL 权（只评估）"), qulonglong(WRITE_DAC));
            m_preset->addItem(text("privilege.workbench.access.preset.write_owner", "修改所有者权（只评估）"), qulonglong(WRITE_OWNER));
            m_preset->addItem(text("privilege.workbench.access.preset.custom", "自定义掩码"));
            accessRow->addWidget(m_preset);
            m_mask = new QLineEdit(hex(GENERIC_READ), this);
            m_mask->setObjectName(QStringLiteral("privilege_access_mask"));
            m_mask->setMaximumWidth(170);
            accessRow->addWidget(m_mask);
            accessRow->addStretch();
            layout->addLayout(accessRow);

            auto* actions = new QHBoxLayout;
            m_assess = new QPushButton(text("privilege.workbench.access.assess", "评估描述符"), this);
            m_assess->setObjectName(QStringLiteral("privilege_access_assess"));
            m_probe = new QPushButton(text("privilege.workbench.access.probe", "测试实际打开"), this);
            m_probe->setObjectName(QStringLiteral("privilege_access_probe"));
            m_probe->setEnabled(false);
            m_probe->setToolTip(text("privilege.workbench.access.probe_tooltip",
                "模拟所选主令牌，申请当前权限并立即关闭句柄；不执行写入、删除或服务控制。"));
            m_cancel = new QPushButton(text("privilege.workbench.access.cancel", "取消"), this);
            m_cancel->setEnabled(false);
            actions->addWidget(m_assess);
            actions->addWidget(m_probe);
            actions->addWidget(m_cancel);
            m_status = new QLabel(text("privilege.workbench.access.ready", "等待评估"), this);
            m_status->setWordWrap(true);
            actions->addWidget(m_status, 1);
            layout->addLayout(actions);

            auto* split = new QSplitter(Qt::Vertical, this);
            m_table = new ks::ui::VisibleTableWidget(split);
            m_table->setObjectName(QStringLiteral("privilege_access_aces"));
            m_table->setColumnCount(8);
            m_table->setHorizontalHeaderLabels({
                text("privilege.workbench.access.column.order", "顺序"),
                text("privilege.workbench.access.column.type", "ACE 类型"), QStringLiteral("SID"),
                text("privilege.workbench.access.column.account", "账户"),
                text("privilege.workbench.access.column.mask", "原始掩码"),
                text("privilege.workbench.access.column.rights", "映射后权限"),
                text("privilege.workbench.access.column.flags", "继承与范围"),
                text("privilege.workbench.access.column.match", "主体匹配与证据")});
            m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
            m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
            m_table->setAlternatingRowColors(true);
            m_table->horizontalHeader()->setStretchLastSection(true);
            m_table->setContextMenuPolicy(Qt::CustomContextMenu);
            m_report = new QPlainTextEdit(split);
            m_report->setObjectName(QStringLiteral("privilege_access_report"));
            m_report->setReadOnly(true);
            m_report->setPlainText(text("privilege.workbench.access.initial_report",
                "先评估描述符；实际打开测试需要当前评估快照，输入变化后必须重新评估。"));
            split->setStretchFactor(0, 3);
            split->setStretchFactor(1, 2);
            layout->addWidget(split, 1);

            connect(current, &QPushButton::clicked, this, [this]() { m_pid->setText(QString::number(GetCurrentProcessId())); });
            connect(m_browse, &QPushButton::clicked, this, [this]()
            {
                const QString chosen = QFileDialog::getOpenFileName(this,
                    text("privilege.workbench.access.choose_file", "选择要诊断的文件"), m_path->text());
                if (!chosen.isEmpty()) m_path->setText(chosen);
            });
            connect(m_pid, &QLineEdit::textChanged, this, [this]() { invalidate(); });
            connect(m_path, &QLineEdit::textChanged, this, [this]() { invalidate(); });
            connect(m_mask, &QLineEdit::textChanged, this, [this]() { invalidate(); });
            connect(m_view, &QComboBox::currentIndexChanged, this, [this]() { invalidate(); });
            connect(m_kind, &QComboBox::currentIndexChanged, this, [this]() { updateKind(); invalidate(); });
            connect(m_preset, &QComboBox::currentIndexChanged, this, [this]()
            {
                if (m_preset->currentData().isValid()) m_mask->setText(hex(DWORD(m_preset->currentData().toULongLong())));
                invalidate();
            });
            connect(m_assess, &QPushButton::clicked, this, [this]() { assess(); });
            connect(m_probe, &QPushButton::clicked, this, [this]() { probe(); });
            connect(m_cancel, &QPushButton::clicked, this, [this]()
            {
                invalidate();
                m_status->setText(text("privilege.workbench.access.cancelled", "已取消；后台系统调用返回后释放资源"));
            });
            connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& position)
            {
                const QModelIndex index = m_table->indexAt(position);
                if (!index.isValid()) return;
                auto* item = m_table->item(index.row(), 2);
                if (item == nullptr || item->text().isEmpty()) return;
                const QString sid = item->text();
                QMenu menu(this);
                auto* copy = menu.addAction(text("privilege.workbench.access.copy_sid", "复制 SID"));
                if (menu.exec(m_table->viewport()->mapToGlobal(position)) == copy && QApplication::clipboard() != nullptr)
                    QApplication::clipboard()->setText(sid);
            });
        }

        ~AccessPage() override
        {
            if (m_cancellation != nullptr) m_cancellation->store(true, std::memory_order_relaxed);
        }

    private:
        void updateKind()
        {
            const auto kind = ObjectKind(m_kind->currentData().toInt());
            m_browse->setEnabled(kind == ObjectKind::File);
            m_view->setVisible(kind == ObjectKind::Registry);
            m_viewLabel->setVisible(kind == ObjectKind::Registry);
            if (kind == ObjectKind::File)
                m_pathHint->setText(text("privilege.workbench.access.hint.file", "输入本地磁盘绝对路径；目录可直接输入。拒绝网络路径、设备路径和命名数据流。"));
            else if (kind == ObjectKind::Registry)
                m_pathHint->setText(text("privilege.workbench.access.hint.registry", "支持 HKLM、HKU 和 HKCU。HKCU 按选定进程用户 SID 解析到 HKU，需已加载该用户配置单元。"));
            else m_pathHint->setText(text("privilege.workbench.access.hint.service", "输入本地服务短名称（例如 Spooler），使用所选服务对象的权限映射。"));
        }

        void invalidate()
        {
            ++m_generation;
            if (m_cancellation != nullptr) m_cancellation->store(true, std::memory_order_relaxed);
            m_result.reset();
            m_assess->setEnabled(true);
            m_probe->setEnabled(false);
            m_cancel->setEnabled(false);
            m_table->setEnabled(false);
            m_table->clearContents();
            m_table->setRowCount(0);
            m_report->setPlainText(text("privilege.workbench.access.stale_report", "输入已变化；旧评估和打开结果已作废。请重新评估当前主体、对象和请求权限。"));
            m_status->setText(text("privilege.workbench.access.changed", "输入已变化；请重新评估"));
            ks::ui::ApplyStatusRole(m_status, ks::ui::StatusRole::Idle);
        }

        bool readRequest(Request& request)
        {
            bool pidValid = false;
            const qulonglong pid = m_pid->text().trimmed().toULongLong(&pidValid, 10);
            bool maskValid = false;
            const QString entered = m_mask->text().trimmed();
            const qulonglong mask = entered.toULongLong(&maskValid,
                entered.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10);
            request.kind = ObjectKind(m_kind->currentData().toInt());
            request.path = m_path->text().trimmed();
            if (!pidValid || pid == 0 || pid > MAXDWORD || !maskValid || mask == 0 || mask > MAXDWORD
                || request.path.isEmpty() || request.path.contains(QChar(0))
                || (request.kind == ObjectKind::Registry && (mask & (KEY_WOW64_32KEY | KEY_WOW64_64KEY)) != 0))
            {
                m_status->setText(text("privilege.workbench.access.validation", "请输入有效 PID、对象名称和非零 32 位掩码；注册表视图使用独立选项。"));
                ks::ui::ApplyStatusRole(m_status, ks::ui::StatusRole::Warning);
                return false;
            }
            request.pid = DWORD(pid);
            request.desired = DWORD(mask);
            request.registryView = DWORD(m_view->currentData().toULongLong());
            return true;
        }

        template<typename Worker>
        void start(Worker worker, const bool probeJob)
        {
            const quint64 generation = ++m_generation;
            m_cancellation = std::make_shared<std::atomic_bool>(false);
            const auto cancellation = m_cancellation;
            m_assess->setEnabled(false);
            m_probe->setEnabled(false);
            m_cancel->setEnabled(true);
            m_table->setEnabled(false);
            m_status->setText(probeJob ? text("privilege.workbench.access.running_probe", "正在核验快照并测试实际打开…")
                : text("privilege.workbench.access.running", "正在后台读取描述符与令牌…"));
            ks::ui::ApplyStatusRole(m_status, ks::ui::StatusRole::Info);
            auto* watcher = new QFutureWatcher<Result>(this);
            auto promise = std::make_shared<QPromise<Result>>();
            promise->start();
            connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, generation]()
            {
                watcher->deleteLater();
                if (generation != m_generation || watcher->future().resultCount() == 0) return;
                const Result result = watcher->result();
                m_assess->setEnabled(true);
                m_cancel->setEnabled(false);
                const bool success = result.stage == Stage::None && result.anchor != nullptr;
                m_result = success ? std::make_shared<Result>(result) : nullptr;
                m_probe->setEnabled(success);
                m_report->setPlainText(reportText(result));
                m_status->setText(success ? (result.probe
                    ? text("privilege.workbench.access.probe_done", "实际打开测试完成；详见证据报告")
                    : text("privilege.workbench.access.done", "描述符评估完成；实际打开尚未测试"))
                    : text("privilege.workbench.access.failed", "%1失败：%2").arg(stageText(result.stage), errorText(result.error)));
                ks::ui::ApplyStatusRole(m_status, success ? ks::ui::StatusRole::Info : ks::ui::StatusRole::Warning);
                fillTable(result, generation);
            });
            watcher->setFuture(promise->future());
            QThreadPool::globalInstance()->start([promise, worker = std::move(worker), cancellation]() mutable
            {
                Result result;
                try { result = worker(cancellation); }
                catch (...) { result.stage = Stage::Internal; result.error = ERROR_UNHANDLED_EXCEPTION; }
                promise->addResult(std::move(result));
                promise->finish();
            });
        }

        void assess()
        {
            Request request;
            if (!readRequest(request)) return;
            m_result.reset();
            start([request](const auto& cancellation) { return diagnose(request, cancellation); }, false);
        }

        void probe()
        {
            if (m_result == nullptr || m_result->anchor == nullptr) return;
            const Result baseline = *m_result;
            start([baseline](const auto& cancellation)
            {
                Result result;
                // Only this disposable thread impersonates. It exits even if reverting fails,
                // leaving neither the GUI thread nor a shared thread-pool worker impersonated.
                std::thread isolated([&]()
                {
                    try { result = probeOnDedicatedThread(baseline, cancellation); }
                    catch (...) { result.stage = Stage::Internal; result.error = ERROR_UNHANDLED_EXCEPTION; }
                });
                isolated.join();
                return result;
            }, true);
        }

        void fillTable(const Result& result, const quint64 generation)
        {
            m_table->clearContents();
            m_table->setRowCount(int(result.aces.size()));
            auto snapshot = std::make_shared<Result>(result);
            auto next = std::make_shared<size_t>(0);
            auto* timer = new QTimer(this);
            timer->setSingleShot(true);
            connect(timer, &QTimer::timeout, this, [this, timer, snapshot, next, generation]()
            {
                if (generation != m_generation) { timer->deleteLater(); return; }
                const size_t end = std::min(*next + size_t(12), snapshot->aces.size());
                for (; *next < end; ++*next)
                {
                    const auto& evidence = snapshot->aces[*next];
                    const QStringList columns{
                        QString::number(*next + 1), aceType(evidence.ace), evidence.sid, evidence.account,
                        hex(evidence.ace.mask), maskText(snapshot->request.kind,
                            mappedAccess(snapshot->request.kind, evidence.ace.mask)),
                        aceFlags(evidence.ace), matchesText(evidence, *snapshot)};
                    for (int column = 0; column < columns.size(); ++column)
                    {
                        auto* item = new QTableWidgetItem(columns[column]);
                        item->setToolTip(columns[column]);
                        m_table->setItem(int(*next), column, item);
                    }
                }
                if (*next < snapshot->aces.size()) timer->start(0);
                else { m_table->setEnabled(true); timer->deleteLater(); }
            });
            timer->start(0);
        }

        QLineEdit* m_pid = nullptr;
        QComboBox* m_kind = nullptr;
        QLineEdit* m_path = nullptr;
        QPushButton* m_browse = nullptr;
        QLabel* m_pathHint = nullptr;
        QComboBox* m_view = nullptr;
        QLabel* m_viewLabel = nullptr;
        QComboBox* m_preset = nullptr;
        QLineEdit* m_mask = nullptr;
        QPushButton* m_assess = nullptr;
        QPushButton* m_probe = nullptr;
        QPushButton* m_cancel = nullptr;
        QLabel* m_status = nullptr;
        ks::ui::VisibleTableWidget* m_table = nullptr;
        QPlainTextEdit* m_report = nullptr;
        quint64 m_generation = 0;
        std::shared_ptr<std::atomic_bool> m_cancellation;
        std::shared_ptr<Result> m_result;
    };
}

    QWidget* createAccessDiagnosticPage(QWidget* parent)
    {
        return new AccessPage(parent);
    }
}
