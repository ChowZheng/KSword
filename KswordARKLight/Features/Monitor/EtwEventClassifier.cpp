#include "EtwEventClassifier.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <string_view>

namespace Ksword::Features::Monitor {

namespace {

std::wstring Lower(std::wstring_view text) {
    std::wstring lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return lowered;
}

bool ContainsAny(const std::wstring& loweredText, const std::initializer_list<std::wstring_view> needles) {
    return std::any_of(needles.begin(), needles.end(),
        [&loweredText](const std::wstring_view needle) {
            return loweredText.find(needle) != std::wstring::npos;
        });
}

// The provider name is checked first because ETW provider names follow
// Microsoft's own spelling (Microsoft-Windows-Kernel-File, ...) and do not vary
// with localization, unlike the summary text.
struct ProviderRule {
    std::wstring_view needle;
    EtwCategory category;
};

constexpr std::array<ProviderRule, 13> kProviderRules{{
    { L"kernel-file", EtwCategory::File },
    { L"kernel-registry", EtwCategory::Registry },
    { L"kernel-process", EtwCategory::Process },
    { L"kernel-thread", EtwCategory::Thread },
    { L"kernel-image", EtwCategory::Image },
    { L"kernel-network", EtwCategory::Network },
    { L"tcpip", EtwCategory::Network },
    { L"ndis", EtwCategory::Network },
    { L"dns-client", EtwCategory::Network },
    { L"security-auditing", EtwCategory::Security },
    { L"kernel-audit-api", EtwCategory::Security },
    { L"amsi", EtwCategory::Security },
    { L"kernel-object", EtwCategory::Process },
}};

// Summary keywords only cover what the compact row actually carries. They are
// intentionally narrow: matching a bare "file" would pull in every event whose
// description happens to mention the word.
EtwCategory CategoryFromSummary(const std::wstring& loweredSummary) {
    if (ContainsAny(loweredSummary, { L"\\registry\\", L"hklm\\", L"hkcu\\", L"注册表" })) {
        return EtwCategory::Registry;
    }
    if (ContainsAny(loweredSummary, { L".sys", L".dll", L".exe", L"映像", L"image load" })) {
        return EtwCategory::Image;
    }
    if (ContainsAny(loweredSummary, { L"\\device\\", L".tmp", L"文件", L"写入", L"读取" })) {
        return EtwCategory::File;
    }
    if (ContainsAny(loweredSummary, { L"tcp", L"udp", L":443", L":80", L"连接", L"端口" })) {
        return EtwCategory::Network;
    }
    if (ContainsAny(loweredSummary, { L"process", L"进程", L"createprocess" })) {
        return EtwCategory::Process;
    }
    if (ContainsAny(loweredSummary, { L"thread", L"线程" })) {
        return EtwCategory::Thread;
    }
    return EtwCategory::Other;
}

} // namespace

EtwCategory ClassifyEtwEvent(const EtwEvent& eventRow) {
    if (!eventRow.providerText.empty()) {
        const std::wstring loweredProvider = Lower(eventRow.providerText);
        for (const ProviderRule& rule : kProviderRules) {
            if (loweredProvider.find(rule.needle) != std::wstring::npos) {
                return rule.category;
            }
        }
    }
    return CategoryFromSummary(Lower(eventRow.summary));
}

const wchar_t* EtwCategoryText(const EtwCategory category) {
    switch (category) {
    case EtwCategory::All: return L"全部";
    case EtwCategory::Process: return L"进程";
    case EtwCategory::Thread: return L"线程";
    case EtwCategory::Image: return L"映像";
    case EtwCategory::File: return L"文件";
    case EtwCategory::Registry: return L"注册表";
    case EtwCategory::Network: return L"网络";
    case EtwCategory::Security: return L"安全";
    case EtwCategory::Other: return L"其他";
    default: return L"其他";
    }
}

bool IsCategorySelectable(const EtwCategory category) {
    return category != EtwCategory::All;
}

const wchar_t* EtwLevelText(const std::uint8_t level) {
    switch (level) {
    case 1: return L"严重";
    case 2: return L"错误";
    case 3: return L"警告";
    case 4: return L"信息";
    case 5: return L"详细";
    default: return L"其他";
    }
}

bool EtwLevelFilterMatches(const int levelFilterIndex, const std::uint8_t level) {
    // Index 0 is "全部"; index n means "level == n" because the combo is filled
    // in level order 1..5 right after 全部.
    if (levelFilterIndex <= 0) {
        return true;
    }
    return level == static_cast<std::uint8_t>(levelFilterIndex);
}

bool EtwCategoryFilterMatches(const EtwCategory filter, const EtwEvent& eventRow) {
    if (filter == EtwCategory::All) {
        return true;
    }
    return ClassifyEtwEvent(eventRow) == filter;
}

} // namespace Ksword::Features::Monitor
