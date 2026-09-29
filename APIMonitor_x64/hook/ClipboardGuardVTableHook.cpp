#include "pch.h"
#include "ClipboardGuardVTableHook.h"
#include "ClipboardGuardCommon.h"

namespace apimon
{
    namespace
    {
        // COM 虚表按 IUnknown(0-2) + IDataObject 自身方法顺序排列，这是接口的
        // 二进制契约，不随实现变化：GetData=3、QueryGetData=5、EnumFormatEtc=8。
        constexpr std::size_t kGetDataSlotIndex = 3;
        constexpr std::size_t kQueryGetDataSlotIndex = 5;
        constexpr std::size_t kEnumFormatEtcSlotIndex = 8;
        // 虚表至少要覆盖到 EnumDAdvise（索引 11）才能安全整体改保护属性。
        constexpr std::size_t kVTableSlotCountToProtect = 12;

        using DataObjectGetDataFn = HRESULT(STDMETHODCALLTYPE*)(IDataObject*, FORMATETC*, STGMEDIUM*);
        using DataObjectQueryGetDataFn = HRESULT(STDMETHODCALLTYPE*)(IDataObject*, FORMATETC*);
        using DataObjectEnumFormatEtcFn = HRESULT(STDMETHODCALLTYPE*)(IDataObject*, DWORD, IEnumFORMATETC**);

        // PatchedVTableEntry：一份已打补丁虚表的原始三个函数指针。
        struct PatchedVTableEntry
        {
            DataObjectGetDataFn originalGetData = nullptr;
            DataObjectQueryGetDataFn originalQueryGetData = nullptr;
            DataObjectEnumFormatEtcFn originalEnumFormatEtc = nullptr;
        };

        // g_vtableMutex 保护 g_patchedVTables：安装（写）和三个 Hooked 函数的
        // 反查（读）都可能来自任意应用线程，必须加锁；虚表槽位本身的指针写入
        // 是单条对齐 8 字节写、天然原子，不需要额外挂起其它线程。
        std::mutex g_vtableMutex;
        std::unordered_map<void*, PatchedVTableEntry> g_patchedVTables;

        HRESULT STDMETHODCALLTYPE HookedDataObjectGetData(
            IDataObject* const thisPointer,
            FORMATETC* const formatEtc,
            STGMEDIUM* const medium);
        HRESULT STDMETHODCALLTYPE HookedDataObjectQueryGetData(
            IDataObject* const thisPointer,
            FORMATETC* const formatEtc);
        HRESULT STDMETHODCALLTYPE HookedDataObjectEnumFormatEtc(
            IDataObject* const thisPointer,
            DWORD directionValue,
            IEnumFORMATETC** const enumFormatEtcOut);

        // FindPatchedEntry 作用：按 thisPointer 的当前虚表指针查回原始函数指针组；
        // 三个 Hooked 函数体共用这一份查找逻辑。
        bool FindPatchedEntry(IDataObject* const thisPointer, PatchedVTableEntry* const entryOut)
        {
            if (thisPointer == nullptr || entryOut == nullptr)
            {
                return false;
            }
            void** const vtablePointer = *reinterpret_cast<void***>(thisPointer);
            if (vtablePointer == nullptr)
            {
                return false;
            }

            std::lock_guard<std::mutex> lockGuard(g_vtableMutex);
            const auto iterator = g_patchedVTables.find(vtablePointer);
            if (iterator == g_patchedVTables.end())
            {
                return false;
            }
            *entryOut = iterator->second;
            return true;
        }

        HRESULT STDMETHODCALLTYPE HookedDataObjectGetData(
            IDataObject* const thisPointer,
            FORMATETC* const formatEtc,
            STGMEDIUM* const medium)
        {
            PatchedVTableEntry entryValue;
            if (!FindPatchedEntry(thisPointer, &entryValue) || entryValue.originalGetData == nullptr)
            {
                // 理论上不会发生：能落到这个函数体，说明 thisPointer 的虚表
                // 就是我们打过补丁的那份，查不到原函数指针属于内部状态不一致。
                return E_UNEXPECTED;
            }

            const UINT formatValue = (formatEtc != nullptr) ? static_cast<UINT>(formatEtc->cfFormat) : 0U;
            const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Read);
            if (actionValue == ClipboardPolicyAction::Block)
            {
                ReportClipboardEvent(ClipboardOperationKind::Read, L"Ole32", L"IDataObject::GetData", formatValue, nullptr, actionValue);
                // DV_E_FORMATETC：调用方看到的和"这个格式本来就不存在"完全一样。
                return DV_E_FORMATETC;
            }

            const HRESULT resultValue = entryValue.originalGetData(thisPointer, formatEtc, medium);
            ReportClipboardEvent(ClipboardOperationKind::Read, L"Ole32", L"IDataObject::GetData", formatValue, nullptr, actionValue);
            return resultValue;
        }

        HRESULT STDMETHODCALLTYPE HookedDataObjectQueryGetData(
            IDataObject* const thisPointer,
            FORMATETC* const formatEtc)
        {
            PatchedVTableEntry entryValue;
            if (!FindPatchedEntry(thisPointer, &entryValue) || entryValue.originalQueryGetData == nullptr)
            {
                return E_UNEXPECTED;
            }

            const UINT formatValue = (formatEtc != nullptr) ? static_cast<UINT>(formatEtc->cfFormat) : 0U;
            const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Enum);
            if (actionValue == ClipboardPolicyAction::Block)
            {
                ReportClipboardEvent(ClipboardOperationKind::Enum, L"Ole32", L"IDataObject::QueryGetData", formatValue, nullptr, actionValue);
                return DV_E_FORMATETC;
            }

