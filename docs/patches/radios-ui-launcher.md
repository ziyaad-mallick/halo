# radios-ui-launcher: wiring the 2.0 right column (`src/launcher.cpp`) to the new radio services

For whoever is redesigning the Start panel. Not applied here because `src/launcher.cpp`, `launcher.h` and
`start_widgets.h` are being changed in another worktree. Written against the services on branch `fix/radios`
(`src/services.h`); the classic panel (`src/control_center.cpp`) already uses all of it, so read that file for a working
example of each change below (search it for `LabelRows`, `RowIndex`, `kMsgOutputs`, `BtDev`, `OutDev`).
Everything is described by function and by the code to look for, not by line number.

## What the services guarantee now (so the UI can drop its workarounds)

- Every `svc::` call named here returns at once and is safe from the UI thread. Nothing blocks on WLAN, WinRT radio,
  Bluetooth, audio-endpoint or registry-broadcast calls any more.
- The Wi-Fi list, the Bluetooth list and the output list are cached in the services and survive `WifiStop()`, `BtStop()`
  and `OutputsStop()` (the Stop calls only stop notifications / cancel a scan). `WifiNets()`, `BtDevices()`,
  `OutputDevicesCached()` therefore return the last rows instantly, and `WifiInfo()`, `BtInfo()`, `OutputsInfo()` say
  whether data exists (`hasData`), whether a refresh is running (`refreshing`) and how old it is (`AgeMs()`).
- `WifiRefresh(hwnd, msg)` / `BtRefresh(hwnd, msg)` set the status to `Loading` before they return and post `msg`
  immediately, so the page can re-read the cached rows on the next message. A refresh that is already running is joined,
  so pressing Refresh repeatedly does not stack scans.
- `Radios::WifiOn()` / `BluetoothOn()` return the requested state while a change is pending; `WifiPending()` /
  `BluetoothPending()` say it is pending. `Radios` finds the radios once per process, so the tiles no longer say
  "Checking…" and ignore clicks after every Start-panel open.
- Rows carry a stable id: `WifiNet::id`, `BtDevice::id`, `Device::id` (audio endpoint id). `WifiNet::pending` and
  `BtDevice::pending` are true while a connect is in flight. `BtDevice::audio / audioActive / audioDefault /
  endpointId` describe the device's audio endpoint (see "Bluetooth device click").
- `svc::CachedSsid()` is the connected network's name without blocking; `Radios::Start()` keeps it current and posts
  the radios message when it changes. `svc::ConnectedSsid()` still exists but is a 20 ms blocking read: do not call it
  from the UI thread.

## Changes, by function

### 1. `Launcher::StartServices` and `StopServices`

- `StartServices`: after `m_radios.Start(m_hwnd, kMsgRadios)` nothing else is needed for the SSID. Add a new message id
  next to `kMsgWifi` / `kMsgBt` (for example `kMsgOutputs = WM_APP + 19`; check it is free) and, if the output page
  should be live, do not start the output list here: it is started when the page opens (below).
- `StopServices`: next to `svc::WifiStop(); svc::BtStop();` add `svc::OutputsStop();`.

### 2. `Launcher::OnServiceMessage`

- `case kMsgRadios:` (the one that sets `m_wifiOn`, `m_btOn`, `m_ssid`): replace
  `m_ssid = m_wifiOn ? svc::ConnectedSsid() : std::wstring();` with `svc::CachedSsid()`.
- `case kMsgWifi:` same replacement for `m_ssid`; keep `m_wifiRows = svc::WifiNets();`.
- `case kMsgBt:` keep `m_btRows = svc::BtDevices();`.
- Add `case kMsgOutputs: m_outs = svc::OutputDevicesCached(); ...` then `LayoutRight(); RedrawRight();` (only the redraw
  if the row count did not change). Route it in `Launcher::OnMessage` where the `m >= kMsgAudio && m <= kMsgMic`
  range is tested: the new id is outside that range, so test it separately.
- For `kMsgWifi`, `kMsgBt` and `kMsgOutputs`: call `LayoutRight()` only when the row count changed (it re-clamps
  `m_ccScroll`); otherwise `RedrawRight()` alone, so a data update does not rebuild the hit list under the pointer.

### 3. `Launcher::ToggleDetail` (the function that switches `m_detail` and starts the scans)

Look for `m_wifiRows.clear(); svc::WifiRefresh(...)`, `m_btRows.clear(); svc::BtRefresh(...)` and
`m_outs = svc::OutputDevices();`. Replace by:

- Wi-Fi: `m_wifiRows = svc::WifiNets(); svc::WifiRefresh(m_hwnd, kMsgWifi);`
- Bluetooth: `m_btRows = svc::BtDevices(); svc::BtRefresh(m_hwnd, kMsgBt);`
- Output: `m_outs = svc::OutputDevicesCached(); svc::OutputsRefresh(m_hwnd, kMsgOutputs);`
  (the synchronous `OutputDevices()` was a property-store read per endpoint on the UI thread).
- In the branches that leave a page (the `svc::WifiStop(); svc::BtStop();` lines) add `svc::OutputsStop();`.

The page now opens with the last rows and updates in place; the services post the changes.

### 4. Press identity (`WM_LBUTTONDOWN` / `WM_LBUTTONUP` in `Launcher::OnMessage`, the `m_pressedIndex` code)

Today the press key is `kind * 1000 + index`. For `Kind::CcRow` the index is the row's position, so a reorder between
press and release can click a different device. Add a member `std::wstring m_pressedRowKey`:

- at mouse-down, when `hit->kind == Kind::CcRow`, store the id of the row it stands for (`m_wifiRows[i].id`,
  `m_btRows[i].id` or `m_outs[i].id`, chosen by `m_detail`); clear it otherwise;
