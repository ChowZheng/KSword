#pragma once

#include "../../../Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverTypes.h"
#include <utility>

namespace Ksword::Features::Kernel {

enum class CallbackRemovalPolicy { Unavailable, Candidate, Verified };

std::uint32_t CallbackRemovalType(std::uint32_t callbackClass);
std::wstring CallbackClassLabel(std::uint32_t callbackClass);
std::wstring CallbackRegistrationLabel(std::uint32_t registrationType, std::uint32_t callbackClass);
CallbackRemovalPolicy CallbackRemovalPolicyFor(const ksword::ark::CallbackEnumEntry& entry);
std::wstring CallbackRemovalLabel(const ksword::ark::CallbackEnumEntry& entry);
const wchar_t* CallbackRemovalGlyph(const ksword::ark::CallbackEnumEntry& entry);

// Both the menu and facade reconstruct identity from numeric protocol cells;
// translated display labels never authorize an operation.
bool ParseCallbackRemovalFields(const std::vector<std::pair<std::wstring, std::wstring>>& fields,
    ksword::ark::CallbackEnumEntry& entry, std::wstring& error);

// Pure EX packet construction. Minifilter children preserve their displayed
// operation record but carry the owning FilterObject in the request handle.
bool BuildCallbackRemovalRequest(const ksword::ark::CallbackEnumEntry& entry,
    KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_REQUEST& packet, std::wstring& error);

bool CallbackRemovalResponseValid(const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_RESPONSE& response,
    std::size_t bytesReturned);

} // namespace Ksword::Features::Kernel
