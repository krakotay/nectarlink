# Mirroring with the physical screen off

## Two different states

The mirror's screen-off option blanks the physical panel while a wake lock keeps
the screen pipeline active. Pressing the phone's power button or letting it time
out can instead lock the default display. A whole-screen mirror must continue to
respect that lock screen. Elevated app windows use their own trusted, unlocked
virtual displays and may stay active while the default display is locked.

## Unicode input

The previous helper tried `findFocus(FOCUS_INPUT)` on an accessibility window and
required `isEditable`. Any missing focus, stale node, missing editable flag, or
rejected `ACTION_SET_TEXT` silently selected the clipboard fallback. Android can
deny an application's clipboard reads while the phone is locked, even if its
virtual app display remains active. This explains how ASCII key events can keep
working while Cyrillic text stops. The old helper did not log which direct-input
condition failed, so the exact failing condition in the reported apps remains
unconfirmed until the revised helper can be checked on a device.

The helper now starts accessibility window tracking before launching the app,
clears cached window/node data before an edit, and restricts the search to the
requested display's focused application window. If no window reports focus,
its sole application window may be used; another focused system window blocks
that fallback. It accepts an editable flag,
an EditText class, or an advertised set-text action. A focused editor is preferred;
without one, only a single unambiguous editable field is accepted. Tree searches
are bounded and truncated trees cannot provide an unfocused fallback. Password
fields are excluded from direct editing. A known focused editor is not rejected
solely because its accessibility `enabled` flag is false: Vivo/WeChat reports
that state after the physical display sleeps while native keys still work.
The actual set-text action remains authoritative. An unfocused fallback editor
must still be enabled.

Unlocked phones use native paste for Unicode text instead of replacing the
entire field and restoring its cursor for every letter. This keeps the editor's
native selection, formatting and text watchers in charge. Direct accessibility
editing remains the clipboard-free path while locked, or if writing the clipboard
fails while unlocked. Every app-side helper command uses one sending queue,
including calls from background threads; a key cannot overtake already queued
text. Queued commands from an old helper connection are discarded. App-window
Send no longer consults the physical display's accessibility service.

Forward Delete is suppressed when fresh accessibility data confirms a collapsed
selection at the end of the text. This includes repeated Delete presses. Other
selections, and fields whose selection cannot be read, keep native key handling.

Failures are logged as `text input on display <id>: <reason>`, at most once per
five seconds per display. Reasons distinguish missing application windows,
missing window roots, noneditable focused nodes, ambiguous editors and truncated
trees. They include window IDs, node/editor counts and phone lock state. These
diagnostics use INFO logging and the helper announces `text input diagnostics: v3`
at startup. They never contain typed characters, editor contents or clipboard data.

## Power ownership

Starting a screen mirror or app window acquires its power request before opening
the helper socket. Startup has its own owner until the app stream takes over.
Failure releases only that owner. Opening a second app does not overwrite the
PC's existing stay-awake/screen-off preference. Panel-off requests are aggregated
across screen mirroring, apps and pending starts; a wake during startup reapplies
the existing panel-off request.

On the state-based Android display-power API, restoration uses
`Display.STATE_UNKNOWN`, allowing Android to choose its normal current state.
It does not force `STATE_ON` if the user has since put the phone to sleep. The
older boolean API retains `true` for restoration.

## Verification status and device checks

Read-only diagnostics after reproducing the issue showed the WeChat window on
the Vivo's virtual display still focused and resumed, with UiAutomation connected
and the window present in accessibility. This rules out complete app-display
shutdown, but does not reveal the old helper's failed node/action condition.

The contributor built and installed the first hardening revision successfully,
but reported the issue unchanged. Its retained log reported
`text input on display 9: no unambiguous editable field`. That message confirmed
field discovery failed before insertion, but combined several lookup conditions.
The revised diagnostics separate those conditions. A later build from
`all-patches` initially omitted that hardening commit; it has now been
cherry-picked into `all-patches` as `b8a726a`.

The next contributor check confirmed the cursor jitter had stopped. After the
phone timed out, ASCII continued working but Cyrillic stopped. The detailed log
identified `focused node is not editable (enabled=false, password=false)` with
`phoneLocked=true`, while the WeChat virtual window remained focused, visible
and resumed. The helper's own `isEnabled` predicate prevented the set-text action
from being attempted. The current revision removes that rejection for focused
editors and includes editable/set-text/password flags in any subsequent refusal.
Acceptance of the action on the device still needs verification.

No new app build or test execution was performed for the current changes, at the
contributor's request. The added `StreamPowerRequestsTest` cases cover startup
failure, handoff to the active stream, and overlapping screen/app owners; they
still need to be run when compilation is authorized.

After a build is authorized, check WeChat and Telegram on both phones:

1. With the phone awake, tap the app window's compose field and type mixed ASCII,
   Cyrillic and emoji. Replace a selection, move the cursor, delete and paste
   multiline text. Check fast typing for lost characters or cursor jumps. Hold
   Forward Delete at the end: no characters should be removed; repeat in the
   middle and with a selection to check native forward deletion still works.
2. Lock the phone with its power button; repeat in the app window. Inspect the
   reason-only diagnostics if direct editing is rejected. Repeat after timeout.
   Do not use Send in a real conversation for this check.
3. Repeat in a whole-screen mirror using NectarLink's panel-off option while
   unlocked. Separately verify that locking the phone shows its normal keyguard.
4. With screen mirroring and two app windows open, toggle panel off, open another
   app, close one stream and cause a startup failure. The remaining owner's
   panel-off request should survive. Closing the last owner restores Android's
   chosen display state.
5. Verify Send in an unsent test draft/form targets the selected display; keep
   another editable field open on the phone to detect accidental cross-display
   routing. Verify fields that reject accessibility editing retain the unlocked
   clipboard fallback and leave the clipboard untouched while locked.

## Android references

- [UiAutomation](https://developer.android.com/reference/android/app/UiAutomation):
  all-display window retrieval and cache clearing (the latter is API 34+).
- [Display power reset in AOSP](https://android.googlesource.com/platform/frameworks/base/+/a8458cd441e838c29087077dd6510543f394ee63%5E!/):
  the state-based API restores the system-selected state using `STATE_UNKNOWN`.
