#pragma once

// Pure Win32 descriptor assessment shared by the page and nonmutating fixtures.
// No object is modified and no privilege is enabled by these helpers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <Authz.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ks::privilege::access
{
    enum class ObjectKind { File, Registry, Service };

    inline GENERIC_MAPPING genericMapping(const ObjectKind kind)
    {
        switch (kind)
        {
        case ObjectKind::Registry:
            return { KEY_READ, KEY_WRITE, KEY_EXECUTE, KEY_ALL_ACCESS };
        case ObjectKind::Service:
            return {
                STANDARD_RIGHTS_READ | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS
                    | SERVICE_INTERROGATE | SERVICE_ENUMERATE_DEPENDENTS,
                STANDARD_RIGHTS_WRITE | SERVICE_CHANGE_CONFIG,
                STANDARD_RIGHTS_EXECUTE | SERVICE_START | SERVICE_STOP
                    | SERVICE_PAUSE_CONTINUE | SERVICE_USER_DEFINED_CONTROL,
                SERVICE_ALL_ACCESS
            };
        default:
            return { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
        }
    }

    inline DWORD mappedAccess(const ObjectKind kind, DWORD mask)
    {
        auto mapping = genericMapping(kind);
        MapGenericMask(&mask, &mapping);
        return mask;
    }

    struct AceView
    {
        BYTE type = 0;
        BYTE flags = 0;
        DWORD mask = 0;
        std::vector<BYTE> sid;
        bool malformed = false;
        bool complex = false;
        bool discretionary = false;
    };

    inline bool isAllowType(const BYTE type)
    {
        return type == ACCESS_ALLOWED_ACE_TYPE || type == ACCESS_ALLOWED_OBJECT_ACE_TYPE
            || type == ACCESS_ALLOWED_CALLBACK_ACE_TYPE
            || type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
    }

    inline bool isDenyType(const BYTE type)
    {
        return type == ACCESS_DENIED_ACE_TYPE || type == ACCESS_DENIED_OBJECT_ACE_TYPE
            || type == ACCESS_DENIED_CALLBACK_ACE_TYPE
            || type == ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE;
    }

    // Validate lengths before invoking SID APIs. Callback payload bytes are kept out of SID.
    inline AceView decodeAce(const BYTE* bytes, const size_t available)
    {
        AceView result;
        if (bytes == nullptr || available < sizeof(ACE_HEADER))
        {
            result.malformed = true;
            return result;
        }
        ACE_HEADER header{};
        std::memcpy(&header, bytes, sizeof(header));
        result.type = header.AceType;
        result.flags = header.AceFlags;
        if (header.AceSize < sizeof(ACE_HEADER) + sizeof(DWORD) || header.AceSize > available
            || (header.AceSize % sizeof(DWORD)) != 0)
        {
            result.malformed = true;
            return result;
        }
        std::memcpy(&result.mask, bytes + sizeof(ACE_HEADER), sizeof(DWORD));
        result.discretionary = isAllowType(header.AceType) || isDenyType(header.AceType);
        const bool objectAce = header.AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE
            || header.AceType == ACCESS_DENIED_OBJECT_ACE_TYPE
            || header.AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE
            || header.AceType == ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE;
        const bool callbackAce = header.AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE
            || header.AceType == ACCESS_DENIED_CALLBACK_ACE_TYPE
            || header.AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE
            || header.AceType == ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE;
        result.complex = objectAce || callbackAce || (!result.discretionary
            && header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE);
        size_t sidOffset = sizeof(ACE_HEADER) + sizeof(DWORD);
        if (objectAce)
        {
            DWORD objectFlags = 0;
            if (header.AceSize < sidOffset + sizeof(DWORD))
            {
                result.malformed = true;
                return result;
            }
            std::memcpy(&objectFlags, bytes + sidOffset, sizeof(DWORD));
            if ((objectFlags & ~(ACE_OBJECT_TYPE_PRESENT | ACE_INHERITED_OBJECT_TYPE_PRESENT)) != 0)
            {
                result.malformed = true;
                return result;
            }
            sidOffset += sizeof(DWORD);
            if (objectFlags & ACE_OBJECT_TYPE_PRESENT) sidOffset += sizeof(GUID);
            if (objectFlags & ACE_INHERITED_OBJECT_TYPE_PRESENT) sidOffset += sizeof(GUID);
        }
        // Unsupported ACE layouts never feed a guessed pointer into a SID API.
        if (!result.discretionary && header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
            return result;
        constexpr size_t sidPrefix = offsetof(SID, SubAuthority);
        if (sidOffset > header.AceSize || header.AceSize - sidOffset < sidPrefix)
        {
            result.malformed = true;
            return result;
        }
        const BYTE subAuthorityCount = bytes[sidOffset + 1];
        const size_t sidSize = sidPrefix + size_t(subAuthorityCount) * sizeof(DWORD);
        if (subAuthorityCount > SID_MAX_SUB_AUTHORITIES || sidSize > header.AceSize - sidOffset)
        {
            result.malformed = true;
            return result;
        }
        result.sid.assign(bytes + sidOffset, bytes + sidOffset + sidSize);
        if (!IsValidSid(result.sid.data()))
        {
            result.sid.clear();
            result.malformed = true;
        }
        return result;
    }

    struct AclView
    {
        bool valid = true;
        bool complex = false;
        std::vector<AceView> aces;
    };

    inline AclView decodeAcl(const ACL* acl, const size_t available)
    {
        AclView result;
        if (acl == nullptr) return result;
        if (available < sizeof(ACL))
        {
            result.valid = false;
            return result;
        }
        ACL header{};
        std::memcpy(&header, acl, sizeof(header));
        if (header.AclSize < sizeof(ACL) || header.AclSize > available
            || (header.AclRevision != ACL_REVISION && header.AclRevision != ACL_REVISION_DS))
        {
            result.valid = false;
            return result;
        }
        const auto* bytes = reinterpret_cast<const BYTE*>(acl);
        size_t offset = sizeof(ACL);
        for (DWORD index = 0; index < header.AceCount; ++index)
        {
            if (offset > header.AclSize || header.AclSize - offset < sizeof(ACE_HEADER))
            {
                result.valid = false;
                break;
            }
            ACE_HEADER aceHeader{};
            std::memcpy(&aceHeader, bytes + offset, sizeof(aceHeader));
            auto ace = decodeAce(bytes + offset, header.AclSize - offset);
            result.valid = result.valid && !ace.malformed;
            result.complex = result.complex || ace.complex;
            result.aces.push_back(std::move(ace));
            if (aceHeader.AceSize < sizeof(ACE_HEADER)
                || aceHeader.AceSize > header.AclSize - offset)
                break;
            offset += aceHeader.AceSize;
        }
        return result;
    }

    struct NativeCheck
    {
        bool succeeded = false;
        bool allowed = false;
        DWORD granted = 0;
        DWORD error = ERROR_SUCCESS;
        std::vector<LUID_AND_ATTRIBUTES> usedPrivileges;
    };

    inline std::vector<BYTE> descriptorForAuthz(PSECURITY_DESCRIPTOR descriptor,
        ObjectKind kind, DWORD& error);

    inline NativeCheck checkDescriptor(PSECURITY_DESCRIPTOR descriptor, HANDLE impersonationToken,
        const ObjectKind kind, const DWORD desiredAccess)
    {
        NativeCheck result;
        auto mappedDescriptor = descriptorForAuthz(descriptor, kind, result.error);
        if (mappedDescriptor.empty()) return result;
        auto mapping = genericMapping(kind);
        DWORD mapped = mappedAccess(kind, desiredAccess);
        DWORD bytes = sizeof(PRIVILEGE_SET) + 16 * sizeof(LUID_AND_ATTRIBUTES);
        std::vector<BYTE> storage(bytes);
        BOOL allowed = FALSE;
        SetLastError(ERROR_SUCCESS);
        BOOL succeeded = AccessCheck(mappedDescriptor.data(), impersonationToken, mapped, &mapping,
            reinterpret_cast<PRIVILEGE_SET*>(storage.data()), &bytes, &result.granted, &allowed);
        if (!succeeded && GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes <= 1024 * 1024)
        {
            storage.resize(bytes);
            succeeded = AccessCheck(mappedDescriptor.data(), impersonationToken, mapped, &mapping,
                reinterpret_cast<PRIVILEGE_SET*>(storage.data()), &bytes, &result.granted, &allowed);
        }
        result.succeeded = succeeded != FALSE;
        result.allowed = result.succeeded && allowed != FALSE;
        result.error = result.succeeded
            ? (result.allowed ? ERROR_SUCCESS : ERROR_ACCESS_DENIED) : GetLastError();
        if (result.succeeded)
        {
            const auto* privileges = reinterpret_cast<const PRIVILEGE_SET*>(storage.data());
            const size_t capacity = storage.size() >= offsetof(PRIVILEGE_SET, Privilege)
                ? (storage.size() - offsetof(PRIVILEGE_SET, Privilege)) / sizeof(LUID_AND_ATTRIBUTES) : 0;
            if (privileges->PrivilegeCount <= capacity)
                result.usedPrivileges.assign(privileges->Privilege,
                    privileges->Privilege + privileges->PrivilegeCount);
        }
        if (!result.succeeded) result.granted = 0;
        return result;
    }

    inline std::vector<BYTE> descriptorForAuthz(PSECURITY_DESCRIPTOR descriptor,
        const ObjectKind kind, DWORD& error)
    {
        std::vector<BYTE> result;
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(descriptor, &control, &revision))
        {
            error = GetLastError();
            return result;
        }
        DWORD size = 0;
        if (control & SE_SELF_RELATIVE)
        {
            size = GetSecurityDescriptorLength(descriptor);
            if (size < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || size > 1024 * 1024)
            {
                error = ERROR_INVALID_SECURITY_DESCR;
                return result;
            }
            const auto* start = static_cast<const BYTE*>(descriptor);
            result.assign(start, start + size);
        }
        else
        {
            MakeSelfRelativeSD(descriptor, nullptr, &size);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size > 1024 * 1024)
            {
                error = GetLastError();
                return result;
            }
            result.resize(size);
            if (!MakeSelfRelativeSD(descriptor, result.data(), &size))
            {
                error = GetLastError();
                result.clear();
                return result;
            }
        }
        PACL dacl = nullptr;
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        if (!GetSecurityDescriptorDacl(result.data(), &present, &dacl, &defaulted))
        {
            error = GetLastError();
            result.clear();
            return result;
        }
        if (present && dacl != nullptr)
        {
            const auto* base = result.data();
            const size_t offset = reinterpret_cast<BYTE*>(dacl) - base;
            if (offset > result.size())
            {
                error = ERROR_INVALID_ACL;
                result.clear();
                return result;
            }
            const auto aclView = decodeAcl(dacl, result.size() - offset);
            if (!aclView.valid)
            {
                error = ERROR_INVALID_ACL;
                result.clear();
                return result;
            }
            auto mapping = genericMapping(kind);
            for (DWORD index = 0; index < dacl->AceCount; ++index)
            {
                void* ace = nullptr;
                if (!GetAce(dacl, index, &ace))
                {
                    error = GetLastError();
                    result.clear();
                    return result;
                }
                const auto* header = static_cast<const ACE_HEADER*>(ace);
                if (isAllowType(header->AceType) || isDenyType(header->AceType))
                {
                    auto* mask = reinterpret_cast<DWORD*>(static_cast<BYTE*>(ace) + sizeof(ACE_HEADER));
                    MapGenericMask(mask, &mapping);
                }
            }
        }
        error = ERROR_SUCCESS;
        return result;
    }

    inline NativeCheck checkDescriptorAuthz(PSECURITY_DESCRIPTOR descriptor, HANDLE token,
        const ObjectKind kind, const DWORD desiredAccess)
    {
        NativeCheck result;
        auto mappedDescriptor = descriptorForAuthz(descriptor, kind, result.error);
        if (mappedDescriptor.empty()) return result;
        AUTHZ_RESOURCE_MANAGER_HANDLE manager = nullptr;
        if (!AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT, nullptr, nullptr,
            nullptr, nullptr, &manager))
        {
            result.error = GetLastError();
            return result;
        }
        AUTHZ_CLIENT_CONTEXT_HANDLE context = nullptr;
        const LUID identity{};
        if (!AuthzInitializeContextFromToken(0, token, manager, nullptr, identity, nullptr, &context))
        {
            result.error = GetLastError();
            AuthzFreeResourceManager(manager);
            return result;
        }
        AUTHZ_ACCESS_REQUEST request{};
        request.DesiredAccess = mappedAccess(kind, desiredAccess);
        DWORD error = ERROR_SUCCESS;
        DWORD sacl = 0;
        AUTHZ_ACCESS_REPLY reply{};
        reply.ResultListLength = 1;
        reply.GrantedAccessMask = &result.granted;
        reply.Error = &error;
        reply.SaclEvaluationResults = &sacl;
        result.succeeded = AuthzAccessCheck(0, context, &request, nullptr, mappedDescriptor.data(),
            nullptr, 0, &reply, nullptr) != FALSE;
        result.error = result.succeeded ? error : GetLastError();
        result.allowed = result.succeeded && error == ERROR_SUCCESS;
        AuthzFreeContext(context);
        AuthzFreeResourceManager(manager);
        if (!result.succeeded) result.granted = 0;
        return result;
    }

    // This is an evidence hint; kernel MIC and object-specific behavior require the real open.
    inline DWORD integrityHintMask(const ObjectKind kind, const DWORD desiredAccess,
        const DWORD subjectRid, const DWORD objectRid, const DWORD labelPolicy,
        const DWORD tokenPolicy)
    {
        if (subjectRid >= objectRid) return 0;
        const auto mapping = genericMapping(kind);
        const DWORD requested = mappedAccess(kind, desiredAccess);
        const DWORD neutral = READ_CONTROL | SYNCHRONIZE;
        DWORD restricted = 0;
        if ((labelPolicy & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP)
            && (tokenPolicy & TOKEN_MANDATORY_POLICY_NO_WRITE_UP))
            restricted |= (mapping.GenericWrite & ~neutral) | DELETE | WRITE_DAC | WRITE_OWNER;
        if (labelPolicy & SYSTEM_MANDATORY_LABEL_NO_READ_UP)
            restricted |= mapping.GenericRead & ~neutral;
        if (labelPolicy & SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP)
            restricted |= mapping.GenericExecute & ~neutral;
        return restricted & requested;
    }
}
