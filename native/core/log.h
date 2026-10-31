// Minimal append-only log at %LOCALAPPDATA%\Atrium\logs\core.log.
#pragma once

namespace atrium {

void Log(const wchar_t* format, ...);

} // namespace atrium
