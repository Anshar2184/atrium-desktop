#include "ui/thumbnail_thread.h"

#include "log.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace atrium {

struct ThumbnailThread::State {
    enum class Kind { Register, Update, Unregister };
    struct Op {
        Kind kind;
        uint64_t id;
        HWND destination = nullptr, source = nullptr;
    };
    HWND notify;
    UINT message;
    mutable std::mutex lock;
    std::condition_variable wake, done;
    std::deque<Op> ops;
    std::unordered_map<uint64_t, DWM_THUMBNAIL_PROPERTIES> latest; // the update waiting for each picture
    std::unordered_set<uint64_t> queued;                           // pictures with an update in `ops`
    std::unordered_map<uint64_t, SIZE> sizes;
    uint64_t next = 0;
    bool stop = false, finished = false;

    void Run()
    {
        std::unordered_map<uint64_t, HTHUMBNAIL> handles; // the worker's own
        for (;;) {
            Op op;
            DWM_THUMBNAIL_PROPERTIES props{};
            {
                std::unique_lock g(lock);
                wake.wait(g, [&] { return stop || !ops.empty(); });
                if (stop && ops.empty())
                    break;
                op = ops.front();
                ops.pop_front();
                if (op.kind == Kind::Update) {
                    queued.erase(op.id);
                    auto it = latest.find(op.id);
                    if (it == latest.end())
                        continue; // unregistered meanwhile
                    props = it->second;
                    latest.erase(it);
                }
            }
            switch (op.kind) {
            case Kind::Register: {
                HTHUMBNAIL h = nullptr;
                SIZE size{};
                if (SUCCEEDED(DwmRegisterThumbnail(op.destination, op.source, &h))) {
                    handles[op.id] = h;
                    DwmQueryThumbnailSourceSize(h, &size);
                }
                {
                    std::lock_guard g(lock);
                    sizes[op.id] = size;
                }
                PostMessageW(notify, message, WPARAM(op.id), 0);
                break;
            }
            case Kind::Update:
                if (auto it = handles.find(op.id); it != handles.end())
                    DwmUpdateThumbnailProperties(it->second, &props);
                break;
            case Kind::Unregister:
                if (auto it = handles.find(op.id); it != handles.end()) {
                    DwmUnregisterThumbnail(it->second);
                    handles.erase(it);
                }
                break;
            }
        }
        for (auto& [id, h] : handles)
            DwmUnregisterThumbnail(h);
        std::lock_guard g(lock);
        finished = true;
        done.notify_all();
    }
};

ThumbnailThread::ThumbnailThread(HWND notify, UINT message) : state_(std::make_shared<State>())
{
    state_->notify = notify;
    state_->message = message;
    std::thread([state = state_] { state->Run(); }).detach();
}

ThumbnailThread::~ThumbnailThread()
{
    std::unique_lock g(state_->lock);
    state_->stop = true;
    state_->wake.notify_all();
    // A DWM that does not answer at all must not keep Atrium from exiting: the thread holds its
    // own reference to the state and finishes (or not) on its own.
    if (!state_->done.wait_for(g, std::chrono::milliseconds(1500), [&] { return state_->finished; }))
        Log(L"thumbnails: DWM did not answer; left behind");
}

uint64_t ThumbnailThread::Register(HWND destination, HWND source)
{
    std::lock_guard g(state_->lock);
    uint64_t id = ++state_->next;
    state_->ops.push_back({State::Kind::Register, id, destination, source});
    state_->wake.notify_one();
    return id;
}

void ThumbnailThread::Update(uint64_t id, const DWM_THUMBNAIL_PROPERTIES& p)
{
    std::lock_guard g(state_->lock);
    state_->latest[id] = p;
    if (state_->queued.insert(id).second) {
        state_->ops.push_back({State::Kind::Update, id});
        state_->wake.notify_one();
    }
}

void ThumbnailThread::Unregister(uint64_t id)
{
    std::lock_guard g(state_->lock);
    state_->latest.erase(id);
    state_->sizes.erase(id);
    state_->ops.push_back({State::Kind::Unregister, id});
    state_->wake.notify_one();
}

SIZE ThumbnailThread::SourceSize(uint64_t id) const
{
    std::lock_guard g(state_->lock);
    auto it = state_->sizes.find(id);
    return it != state_->sizes.end() ? it->second : SIZE{};
}

} // namespace atrium
