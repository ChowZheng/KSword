#pragma once

#include "KernelProjectionValues.h"
#include "KernelValueSupport.h"
#include "KernelPageLayout.h"
#include "../../Core/Win32Lean.h"

#include <CommCtrl.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ksword::Features::Kernel {

struct KernelValueControl {
    int id = 0;
    explicit operator bool() const noexcept { return id != 0; }
};

// This class has value-only controls and immutable inputs. A missed Win32
// control call cannot compile: Control has no conversion to HWND.
class KernelValueProjectionBuilder final {
public:
    explicit KernelValueProjectionBuilder(std::shared_ptr<const KernelProjectionInput> input);
    std::shared_ptr<KernelValueProjection> build(std::uint64_t requestId);
private:
    struct R0Values {
        KernelFeatureId featureId=KernelFeatureId::CpuHardwareSnapshot;
        std::vector<std::wstring> columns;
        Ksword::Ui::ImmutableTextRows rows;
        std::wstring statusText,selectedRowSignature;
        int topRow=0;
    };
    struct InsensitiveLess {
        bool operator()(const std::wstring& left,const std::wstring& right) const {return ::_wcsicmp(left.c_str(),right.c_str())<0;}
    };
    const std::vector<std::wstring>& Aliases(const KernelFeatureId feature,const std::wstring& column) {
        const auto key=std::make_pair(feature,column);
        auto found=aliases_.find(key);
        if(found==aliases_.end()) {
            auto aliases=ColumnAliases(feature,column);
            if(aliases.empty())aliases.push_back(column);
            found=aliases_.emplace(key,std::move(aliases)).first;
        }
        return found->second;
    }
    std::wstring ReadField(const std::vector<std::wstring>& row,const std::vector<std::wstring>& columns,
        std::initializer_list<const wchar_t*> names) {
        auto found=columnIndexes_.find(&columns);
        if(found==columnIndexes_.end()) {
            ColumnIndex index;
            for(std::size_t column=0;column<columns.size();++column) index[columns[column]].push_back(column);
            found=columnIndexes_.emplace(&columns,std::move(index)).first;
        }
        for(const auto* name:names) {
            const auto indexes=found->second.find(name);
            if(indexes==found->second.end())continue;
            for(const auto index:indexes->second)if(index<row.size()&&!row[index].empty())return row[index];
        }
        return {};
    }
    bool HasValue(const std::vector<std::wstring>& row,const std::vector<std::wstring>& columns,
        std::initializer_list<const wchar_t*> names){return !ReadField(row,columns,names).empty();}
    std::uint64_t ReadNonZero(const std::vector<std::wstring>& row,const std::vector<std::wstring>& columns,
        std::initializer_list<const wchar_t*> names){std::uint64_t value=0;return HexToUInt64(ReadField(row,columns,names),value)?value:0;}
    const std::wstring& Text(KernelValueControl control) const {
        return control.id==2 ? input_->moduleFilter : input_->filter;
    }
    int Checked(KernelValueControl) const {return input_->riskOnly?BST_CHECKED:BST_UNCHECKED;}
    void WriteText(KernelValueControl control,const wchar_t* text) {
        if(control.id==3){result_->status=text?text:L"";result_->hasStatus=true;}
        if(control.id==4){result_->detail=text?text:L"";result_->hasDetail=true;}
    }
    void SetColumn(int index,const std::wstring&,int width) {
        if(index>=0){if(result_->widths.size()<=static_cast<std::size_t>(index))result_->widths.resize(static_cast<std::size_t>(index)+1);result_->widths[static_cast<std::size_t>(index)]=width;}
    }
    std::array<KernelProjectedChoice,4> choices_;
    const std::wstring& ChoiceText(KernelValueControl control) const {
        static const std::wstring empty;
        if(control.id<10 || control.id>13)return empty;
        const auto& choice=choices_[static_cast<std::size_t>(control.id-10)];
        return choice.selected>=0 && static_cast<std::size_t>(choice.selected)<choice.items.size()?choice.items[static_cast<std::size_t>(choice.selected)]:empty;
    }
    LRESULT ChoiceMessage(KernelValueControl control,UINT message,WPARAM parameter,LPARAM text) {
        if(control.id<10 || control.id>13)return CB_ERR;
        auto& choice=choices_[static_cast<std::size_t>(control.id-10)];
        switch(message) {
        case CB_GETCURSEL:return choice.selected;
        case CB_RESETCONTENT:choice.items.clear();choice.selected=-1;choice.changed=true;return 0;
        case CB_ADDSTRING:choice.items.emplace_back(reinterpret_cast<const wchar_t*>(text));return static_cast<LRESULT>(choice.items.size()-1);
        case CB_SETCURSEL:choice.selected=static_cast<int>(parameter);return choice.selected;
        default:return CB_ERR;
        }
    }
    void RequireTreeValueModel() {RebuildObjectNamespaceTreeFromCurrentRows();}
    void ExpandTreeValue(std::size_t item){if(item>0&&item<=result_->treeNodes.size())result_->treeNodes[item-1].expand=true;}
    void SelectTreeValue(std::size_t item){result_->treeSelected=item;}
    const KernelFeatureDescriptor* CurrentDescriptor() const {return &input_->descriptor;}
    std::vector<int> CurrentCopyColumnIndices() const {
        auto columns=PreferredCopyColumnIndices(input_->source->featureId,currentColumns_);
        if(columns.empty())for(std::size_t index=0;index<currentColumns_.size();++index)columns.push_back(static_cast<int>(index));
        return columns;
    }
    void AppendSummary(KernelValueControl,std::vector<std::wstring> row){result_->summary.push_back(std::move(row));}
    void ClearValueList(KernelValueControl){result_->summary.clear();}
    void ClearTables(){result_->widths.clear();result_->summary.clear();}
    void CommitRows(){}
    void SetSelection(KernelValueControl,int,unsigned,unsigned){result_->selectFirst=true;}
    void EnsureItem(KernelValueControl,int,bool){}
    void SelectionDetail(){result_->selectionDetail=true;}
    std::shared_ptr<const KernelProjectionInput> input_;
    std::shared_ptr<KernelValueProjection> result_;
    std::vector<std::wstring> currentColumns_,currentRawColumns_;
    std::shared_ptr<const KernelRawRows> currentRawRows_;
    Ksword::Ui::ImmutableTextRows currentRows_;
    std::vector<int> currentRowIndents_;
    using ColumnIndex=std::map<std::wstring,std::vector<std::size_t>,InsensitiveLess>;
    std::unordered_map<const std::vector<std::wstring>*,ColumnIndex> columnIndexes_;
    std::map<std::pair<KernelFeatureId,std::wstring>,std::vector<std::wstring>> aliases_;
    const KernelValueControl resultList_{5},filterEdit_{1},moduleFilterEdit_{2},statusText_{3},detailEdit_{4},riskOnlyCheck_{6},summaryList_{7};
    const KernelValueControl deviceDriverDirectoryCombo_{input_->choicePresent[0]?10:0},deviceDriverTypeCombo_{input_->choicePresent[1]?11:0},
        baseNamedScopeCombo_{input_->choicePresent[2]?12:0},baseNamedTypeCombo_{input_->choicePresent[3]?13:0};

    void RebuildKernelHookListFromCache(KernelFeatureId featureId);
    void RebuildAtomTableFromCache();
    void RebuildKernelMemoryScanListFromCache(KernelFeatureId featureId);
    void RebuildCrossViewListFromCache(KernelFeatureId featureId);
    void RebuildIntegrityEvidenceListFromCache(KernelFeatureId featureId);
    void RebuildDiagnosticDualTableFromCache(KernelFeatureId featureId);
    void RebuildCallbackEnumerationListFromCache();
    std::wstring BuildDiagnosticReportForCurrentFeature() const;
    std::wstring NormalizeCallbackModulePath(const std::wstring& modulePath) const;
    R0Values BuildR0Values(KernelFeatureId featureId, std::wstring pageName,
        const std::vector<std::wstring>& rawColumns, const KernelRawRows& rawRows,
        std::wstring filterText, bool riskOnly, std::wstring selectedRowSignature,
        int topRow);
    void RebuildR0ValueProjection(KernelFeatureId featureId);
    void RebuildObjectNamespaceListFromCache(KernelFeatureId featureId);
    void RebuildObjectNamespaceTreeFromCurrentRows();
};

} // namespace Ksword::Features::Kernel
