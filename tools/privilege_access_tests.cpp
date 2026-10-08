// In-memory ACL fixtures only: no filesystem/registry/service security changes.
#include "../Ksword5.1/Ksword5.1/PrivilegeDock/PrivilegeAccessSecurity.h"

#include <cstdio>
#include <cstdlib>

using namespace ks::privilege::access;

namespace
{
    int assertions = 0;
    void require(bool condition, const char* description)
    {
        ++assertions;
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s (Win32=%lu)\n", description, GetLastError());
            std::exit(1);
        }
    }

    struct Fixture
    {
        SECURITY_DESCRIPTOR descriptor{};
        std::vector<BYTE> aclBytes = std::vector<BYTE>(2048);
        PACL acl = reinterpret_cast<PACL>(aclBytes.data());
        Fixture(PSID owner, PACL overrideAcl = nullptr, bool nullAcl = false)
        {
            require(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) != FALSE,
                "initialize descriptor");
            require(SetSecurityDescriptorOwner(&descriptor, owner, FALSE) != FALSE, "set owner");
            require(SetSecurityDescriptorGroup(&descriptor, owner, FALSE) != FALSE, "set group");
            require(InitializeAcl(acl, DWORD(aclBytes.size()), ACL_REVISION) != FALSE, "initialize ACL");
            require(SetSecurityDescriptorDacl(&descriptor, TRUE,
                nullAcl ? nullptr : (overrideAcl ? overrideAcl : acl), FALSE) != FALSE, "set DACL");
        }
        void allow(PSID sid, DWORD mask, DWORD flags = 0)
        {
            require(AddAccessAllowedAceEx(acl, ACL_REVISION, flags, mask, sid) != FALSE, "append allow");
        }
        void deny(PSID sid, DWORD mask, DWORD flags = 0)
        {
            require(AddAccessDeniedAceEx(acl, ACL_REVISION, flags, mask, sid) != FALSE, "append deny");
        }
    };

    void expect(Fixture& fixture, HANDLE token, ObjectKind kind, DWORD requested, bool expected,
        const char* description)
    {
        const auto native = checkDescriptor(&fixture.descriptor, token, kind, requested);
        if (native.allowed != expected) std::fprintf(stderr,
            "AccessCheck: allowed=%d error=%lu granted=%lx request=%lx\n", native.allowed,
            native.error, native.granted, requested);
        require(native.succeeded, "AccessCheck call must succeed");
        require(native.allowed == expected, description);
        require(native.error == (expected ? ERROR_SUCCESS : ERROR_ACCESS_DENIED),
            "successful AccessCheck has stable allow or deny status");
        const auto authz = checkDescriptorAuthz(&fixture.descriptor, token, kind, requested);
        if (authz.allowed != expected) std::fprintf(stderr,
            "Authz: allowed=%d error=%lu granted=%lx request=%lx\n", authz.allowed,
            authz.error, authz.granted, requested);
        require(authz.succeeded, "AuthzAccessCheck call must succeed");
        require(authz.allowed == expected, description);
        if (expected)
        {
            require((native.granted & mappedAccess(kind, requested)) == mappedAccess(kind, requested),
                "granted mask covers mapped request");
            require((authz.granted & mappedAccess(kind, requested)) == mappedAccess(kind, requested),
                "Authz granted mask covers mapped request");
        }
    }
}

