#pragma once

#include "ProcessDetails.h"

namespace Ksword::Features::Process {
// Optional, read-only R3 queries; no imported dependency on newer Windows APIs.
void QueryProcessExtraDetails(ks::process::ProcessRecord& record, std::uint32_t demand, bool needEfficiency);
} // namespace Ksword::Features::Process
