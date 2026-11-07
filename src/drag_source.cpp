#include "drag_source.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstring>
#include <new>
#include <utility>

namespace {

class FileDataObject : public IDataObject {
public:
    explicit FileDataObject(std::vector<std::wstring> paths)
        : m_paths(std::move(paths))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_INVALIDARG;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDataObject)
        {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_ref);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0)
            delete this;
        return (ULONG)n;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fmt, STGMEDIUM* med) override
    {
        if (!fmt || !med)
            return E_INVALIDARG;
        if (fmt->cfFormat != CF_HDROP || !(fmt->tymed & TYMED_HGLOBAL))
            return DV_E_FORMATETC;
        if (fmt->dwAspect != DVASPECT_CONTENT)
            return DV_E_DVASPECT;
        if (fmt->lindex != -1)
            return DV_E_LINDEX;

        size_t chars = 1;
        for (const auto& p : m_paths)
            chars += p.size() + 1;

        HGLOBAL h = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
        if (!h)
            return E_OUTOFMEMORY;

        DROPFILES* df = (DROPFILES*)GlobalLock(h);
        if (!df)
        {
            GlobalFree(h);
            return E_OUTOFMEMORY;
        }

        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;

        wchar_t* dst = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
        for (const auto& p : m_paths)
        {
            memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
            dst += p.size();
            *dst++ = L'\0';
        }
        *dst = L'\0';
        GlobalUnlock(h);

        med->tymed = TYMED_HGLOBAL;
        med->hGlobal = h;
        med->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fmt) override
    {
        if (!fmt)
            return E_INVALIDARG;
        if (fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL) &&
            fmt->dwAspect == DVASPECT_CONTENT && fmt->lindex == -1)
            return S_OK;
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,
                                                    FORMATETC* out) override
    {
        if (out)
            out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir,
                                            IEnumFORMATETC** out) override
    {
        if (!out)
            return E_INVALIDARG;
        *out = nullptr;
        if (dir != DATADIR_GET)
            return E_NOTIMPL;

        FORMATETC fmt{};
        fmt.cfFormat = (CLIPFORMAT)CF_HDROP;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;
        return SHCreateStdEnumFmtEtc(1, &fmt, out);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*,
                                      DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~FileDataObject() = default;

    LONG m_ref = 1;
    std::vector<std::wstring> m_paths;
};

class FileDropSource : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_INVALIDARG;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropSource)
        {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_ref);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0)
            delete this;
        return (ULONG)n;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape,
                                                DWORD keyState) override
    {
        if (escape)
            return DRAGDROP_S_CANCEL;
        if (!(keyState & MK_LBUTTON))
            return DRAGDROP_S_DROP;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ~FileDropSource() = default;

    LONG m_ref = 1;
};

} // namespace

HRESULT ImageEdit_DoDragDrop(HWND owner, const std::vector<std::wstring>& paths)
{
    (void)owner;
    if (paths.empty())
        return E_INVALIDARG;

    FileDataObject* data = new (std::nothrow) FileDataObject(paths);
    if (!data)
        return E_OUTOFMEMORY;

    FileDropSource* source = new (std::nothrow) FileDropSource();
    if (!source)
    {
        data->Release();
        return E_OUTOFMEMORY;
    }

    DWORD effect = 0;
    HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_MOVE |
                                             DROPEFFECT_LINK, &effect);

    source->Release();
    data->Release();
    return hr;
}