int main()
{
    HANDLE primary = nullptr;
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &primary) != FALSE,
        "open own token");
    require(DuplicateTokenEx(primary, TOKEN_QUERY, nullptr, SecurityImpersonation,
        TokenImpersonation, &token) != FALSE, "duplicate own token");
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<BYTE> userBytes(needed);
    require(GetTokenInformation(token, TokenUser, userBytes.data(), needed, &needed) != FALSE,
        "read user SID");
    const auto owner = reinterpret_cast<TOKEN_USER*>(userBytes.data())->User.Sid;
    BYTE worldBytes[SECURITY_MAX_SID_SIZE]{};
    DWORD worldSize = sizeof(worldBytes);
    require(CreateWellKnownSid(WinWorldSid, nullptr, worldBytes, &worldSize) != FALSE, "world SID");
    const auto world = static_cast<PSID>(worldBytes);

    {
        Fixture f(owner, nullptr, true);
        expect(f, token, ObjectKind::File, FILE_READ_DATA | FILE_WRITE_DATA, true, "NULL DACL allows");
    }
    {
        Fixture f(owner);
        expect(f, token, ObjectKind::File, FILE_READ_DATA, false, "empty DACL denies data access");
    }
    for (const auto kind : {ObjectKind::File, ObjectKind::Registry, ObjectKind::Service})
    {
        Fixture f(owner);
        f.allow(world, GENERIC_READ);
        expect(f, token, kind, GENERIC_READ, true, "generic ACE and request map for object kind");
        const auto mapping = genericMapping(kind);
        require(mappedAccess(kind, GENERIC_READ) == mapping.GenericRead, "read generic mapping");
        require(mappedAccess(kind, GENERIC_WRITE) == mapping.GenericWrite, "write generic mapping");
        require(mappedAccess(kind, GENERIC_EXECUTE) == mapping.GenericExecute, "execute generic mapping");
        require(mappedAccess(kind, GENERIC_ALL) == mapping.GenericAll, "all generic mapping");
    }
    {
        Fixture f(owner);
        f.deny(world, FILE_WRITE_DATA);
        f.allow(world, FILE_GENERIC_READ | FILE_GENERIC_WRITE);
        expect(f, token, ObjectKind::File, FILE_WRITE_DATA, false, "ordered deny ACE wins");
        expect(f, token, ObjectKind::File, FILE_READ_DATA, true, "deny mask does not block unrelated read");
    }
    {
        Fixture f(owner);
        f.deny(world, FILE_READ_DATA, INHERIT_ONLY_ACE | CONTAINER_INHERIT_ACE);
        f.allow(world, FILE_READ_DATA);
        expect(f, token, ObjectKind::File, FILE_READ_DATA, true, "inherit-only ACE does not apply to object");
        const auto decoded = decodeAcl(f.acl, f.aclBytes.size());
        require(decoded.valid && decoded.aces.size() == 2, "bounded ACL decoder reads both ACEs");
        require((decoded.aces[0].flags & INHERIT_ONLY_ACE) != 0, "inherit-only evidence retained");
        require(EqualSid(const_cast<BYTE*>(decoded.aces[0].sid.data()), world) != FALSE, "decoded SID retained");
    }
    {
        SID_AND_ATTRIBUTES restrictedSid{world, 0};
        HANDLE restrictedPrimary = nullptr;
        HANDLE restrictedToken = nullptr;
        require(CreateRestrictedToken(primary, 0, 0, nullptr, 0, nullptr, 1, &restrictedSid,
            &restrictedPrimary) != FALSE, "create isolated restricted token");
        require(DuplicateTokenEx(restrictedPrimary, TOKEN_QUERY, nullptr, SecurityImpersonation,
            TokenImpersonation, &restrictedToken) != FALSE, "duplicate restricted token");
        Fixture userOnly(owner);
        userOnly.allow(owner, FILE_READ_DATA);
        expect(userOnly, token, ObjectKind::File, FILE_READ_DATA, true, "user-only ACE allows normal token");
        expect(userOnly, restrictedToken, ObjectKind::File, FILE_READ_DATA, false,
            "restricted token requires restricted SID grant too");
        Fixture both(owner);
        both.allow(world, FILE_READ_DATA);
        expect(both, restrictedToken, ObjectKind::File, FILE_READ_DATA, true,
            "world grant satisfies normal and restricted passes");
        CloseHandle(restrictedToken);
        CloseHandle(restrictedPrimary);
    }
    {
        SID_AND_ATTRIBUTES disableWorld{world, 0};
        HANDLE denyOnlyPrimary = nullptr;
        HANDLE denyOnlyToken = nullptr;
        require(CreateRestrictedToken(primary, 0, 1, &disableWorld, 0, nullptr, 0, nullptr,
            &denyOnlyPrimary) != FALSE, "create isolated deny-only token");
        require(DuplicateTokenEx(denyOnlyPrimary, TOKEN_QUERY, nullptr, SecurityImpersonation,
            TokenImpersonation, &denyOnlyToken) != FALSE, "duplicate deny-only token");
        Fixture worldOnly(owner);
        worldOnly.allow(world, FILE_READ_DATA);
        expect(worldOnly, denyOnlyToken, ObjectKind::File, FILE_READ_DATA, false,
            "deny-only SID cannot grant access");
        Fixture denied(owner);
        denied.deny(world, FILE_READ_DATA);
        denied.allow(owner, FILE_READ_DATA);
        expect(denied, denyOnlyToken, ObjectKind::File, FILE_READ_DATA, false,
            "deny-only SID still participates in deny ACE");
        CloseHandle(denyOnlyToken);
        CloseHandle(denyOnlyPrimary);
    }
    {
        BYTE truncated[sizeof(ACE_HEADER)]{};
        require(decodeAce(truncated, sizeof(truncated)).malformed, "truncated mask rejected");
        std::vector<BYTE> malformed(sizeof(ACCESS_ALLOWED_ACE) + 8);
        auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(malformed.data());
        ace->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
        ace->Header.AceSize = WORD(malformed.size());
        auto* sid = reinterpret_cast<SID*>(&ace->SidStart);
        sid->Revision = SID_REVISION;
        sid->SubAuthorityCount = SID_MAX_SUB_AUTHORITIES;
        require(decodeAce(malformed.data(), malformed.size()).malformed,
            "oversized SID subauthority count rejected before SID API");
        ACL invalid{};
        invalid.AclRevision = ACL_REVISION;
        invalid.AclSize = WORD(sizeof(ACL) + 32);
        require(!decodeAcl(&invalid, sizeof(invalid)).valid, "ACL claimed bytes exceed allocation");
    }
    require(integrityHintMask(ObjectKind::File, GENERIC_WRITE, SECURITY_MANDATORY_LOW_RID,
        SECURITY_MANDATORY_MEDIUM_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP,
        TOKEN_MANDATORY_POLICY_NO_WRITE_UP) != 0, "low-to-medium write MIC evidence");
    require(integrityHintMask(ObjectKind::File, GENERIC_READ, SECURITY_MANDATORY_LOW_RID,
        SECURITY_MANDATORY_MEDIUM_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP,
        TOKEN_MANDATORY_POLICY_NO_WRITE_UP) == 0, "shared READ_CONTROL and SYNCHRONIZE are not write evidence");
    require(integrityHintMask(ObjectKind::File, GENERIC_WRITE, SECURITY_MANDATORY_HIGH_RID,
        SECURITY_MANDATORY_MEDIUM_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP,
        TOKEN_MANDATORY_POLICY_NO_WRITE_UP) == 0, "higher integrity has no upward restriction hint");
    CloseHandle(token);
    CloseHandle(primary);
    std::printf("PASS: %d native AccessCheck/Authz and bounded-parser assertions; no object mutations\n",
        assertions);
    return 0;
}
