#include "shell/projection.h"

#include "log.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

namespace atrium {

using namespace winrt;
using namespace winrt::Windows::Devices::Enumeration;
using winrt::Windows::UI::ViewManagement::ProjectionManager;

namespace {
struct DisplaySubscription {
    DeviceWatcher watcher{nullptr};
    event_token added{}, updated{}, removed{}, completed{}, stopped{};
    ~DisplaySubscription()
    {
        if (!watcher) return;
        // Revoke before Stop, outside State::lock: a callback may be finishing.
        try { if (added.value) watcher.Added(added); } catch (...) {}
        try { if (updated.value) watcher.Updated(updated); } catch (...) {}
        try { if (removed.value) watcher.Removed(removed); } catch (...) {}
        try { if (completed.value) watcher.EnumerationCompleted(completed); } catch (...) {}
        try { if (stopped.value) watcher.Stopped(stopped); } catch (...) {}
        try {
            auto status = watcher.Status();
            if (status == DeviceWatcherStatus::Started || status == DeviceWatcherStatus::EnumerationCompleted) watcher.Stop();
        } catch (...) {}
    }
};
}

struct DisplayFinder::State {
    mutable std::mutex lock;
    std::map<std::wstring, std::wstring> found; // id -> name
    std::shared_ptr<DisplaySubscription> subscription;
    HWND hwnd = nullptr;
    UINT msg = 0;
    bool searching = false;
    unsigned gen = 0;
    void Notify(unsigned expected)
    {
        std::lock_guard g(lock);
        if (gen == expected && hwnd)
            PostMessageW(hwnd, msg, 0, 0);
    }
};

DisplayFinder::DisplayFinder() : state_(std::make_shared<State>()) {}

DisplayFinder::~DisplayFinder() { Stop(); }

void DisplayFinder::Start(HWND hwnd, UINT msg)
{
    Stop();
    unsigned gen;
    {
        std::lock_guard g(state_->lock);
        state_->found.clear();
        state_->hwnd = hwnd;
        state_->msg = msg;
        state_->searching = true;
        gen = ++state_->gen;
    }
    std::weak_ptr<State> weak = state_;
    std::thread([weak, gen] {
        init_apartment(apartment_type::multi_threaded);
        try {
            auto subscription = std::make_shared<DisplaySubscription>();
            auto w = DeviceInformation::CreateWatcher(ProjectionManager::GetDeviceSelector());
            subscription->watcher = w;
            auto changed = [weak, gen](auto&& apply) {
                if (auto s = weak.lock()) {
                    {
                        std::lock_guard g(s->lock);
                        if (s->gen != gen)
                            return;
                        apply(*s);
                    }
                    s->Notify(gen);
                }
            };
            subscription->added = w.Added([changed](const DeviceWatcher&, const DeviceInformation& d) {
                std::wstring id(d.Id()), name(d.Name());
                changed([&](State& s) { s.found[id] = name; });
            });
            subscription->updated = w.Updated([](const DeviceWatcher&, const DeviceInformationUpdate&) {}); // needed for the enumeration to complete
            subscription->removed = w.Removed([changed](const DeviceWatcher&, const DeviceInformationUpdate& u) {
                std::wstring id(u.Id());
                changed([&](State& s) { s.found.erase(id); });
            });
            subscription->completed = w.EnumerationCompleted([changed](const DeviceWatcher&, const winrt::Windows::Foundation::IInspectable&) { changed([](State& s) { s.searching = false; }); });
            subscription->stopped = w.Stopped([changed](const DeviceWatcher&, const winrt::Windows::Foundation::IInspectable&) { changed([](State& s) { s.searching = false; }); });
            // Start before publishing. If Stop invalidates this generation while
            // Start is in progress, the unpublished subscription stops itself.
            w.Start();
            if (auto s = weak.lock()) {
                std::lock_guard g(s->lock);
                if (s->gen == gen) s->subscription = subscription;
            }
        } catch (...) {
            Log(L"cast: watching for wireless displays failed");
            if (auto s = weak.lock()) {
                {
                    std::lock_guard g(s->lock);
                    if (s->gen != gen) return;
                    s->searching = false;
                }
                s->Notify(gen);
            }
        }
        // The apartment stays: the watcher lives on in the multithreaded one.
    }).detach();
}

void DisplayFinder::Stop()
{
    std::shared_ptr<DisplaySubscription> subscription;
    {
        std::lock_guard g(state_->lock);
        ++state_->gen;
        state_->hwnd = nullptr;
        state_->searching = false;
        subscription = std::move(state_->subscription);
    }
}

std::vector<WirelessDisplay> DisplayFinder::Displays() const
{
    std::lock_guard g(state_->lock);
    std::vector<WirelessDisplay> list;
    for (const auto& [id, name] : state_->found)
        list.push_back({id, name});
    std::sort(list.begin(), list.end(), [](const WirelessDisplay& a, const WirelessDisplay& b) { return a.name < b.name; });
    return list;
}

bool DisplayFinder::Searching() const
{
    std::lock_guard g(state_->lock);
    return state_->searching;
}

bool ProjectTo(const std::wstring& id)
{
    try {
        DeviceInformation d = DeviceInformation::CreateFromIdAsync(id).get();
        // No views of our own: Windows projects the desktop.
        ProjectionManager::StartProjectingAsync(0, 0, d).get();
        Log(L"cast: projecting");
        return true;
    } catch (const hresult_error& e) {
        Log(L"cast: projecting failed (0x%08X)", unsigned(e.code()));
    } catch (...) {
        Log(L"cast: projecting failed");
    }
    return false;
}

bool StopProjecting()
{
    try {
        ProjectionManager::StopProjectingAsync(0, 0).get();
        Log(L"cast: stopped");
        return true;
    } catch (...) {
        Log(L"cast: stopping failed");
    }
    return false;
}

ProjectionMode CurrentProjectionMode(int* displays)
{
    ProjectionMode mode = ProjectionMode::Unknown;
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &paths, &modes) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
        std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
        DISPLAYCONFIG_TOPOLOGY_ID topology{};
        if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &paths, p.data(), &modes, m.data(), &topology) == ERROR_SUCCESS) {
            switch (topology) {
            case DISPLAYCONFIG_TOPOLOGY_INTERNAL:
                mode = ProjectionMode::Internal;
                break;
            case DISPLAYCONFIG_TOPOLOGY_CLONE:
                mode = ProjectionMode::Clone;
                break;
            case DISPLAYCONFIG_TOPOLOGY_EXTEND:
                mode = ProjectionMode::Extend;
                break;
            case DISPLAYCONFIG_TOPOLOGY_EXTERNAL:
                mode = ProjectionMode::External;
                break;
            default:
                break;
            }
        }
    }
    if (displays) {
        // Connected targets, whether in use or not.
        std::set<std::tuple<LONG, DWORD, UINT32>> targets;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &paths, &modes) == ERROR_SUCCESS) {
            std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
            std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
            if (QueryDisplayConfig(QDC_ALL_PATHS, &paths, p.data(), &modes, m.data(), nullptr) == ERROR_SUCCESS)
                for (UINT32 i = 0; i < paths; ++i)
                    if (p[i].targetInfo.targetAvailable)
                        targets.insert({p[i].targetInfo.adapterId.HighPart, p[i].targetInfo.adapterId.LowPart, p[i].targetInfo.id});
        }
        *displays = int(targets.size());
    }
    return mode;
}

bool SetProjectionMode(ProjectionMode mode)
{
    UINT32 flag = mode == ProjectionMode::Internal ? SDC_TOPOLOGY_INTERNAL
                  : mode == ProjectionMode::Clone  ? SDC_TOPOLOGY_CLONE
                  : mode == ProjectionMode::Extend ? SDC_TOPOLOGY_EXTEND
                  : mode == ProjectionMode::External ? SDC_TOPOLOGY_EXTERNAL
                                                     : 0;
    if (!flag)
        return false;
    LONG r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | flag);
    Log(L"cast: projection mode %d (%ld)", int(mode), r);
    return r == ERROR_SUCCESS;
}

} // namespace atrium
