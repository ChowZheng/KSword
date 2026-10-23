#include "KernelProjectionBuilder.h"

#include "KernelPage.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ksword::Features::Kernel {

KernelValueProjectionBuilder::KernelValueProjectionBuilder(std::shared_ptr<const KernelProjectionInput> input)
    : input_(std::move(input)), currentColumns_(input_->source->columns),
      currentRawColumns_(input_->source->columns), currentRawRows_(input_->source->rows) {
    if (!currentRawRows_ || (currentRawRows_->empty() && !input_->source->fallbackDisplay.empty())) {
        currentRawRows_ = input_->source->fallbackDisplay.contiguousSnapshot();
    }
    if (!currentRawRows_) currentRawRows_ = EmptyKernelRawRows();
    for(std::size_t index=0;index<input_->selectedChoices.size();++index) {
        auto& choice=choices_[index];choice.items.push_back(L"");
        if(!input_->selectedChoices[index].empty()){choice.items.push_back(input_->selectedChoices[index]);choice.selected=1;}
    }
}

std::shared_ptr<KernelValueProjection> KernelValueProjectionBuilder::build(const std::uint64_t requestId) {
    result_=std::make_shared<KernelValueProjection>();
    result_->input=input_;
    result_->requestId=requestId;
    auto summary=std::make_shared<KernelRawSummaryIndex>(*currentRawRows_,currentRawColumns_);
    summary->statusFlags=FirstRowUInt64(*currentRawRows_,currentRawColumns_,{L"StatusFlags"});
    summary->dynStatusFlags=FirstRowUInt64(*currentRawRows_,currentRawColumns_,{L"DynDataStatus"});
    result_->rawSummary=std::move(summary);
    const auto feature=input_->source->featureId;
    switch(feature) {
    case KernelFeatureId::ObjectNamespaceOverview:
    case KernelFeatureId::ObjectDirectoryRecursive:
    case KernelFeatureId::NamedPipe:
    case KernelFeatureId::BaseNamedObjects:
    case KernelFeatureId::SymbolicLink:
    case KernelFeatureId::DeviceDriverObjects:
    case KernelFeatureId::ObjectTypeMatrix:
    case KernelFeatureId::CommunicationEndpoint: RebuildObjectNamespaceListFromCache(feature); break;
    case KernelFeatureId::CpuHardwareSnapshot:
    case KernelFeatureId::PhysicalMemoryLayout:
    case KernelFeatureId::MutationAudit:
    case KernelFeatureId::KeyboardHotkeys:
    case KernelFeatureId::KeyboardHooks:
    case KernelFeatureId::DynDataCapabilities:
    case KernelFeatureId::MinifilterBypassPids:
    case KernelFeatureId::KernelTimerDpc:
    case KernelFeatureId::IoctlRegistry: RebuildR0ValueProjection(feature); break;
    case KernelFeatureId::DynData:
    case KernelFeatureId::DriverStatus: RebuildDiagnosticDualTableFromCache(feature); break;
    case KernelFeatureId::CallbackEnumeration: RebuildCallbackEnumerationListFromCache(); break;
    case KernelFeatureId::AtomTable: RebuildAtomTableFromCache(); break;
    case KernelFeatureId::KernelExecutableMemory:
    case KernelFeatureId::KernelMemoryEvidence: RebuildKernelMemoryScanListFromCache(feature); break;
    case KernelFeatureId::ProcessCrossView:
    case KernelFeatureId::ThreadCrossView: RebuildCrossViewListFromCache(feature); break;
    case KernelFeatureId::DriverIntegrity:
    case KernelFeatureId::KernelCpuIntegrity: RebuildIntegrityEvidenceListFromCache(feature); break;
    default: RebuildKernelHookListFromCache(feature); break;
    }
    result_->nativeTreeHandles.resize(result_->treeNodes.size());
    for(std::size_t index=0;index<result_->treeNodes.size();++index) {
        const auto& node=result_->treeNodes[index];if(!node.state)continue;
        const auto key=KernelTreeNodeKey(*node.state);
        if(!input_->treeSelectedKey.empty() && key==input_->treeSelectedKey)result_->treeSelected=index+1;
        if(!input_->treeTopKey.empty() && key==input_->treeTopKey)result_->treeTop=index+1;
    }
    result_->choices=std::move(choices_);
    result_->columns=std::move(currentColumns_);
    result_->rows=std::move(currentRows_);
    result_->indents=std::move(currentRowIndents_);
    for(std::size_t index=0;index<result_->rows.size();++index) {
        result_->rowHashes[KernelProjectedRowHash(result_->rows[index])].push_back(index);
    }
    return result_;
}

} // namespace Ksword::Features::Kernel