- at mouse-up, for `Kind::CcRow` compare that id with the id of the row now under the pointer instead of comparing the
  index; and in `CcClick`, look the row up by id (a `find` over the rows) rather than by `hit.index`; ignore the click
  if it is no longer there.
- Do not reset the press when data arrives (the 2.0 column does not today: keep it that way). The classic panel did
  (`pressed = {}` in its kMsgBt / kMsgWifi cases) and that is what made clicks feel ignored while devices streamed in.

### 5. `Launcher::CcClick`

- `case Kind::CcRefresh:` look for `m_wifiRows.clear();` / `m_btRows.clear();` just before `svc::WifiRefresh(...)` /
  `svc::BtRefresh(...)`: delete both lines. The early return for `Loading` / `Connecting` can stay.
- `case Kind::CcRow:` Wi-Fi branch (`svc::WifiConnect(m_wifiRows[hit.index])`): find the row by id; ignore if
  `row.pending`; call `svc::WifiConnect(copyOfRow)`; then `m_wifiRows = svc::WifiNets();` so the row shows its pending
  state. `WifiConnect` returns at once; the WLAN calls run on a worker.
- Output branch (the one calling `svc::SetDefaultOutput(id)` and then `m_speaker.Bind(...)`): replace by
  `svc::SetDefaultAudioOutput(id, m_hwnd, kMsgOutputs);`, keep the optimistic `isDefault` flip, and delete the
  immediate `m_speaker.Bind(eRender, m_hwnd, kMsgAudio)` plus the `m_vol` / `m_muted` re-read: the default-device
  change posts `kMsgAudio` with `wParam == AudioEndpoint::kDeviceChanged`, and `Launcher::OnMessage` already calls
  `m_speaker.OnMessage(w)` for it, which rebinds. (`SetDefaultOutput` itself still works and is now fire-and-forget.)
- Bluetooth branch (the one that does `Hide(); OpenQuietly(L"ms-settings:bluetooth");`): replace by, with `d` the row
  found by id:
  1. `d.pending`: return.
  2. `d.audio && !d.audioActive` (a paired audio device that is not connected):
     `svc::ConnectBluetoothAudio(d.id, m_hwnd, kMsgBt); m_btRows = svc::BtDevices();` (the row now has `pending`).
  3. `d.audio && d.audioActive && !d.audioDefault && !d.endpointId.empty()` (connected, not the output):
     `svc::SetDefaultAudioOutput(d.endpointId, m_hwnd, kMsgOutputs);` and flip `isDefault` in `m_outs`.
  4. `d.audio && d.audioActive && d.audioDefault`: nothing to do.
  5. `!d.audio` (a mouse, a keyboard, anything without an audio endpoint): keep `Hide(); OpenQuietly(L"ms-settings:bluetooth");`.
  `ConnectBluetoothAudio` falls back to opening `ms-settings:bluetooth` itself on any failure, so the UI needs no
  failure path. `svc::DisconnectBluetoothAudio(d.id, ...)` exists if a "Disconnect" affordance is wanted.
- Radio toggles (`case Kind::CcWifi` / `CcBt` and the airplane case): no change to the calls. They are non-blocking and
  `m_radios.WifiOn()` now holds the requested state, so the tile does not flicker back.

### 6. `Launcher::DrawRight`

- Tile sub-label (the `std::wstring sub = ...` expression for the Wi-Fi / Bluetooth tiles; `tools/radio_label_test.py`
  executes this expression, so run it after editing): after computing `sub`, when the radio is available and
  `m_radios.WifiPending()` (or `BluetoothPending()`), show `L"Turning on…"` / `L"Turning off…"` according to the
  requested state. Keep the existing startup labels the test checks.
- Refresh pill (`case Kind::CcRefresh`): `loading` is already true for `Loading`; with cached rows on screen, read
  "Updating…" instead of "Scanning…" when `total > 0`, or use `svc::WifiInfo().refreshing` / `svc::BtInfo().refreshing`.
  Optionally show the age: `Info().AgeMs()`.
- The list's message line (the `switch (status.phase)` in the branch that draws a list's message): in `Loading` show
  `L"Updating…"` when rows exist, the existing "Looking for…" text only when the list is empty. The Bluetooth hint
  "pairing and connecting are in Windows Settings" is no longer true for audio devices; say, for example, "Select
  earphones to connect or switch to them".
- `case Kind::CcRow`: Wi-Fi row: if `n.pending` draw `Connecting…` where the lock / tick glyph goes. Bluetooth row:
  right-hand text `d.pending ? "Connecting…" : d.connected ? (d.audio && !d.audioDefault ? "Switch to" : "Connected") :
  d.audio ? "Connect" : "Paired"`.

### 7. Optional clean-ups that the services make possible

- Any `std::thread` hop around `svc::SetHotspot`, `svc::SetDarkMode`, `svc::SetDefaultOutput`, `svc::WifiConnect`,
  `m_radios.SetWifi/SetBluetooth`, `svc::ConnectedSsid` can go; they no longer block. Keep a hop around
  `svc::HotspotAvailable()` / `svc::ReadHotspot()` (still blocking reads).

## What to re-run after applying

`python tools/radio_label_test.py` (executes the tile label expression), `python tools/cc_radio_settings_test.py`
(checks that the radio settings control and the two ms-settings destinations are still in launcher.cpp), then a
`HALO_OUT=build/halo-radios.exe bash tools/build.sh halo`. Nothing here needs a window or a real radio to compile.

## Not verified

None of this runtime behaviour has been exercised: see the radios report for the order in which the owner should try it.
