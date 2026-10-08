#pragma once
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <atomic>

// A real UIA provider served through WM_GETOBJECT. This makes logical-tree
// traversal testable on a non-input desktop, where system HWND proxies can
// legitimately omit children. It has no action implementations.
class InspectionFixtureProvider final : public IRawElementProviderSimple,
    public IRawElementProviderFragment, public IRawElementProviderFragmentRoot
{
public:
    inline static std::atomic<int> childNavigations{0};
    inline static std::atomic<int> propertiesRead{0};
    InspectionFixtureProvider(HWND root, HWND button, HWND edit, int index = 0, InspectionFixtureProvider* tree = nullptr)
        : m_root(root), m_button(button), m_edit(edit), m_index(index), m_tree(tree ? tree : this)
    { if (m_tree != this) m_tree->AddRef(); }
    ~InspectionFixtureProvider() { if (m_tree != this) m_tree->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IRawElementProviderSimple))
            *result = static_cast<IRawElementProviderSimple*>(this);
        else if (id == __uuidof(IRawElementProviderFragment))
            *result = static_cast<IRawElementProviderFragment*>(this);
        else if (id == __uuidof(IRawElementProviderFragmentRoot) && m_index == 0)
            *result = static_cast<IRawElementProviderFragmentRoot*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG refs = --m_refs; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override
    { *options = ProviderOptions_ServerSideProvider; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** result) override
    { *result = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* result) override
    {
        if (m_index) ++propertiesRead;
        ::VariantInit(result);
        switch (property)
        {
        case UIA_ControlTypePropertyId:
            result->vt = VT_I4; result->lVal = m_index == 0 ? UIA_WindowControlTypeId
                : m_index == 1 ? UIA_ButtonControlTypeId : UIA_EditControlTypeId; break;
        case UIA_NamePropertyId:
            result->vt = VT_BSTR; result->bstrVal = ::SysAllocString(m_index == 0 ? L"Inspection fixture"
                : m_index == 1 ? L"Accept" : L"Read only inspection"); break;
        case UIA_LocalizedControlTypePropertyId:
            result->vt = VT_BSTR; result->bstrVal = ::SysAllocString(m_index == 0 ? L"window"
                : m_index == 1 ? L"button" : L"edit"); break;
        case UIA_FrameworkIdPropertyId:
            result->vt = VT_BSTR; result->bstrVal = ::SysAllocString(L"InspectionFixture"); break;
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId:
        case UIA_IsEnabledPropertyId:
            result->vt = VT_BOOL; result->boolVal = VARIANT_TRUE; break;
        case UIA_IsOffscreenPropertyId:
        case UIA_IsPasswordPropertyId:
            result->vt = VT_BOOL; result->boolVal = VARIANT_FALSE; break;
        case UIA_ProcessIdPropertyId:
            result->vt = VT_I4; result->lVal = static_cast<LONG>(::GetCurrentProcessId()); break;
        case UIA_NativeWindowHandlePropertyId:
            // Children are logical controls with no HWND of their own.
            result->vt = VT_I4; result->lVal = m_index ? 0 : static_cast<LONG>(reinterpret_cast<LONG_PTR>(m_root)); break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** provider) override
    { *provider = nullptr; return m_index ? S_OK : ::UiaHostProviderFromHwnd(m_root, provider); }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** provider) override
    {
        if (direction == NavigateDirection_FirstChild) ++childNavigations;
        *provider = nullptr;
        int index = -1;
        if (m_index == 0 && direction == NavigateDirection_FirstChild) index = 1;
        if (m_index == 0 && direction == NavigateDirection_LastChild) index = 2;
        if (m_index && direction == NavigateDirection_Parent) index = 0;
        if (m_index == 1 && direction == NavigateDirection_NextSibling) index = 2;
        if (m_index == 2 && direction == NavigateDirection_PreviousSibling) index = 1;
        if (index == 0) { *provider = m_tree; m_tree->AddRef(); }
        else if (index >= 0) *provider = new InspectionFixtureProvider(m_root, m_button, m_edit, index, m_tree);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** ids) override
    {
        *ids = nullptr;
        if (!m_index) return S_OK;
        *ids = ::SafeArrayCreateVector(VT_I4, 0, 2);
        if (!*ids) return E_OUTOFMEMORY;
        LONG index = 0; int append = UiaAppendRuntimeId;
        ::SafeArrayPutElement(*ids, &index, &append);
        index = 1; ::SafeArrayPutElement(*ids, &index, &m_index); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* bounds) override
    {
        RECT rect{}; ::GetWindowRect(m_index == 0 ? m_root : m_index == 1 ? m_button : m_edit, &rect);
        *bounds = UiaRect{static_cast<double>(rect.left), static_cast<double>(rect.top),
            static_cast<double>(rect.right - rect.left), static_cast<double>(rect.bottom - rect.top)};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** roots) override { *roots = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** root) override
    { *root = m_tree; m_tree->AddRef(); return S_OK; }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** provider) override
    {
        int selected = 0;
        for (int index = 1; index <= 2; ++index) {
            RECT bounds{}; ::GetWindowRect(index == 1 ? m_button : m_edit, &bounds);
            if (x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom) selected = index;
        }
        if (!selected) { *provider = m_tree; m_tree->AddRef(); }
        else *provider = new InspectionFixtureProvider(m_root, m_button, m_edit, selected, m_tree);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** provider) override { *provider = nullptr; return S_OK; }
private:
    std::atomic<ULONG> m_refs{1};
    HWND m_root, m_button, m_edit;
    int m_index;
    InspectionFixtureProvider* m_tree;
};
