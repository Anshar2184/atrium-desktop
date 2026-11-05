#pragma once

#include <string>

namespace atrium {

inline constexpr wchar_t kAppId[] = L"Atrium.Desktop";

// Only a complete distribution (including its uninstaller) registers with Windows.
void RegisterApplication(const std::wstring& directory);

} // namespace atrium
