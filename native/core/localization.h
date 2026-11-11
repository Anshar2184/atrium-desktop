// The interface's language (SETTINGS-PROTOCOL: `language`). Chinese is the
// source language: every user-visible literal is written in Chinese and Tr()
// looks up its English text when English is active. Strings that are not in
// the table fall back to Chinese, so a missing translation never blanks a
// label. TrFormat() does the same for printf-style templates.
#pragma once

#include <string>

namespace atrium {

// "auto" follows Windows' UI language (Chinese -> zh, anything else -> en);
// "zh" and "en" select the language explicitly. Unknown values act as "auto".
// Returns true when the active language changed.
bool SetLanguage(const std::wstring& setting);
bool LanguageIsEnglish();

// The text for `chinese` in the active language.
const wchar_t* Tr(const wchar_t* chinese);

// Formats a localized template (printf syntax) into a string.
std::wstring TrFormat(const wchar_t* chinese, ...);

} // namespace atrium