            const HRESULT resultValue = entryValue.originalQueryGetData(thisPointer, formatEtc);
            ReportClipboardEvent(ClipboardOperationKind::Enum, L"Ole32", L"IDataObject::QueryGetData", formatValue, nullptr, actionValue);
            return resultValue;
        }

        HRESULT STDMETHODCALLTYPE HookedDataObjectEnumFormatEtc(
            IDataObject* const thisPointer,
            const DWORD directionValue,
            IEnumFORMATETC** const enumFormatEtcOut)
        {
            PatchedVTableEntry entryValue;
            if (!FindPatchedEntry(thisPointer, &entryValue) || entryValue.originalEnumFormatEtc == nullptr)
            {
                return E_UNEXPECTED;
            }

            const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Enum);
            if (actionValue == ClipboardPolicyAction::Block)
            {
                ReportClipboardEvent(ClipboardOperationKind::Enum, L"Ole32", L"IDataObject::EnumFormatEtc", 0, nullptr, actionValue);
                if (enumFormatEtcOut != nullptr)
                {
                    *enumFormatEtcOut = nullptr;
                }
                return E_NOTIMPL;
            }

            const HRESULT resultValue = entryValue.originalEnumFormatEtc(thisPointer, directionValue, enumFormatEtcOut);
            ReportClipboardEvent(ClipboardOperationKind::Enum, L"Ole32", L"IDataObject::EnumFormatEtc", 0, nullptr, actionValue);
            return resultValue;
        }
    }

    void InstallClipboardDataObjectVTableHooksIfNeeded(IDataObject* const dataObject)
    {
        if (dataObject == nullptr)
        {
            return;
        }
        void** const vtablePointer = *reinterpret_cast<void***>(dataObject);
        if (vtablePointer == nullptr)
        {
            return;
        }

        std::lock_guard<std::mutex> lockGuard(g_vtableMutex);
        if (g_patchedVTables.find(vtablePointer) != g_patchedVTables.end())
        {
            // 同一 COM 类的多个实例通常共享同一份虚表，重复调用是安全的空操作。
            return;
        }

        DWORD oldProtect = 0;
        // 虚表一般落在只读数据段；这里改的是数据指针不是可执行代码，
        // 只需要 PAGE_READWRITE，不需要 PAGE_EXECUTE_READWRITE，也不需要
        // 事后 FlushInstructionCache。
        if (::VirtualProtect(vtablePointer, sizeof(void*) * kVTableSlotCountToProtect, PAGE_READWRITE, &oldProtect) == FALSE)
        {
            return;
        }

        PatchedVTableEntry entryValue;
        entryValue.originalGetData = reinterpret_cast<DataObjectGetDataFn>(vtablePointer[kGetDataSlotIndex]);
        entryValue.originalQueryGetData = reinterpret_cast<DataObjectQueryGetDataFn>(vtablePointer[kQueryGetDataSlotIndex]);
        entryValue.originalEnumFormatEtc = reinterpret_cast<DataObjectEnumFormatEtcFn>(vtablePointer[kEnumFormatEtcSlotIndex]);

        vtablePointer[kGetDataSlotIndex] = reinterpret_cast<void*>(&HookedDataObjectGetData);
        vtablePointer[kQueryGetDataSlotIndex] = reinterpret_cast<void*>(&HookedDataObjectQueryGetData);
        vtablePointer[kEnumFormatEtcSlotIndex] = reinterpret_cast<void*>(&HookedDataObjectEnumFormatEtc);

        DWORD ignoredProtect = 0;
        ::VirtualProtect(vtablePointer, sizeof(void*) * kVTableSlotCountToProtect, oldProtect, &ignoredProtect);

        g_patchedVTables.emplace(vtablePointer, entryValue);
    }

    void UninstallAllClipboardDataObjectVTableHooks()
    {
        std::lock_guard<std::mutex> lockGuard(g_vtableMutex);
        for (const auto& [vtablePointerKey, entryValue] : g_patchedVTables)
        {
            void** const vtablePointer = static_cast<void**>(vtablePointerKey);
            DWORD oldProtect = 0;
            if (::VirtualProtect(vtablePointer, sizeof(void*) * kVTableSlotCountToProtect, PAGE_READWRITE, &oldProtect) == FALSE)
            {
                // 复原失败也不中止：宁可留一份指向本模块的悬挂指针直到该 COM
                // 对象自然析构（会话即将结束、DLL 即将卸载），也不要在这里抛异常。
                continue;
            }
            vtablePointer[kGetDataSlotIndex] = reinterpret_cast<void*>(entryValue.originalGetData);
            vtablePointer[kQueryGetDataSlotIndex] = reinterpret_cast<void*>(entryValue.originalQueryGetData);
            vtablePointer[kEnumFormatEtcSlotIndex] = reinterpret_cast<void*>(entryValue.originalEnumFormatEtc);
            DWORD ignoredProtect = 0;
            ::VirtualProtect(vtablePointer, sizeof(void*) * kVTableSlotCountToProtect, oldProtect, &ignoredProtect);
        }
        g_patchedVTables.clear();
    }
}
