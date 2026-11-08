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
requested display's focused application window. It accepts an editable flag,
an EditText class, or an advertised set-text action. A focused editor is preferred;
without one, only a single unambiguous editable field is accepted. Tree searches
are bounded and truncated trees cannot provide an unfocused fallback. Password
fields are excluded from direct editing.

Direct insertion and cursor restoration run in the helper's input queue. ASCII
text, editing keys, shortcuts and Send are ordered with them. App-window Send no
longer consults the physical display's accessibility service. Clipboard fallback
is attempted only while the keyguard is unlocked, so unsuccessful locked-phone
input does not replace the user's clipboard.

Failures are logged as `text input on display <id>: <reason>`, at most once per
five seconds per display. Reasons distinguish missing/unambiguous editors,
rejected actions, unavailable accessibility and the locked clipboard. They never
contain typed characters, editor contents or clipboard data.

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

No app build or test execution was performed for these changes, at the
contributor's request. The added `StreamPowerRequestsTest` cases cover startup
failure, handoff to the active stream, and overlapping screen/app owners; they
still need to be run when compilation is authorized.

After a build is authorized, check WeChat and Telegram on both phones:

1. With the phone awake, tap the app window's compose field and type mixed ASCII,
   Cyrillic and emoji. Replace a selection, move the cursor, delete and paste
   multiline text. Check fast typing for lost characters or cursor jumps.
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
