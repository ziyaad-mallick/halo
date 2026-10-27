# Classic and Halo 2 responsiveness work

Base: `6e7fe5f`. Installation and native visual/hardware verification remain separate. The installed shell is not replaced by this work.

## Dock animation graph

`Dock::SetItems` now retains existing expressions when item identity, order, brush and divider membership are unchanged. Running-dot changes still animate and a newly running app still settles its launch bounce. Any leaving entry, changed membership/order or changed brush retains the full rebuild path. An empty initial list also retains initialization.

Actual-source fixture `python tools/dock_update_test.py`: before the fix, eight cases produced three failures; after the fix, zero failures. Fresh dock hit tests pass with Halo 2 both off and on. Fresh Start layout/sampler test passes. This proves avoided expression rebuilding for the covered updates, not a measured frame-rate improvement.

## Correctness and workers

- Dock refresh clears captured press/hover state before releasing capture or replacing app arrays. The extracted refresh-prefix fixture failed before the fix and now passes, including synchronous capture reentry and preservation of another window's capture. Cancellation is conservative: an app-list refresh cancels the pending click.
- Notification removal and clear only hide IDs after Windows reports success. Failed activation or a missing app ID preserves the alert. Nine extracted-handler cases produced five failures before the fix and zero afterward. Errors are logged and retained alerts remain available to retry; no new error banner is added.
- Brightness uses one active writer per burst and coalesces requests to the latest target. Shared-owned state survives detached work. Failed writes remain retryable and successful probe readback updates the deduplication cache. The synthetic blocked-WMI test reproduced three failures and a peak of 100 workers before the fix; now zero failures, peak one worker, at most two writes for that burst. Real monitor behavior remains unverified. WMI method success is checked using [Microsoft's documented zero return value](https://learn.microsoft.com/en-us/windows/win32/wmicoreprov/wmisetbrightness-method-in-class-wmimonitorbrightnessmethods).
- Start recent-file scanning and icon extraction run on one value-only worker. Destruction cancels result delivery without joining slow shell calls. UI results defer while menus are open; device-bound brushes stay on the UI thread. Seven structural checks failed before the change and pass now; an actual-worker-method fixture verifies bounded scanning, cancelled delivery and owned completion. A separate six-case snapshot fixture caught an unnecessary unchanged-content rebuild (one failure before the follow-up, zero after).

## Verification and review artifact

Separate output: `build/halo-responsiveness-2026-10-27.exe`. Build/test logs: `build/responsiveness-build-2026-10-27.log` and `build/responsiveness-tests-2026-10-27.log`. Final execution status is recorded in the shared ops log. No installation or native preview is performed.

## Remaining audit coverage

The broader audit still calls for transition-consistent hit geometry, reliable hotspot completion, notification overflow navigation, keyboard access and mixed-DPI behavior. Cold dock/app icons and initial app-index/avatar work still have synchronous paths. Interrupted dock reveal motion also needs native reproduction before changing its animation logic. These are not resolved by this batch.

No measured frame-rate, latency, CPU or GPU improvement is claimed. Next verification requires a safe native session for both modes, hardware/permission failures, app insertion/removal during input, mixed DPI and frame timing. Passing fixtures and compiling the shell do not establish visual smoothness.
