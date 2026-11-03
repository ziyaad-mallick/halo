#pragma once
// The notification list (Windows' Action Center toasts, read with UserNotificationListener, which works for an
// unpackaged app) kept current for whoever draws it. No window code lives here.
//
// One worker thread sleeps on a change notification for the notification database folder. While the feed is
// inactive a change is re-armed and ignored, so a closed panel costs nothing: SetActive(true) reads the list at
// once. Results come back to the UI thread through a message-only window and `changed` runs there.
//
// HALO_TEST / HALO_NOTIF_DRYRUN: removals and launches are logged, never done. HALO_NOTIF_DEMO: a fixed list of
// fake notifications (for screenshots).

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace halo::notifs {

struct IconPx {
    UINT w = 0, h = 0;
    std::vector<uint32_t> px;  // premultiplied BGRA, at most 64 px
};

struct Notif {
    UINT id = 0;
    std::wstring appKey, aumid, app, title, body;
    long long ft = 0;  // FILETIME of creation
};

class Feed {
public:
    Feed();
    ~Feed();
    Feed(Feed const&) = delete;
    Feed& operator=(Feed const&) = delete;

    // UI thread only, like everything here. `changed` runs on it after the list or the Ready state changed.
    void Start(HINSTANCE inst, std::function<void()> changed);
    void Stop();
    void SetActive(bool on);

    bool Ready() const;  // the first read has come back (with a list or a failure)
    // Newest first, without what was dismissed here and has not left Windows' list yet.
    std::vector<Notif> Items() const;
    IconPx const* Icon(std::wstring const& appKey) const;  // null: draw a letter tile

    void Remove(std::vector<UINT> const& ids);  // the x on a card or group
    void Clear();                               // all of them, in one call
    bool Open(Notif const& n);                  // launches the app and removes the toast; false if it could not

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace halo::notifs
