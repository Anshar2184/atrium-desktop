#include "render/ui_font.h"

#include "log.h"

#include <windows.h>

#include <winrt/base.h>

#include <algorithm>
#include <mutex>

namespace atrium {

namespace {

struct Face {
    winrt::com_ptr<IDWriteFontCollection> collection; // null: the system's fonts
    winrt::com_ptr<IDWriteTypography> tabular;
};

const Face& TheFace()
{
    static Face face;
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
        std::wstring base = exe;
        base = base.substr(0, base.find_last_of(L'\\') + 1);
        // The package keeps one copy, with the settings app; a development build has its own.
        std::wstring dir;
        for (const wchar_t* sub : {L"Settings\\Fonts\\", L"Fonts\\"})
            if (GetFileAttributesW((base + sub + L"MiSans-Medium.otf").c_str()) != INVALID_FILE_ATTRIBUTES) {
                dir = base + sub;
                break;
            }
        if (dir.empty()) {
            Log(L"font: no MiSans next to the core, using Segoe UI");
            return;
        }
        winrt::com_ptr<IDWriteFactory5> dw;
        winrt::com_ptr<IDWriteFontSetBuilder1> builder;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), reinterpret_cast<IUnknown**>(dw.put()))) ||
            FAILED(dw->CreateFontSetBuilder(builder.put())))
            return;
        for (const wchar_t* name : {L"MiSans-Medium.otf", L"MiSans-Demibold.otf", L"MiSans-Semibold.otf"}) {
            winrt::com_ptr<IDWriteFontFile> file;
            if (FAILED(dw->CreateFontFileReference((dir + name).c_str(), nullptr, file.put())) || FAILED(builder->AddFontFile(file.get()))) {
                Log(L"font: %s unreadable in %s, using Segoe UI", name, dir.c_str());
                return;
            }
        }
        winrt::com_ptr<IDWriteFontSet> set;
        winrt::com_ptr<IDWriteFontCollection1> collection;
        if (FAILED(builder->CreateFontSet(set.put())) || FAILED(dw->CreateFontCollectionFromFontSet(set.get(), collection.put())))
            return;
        UINT32 index = 0;
        BOOL exists = FALSE;
        collection->FindFamilyName(L"MiSans", &index, &exists);
        if (!exists)
            return;
        winrt::com_ptr<IDWriteTypography> tabular;
        if (SUCCEEDED(dw->CreateTypography(tabular.put())))
            tabular->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
        face.collection = collection;
        face.tabular = tabular;
        Log(L"font: MiSans");
    });
    return face;
}

} // namespace

HRESULT UiTextFormat(IDWriteFactory* dw, float px, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out)
{
    const Face& face = TheFace();
    if (!face.collection)
        return dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", out);
    DWRITE_FONT_WEIGHT w = weight <= DWRITE_FONT_WEIGHT_NORMAL     ? DWRITE_FONT_WEIGHT(380)
                           : weight <= DWRITE_FONT_WEIGHT_MEDIUM ? DWRITE_FONT_WEIGHT(450)
                                                                   : DWRITE_FONT_WEIGHT(520);
    return dw->CreateTextFormat(L"MiSans", face.collection.get(), w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"zh-cn", out);
}

HRESULT UiTextLayout(IDWriteFactory* dw, const std::wstring& s, IDWriteTextFormat* f, float width, float height, IDWriteTextLayout** out)
{
    HRESULT hr = dw->CreateTextLayout(s.c_str(), UINT32(s.size()), f, width, height, out);
    if (SUCCEEDED(hr) && TheFace().tabular)
        (*out)->SetTypography(TheFace().tabular.get(), {0, UINT32(s.size())});
    return hr;
}

float MeasureUiText(IDWriteFactory* dw, const std::wstring& s, float px, DWRITE_FONT_WEIGHT weight)
{
    winrt::com_ptr<IDWriteTextFormat> f;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(UiTextFormat(dw, px, weight, f.put())) || FAILED(UiTextLayout(dw, s, f.get(), 100000, 1000, layout.put())))
        return px * float(s.size()) * 0.6f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void DrawUiText(ID2D1RenderTarget* rt, IDWriteFactory* dw, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, ID2D1Brush* b,
                D2D1_DRAW_TEXT_OPTIONS options)
{
    bool digits = std::any_of(s.begin(), s.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!digits || !TheFace().tabular || FAILED(UiTextLayout(dw, s, f, r.right - r.left, r.bottom - r.top, layout.put()))) {
        rt->DrawText(s.c_str(), UINT32(s.size()), f, r, b, options);
        return;
    }
    rt->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.get(), b, options);
}

} // namespace atrium
