# Mirroring with the physical screen off

## Two different states

The mirror's screen-off option blanks the physical panel while a wake lock keeps
the screen pipeline active. Pressing the phone's power button or letting it time
out can instead lock the default display. A whole-screen mirror must continue to
respect that lock screen. Elevated app windows use their own trusted, unlocked
virtual displays and may stay active while the default display is locked.

## Unicode input rollback

The experimental shell-side accessibility text insertion and its follow-up editor
searches were reverted after device checks still failed to enter Cyrillic while
the phone was locked. Input now follows the upstream implementation: the normal
accessibility service handles Unicode on the physical screen when available;
Elevated app windows use key events for ASCII and clipboard paste for other text.
Android may deny clipboard access while locked, so Unicode input in those windows
remains a known limitation. This rollback does not claim to fix it.

The screen-off lifecycle changes below remain independent of that experiment.

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
