#pragma once

#include <string>

namespace atrium {

inline constexpr wchar_t kAppId[] = L"Atrium.Desktop";

bool IsRegisteredApplication(const std::wstring& directory);
// Development builds never register, including complete development packages.
void RegisterApplication(const std::wstring& directory);

} // namespace atrium
