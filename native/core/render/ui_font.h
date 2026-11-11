// The interface face (D40): MiSans, shipped with the settings app (Settings\Fonts)
// and loaded privately (never installed). Its weights run light: Medium
// (380) reads as a body text, Demibold (450) as emphasis, Semibold (520) as
// titles, so callers keep asking for the usual weights and UiTextFormat maps
// them. Digits are tabular wherever they are drawn (a clock or a level must
// not shift as it changes). Without the files: Segoe UI Variable Text, with
// Chinese from the system's fallback, as before.
#pragma once

#include <d2d1.h>
#include <dwrite_3.h>

#include <string>

namespace atrium {

// The icon glyphs' face: Segoe Fluent Icons (Windows 11), else Segoe MDL2 Assets (Windows 10 and
// systems without it), which has every glyph Atrium draws at the same code points.
const wchar_t* IconFontFamily();

// A text format in the interface face at `px`, for one of the usual weights.
HRESULT UiTextFormat(IDWriteFactory* dw, float px, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out);

// A layout of `s` in `f`, its digits tabular.
HRESULT UiTextLayout(IDWriteFactory* dw, const std::wstring& s, IDWriteTextFormat* f, float width, float height, IDWriteTextLayout** out);

// The width of `s` laid out in one line at `px` (px).
float MeasureUiText(IDWriteFactory* dw, const std::wstring& s, float px, DWRITE_FONT_WEIGHT weight);

// DrawText, with tabular digits when `s` has any.
void DrawUiText(ID2D1RenderTarget* rt, IDWriteFactory* dw, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, ID2D1Brush* b,
                D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE);

} // namespace atrium
