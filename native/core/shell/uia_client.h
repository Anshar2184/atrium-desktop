// A UI Automation client for a reader thread (WeChat, Feishu). Right after
// Atrium starts, one of the two often fails to make its condition (E_FAIL),
// which used to end that reader for the session: a failure is retried every
// two seconds (logged once).
#pragma once

#include "log.h"

#include <windows.h>
#include <uiautomation.h>

#include <winrt/base.h>

#include <atomic>

namespace atrium {

// `prepare` makes what the reader needs from the client (its conditions) and returns an HRESULT.
template <typename Prepare>
winrt::com_ptr<IUIAutomation> CreateUia(const std::atomic<bool>& stop, HANDLE wake, const wchar_t* who, Prepare prepare)
{
    for (bool logged = false; !stop; WaitForSingleObject(wake, 2000)) {
        winrt::com_ptr<IUIAutomation> uia;
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()));
        if (SUCCEEDED(hr))
            hr = prepare(uia.get());
        if (SUCCEEDED(hr))
            return uia;
        if (!logged)
            Log(L"%s: UI Automation set-up failed (0x%08X), retrying", who, unsigned(hr));
        logged = true;
    }
    return nullptr;
}

} // namespace atrium
