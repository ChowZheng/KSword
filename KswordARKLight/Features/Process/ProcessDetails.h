#pragma once

#include "ProcessColumns.h"
#include "../../../Ksword5.1/Ksword5.1/ksword/process/process.h"

namespace Ksword::Features::Process {

// Pure demand/formatting layer shared by the worker and regression tests.
std::uint32_t DetailDemandForColumns(const std::vector<ProcessColumnId>& columns);
void ApplyProcessDetailRecord(ProcessSnapshotRow& row, const ks::process::ProcessRecord& record);

} // namespace Ksword::Features::Process
