# ADR-0059: The drive view: the phone's Dash with every bar hidden, the screen kept awake, and one route for Android's Back

- **Status:** Accepted; built in v0.7.1.
- **Date:** 2026-11-08.
- **Builds on:** [ADR-0057](0057-dashboards.md) (the dashboards, and its
  appendix "six columns, the panel, one gesture, copy and paste"),
  [ADR-0058](0058-night-mode.md) (the Night switch),
  [ADR-0056](0056-analysis-log.md) (Start log and Stop log) and
  [ADR-0047](0047-adapter-relay.md) (the relay holding the adapter).

## Context

On a phone the Dash page shares the screen with the top bar, the page's
tool row, the tab bar and the system's status and navigation bars. In a
car the gauges are the point and the chrome is read once. This record
gives the chrome back on request: a view with no bars at all, which a
driver enters before setting off and leaves at the end.

Forces: a driver's taps are coarse and sometimes accidental, so an exit
must not be a single stray touch; iOS has no back button and Android's
back is a key or an edge swipe depending on the phone's navigation
mode; the app draws under the system's bars itself, so hiding them is a
window state, not a layout; a screen that sleeps mid-drive is useless;
the dashboards are the user's pages (ADR-0057), so the view shows
whatever they built, not a layout of its own.

## Decision

### 1. A state of the Dash page

The drive view is a state of the Dash page (`DashboardPage.qml`'s
`driving`), on the phone form only, not a page. It is entered from the
tool row's **Drive** button, at the row's start, enabled only while a
session is connected and edit mode is off; with no session it reads
"Connect first". The pages, their cells and the swipe between them are
those of the normal view; edit mode is not reachable in it, and a long
press on a gauge does nothing.

### 2. What hides and what stays

Hidden: the top bar, the tool row, the tab bar, the page indicator, and
the system's status and navigation bars (section 5).

Kept: a **status line**, 28 dp tall, under the top inset, in caption
type: a dot in the link state's color and `Theme.linkShortLabel` of the
state, then a separator dot and `Bridge.uriLabel` in `textDim`, eliding from its end
while the state word never does. At its right, while a log runs, "Log",
its elapsed time in the value face with tabular figures and its size in
MB; while a recording runs and no log, "Recording" and its marks when
there are any. With nothing running the line is the link state and the
address alone; it never says "No log". The line is information, not a
control: it takes no taps.

**Stop log stays in the view.** Stop log normally opens the log in the
viewer (ADR-0056 section 1). Pressed in the drive view's revealed bars,
it doesn't: a driver isn't thrown out of the view by a button press,
and the log is under Recs afterward. The status line says "Log ended",
naming neither folder nor time, for the hide delay (4 s when the delay
is Never), counted while the bars are down, since the header covers the
line while they're up; the bars stay as they are. It sets no log
notice: a notice lasts until Dismiss or the next log, and a notice
reveals the bars (section 3). A log that ends without being asked to
still goes through the notice and reveals.

### 3. Leaving: the reveal

A tap anywhere on the gauges slides the top bar, the tool row and the
tab bar in over the page; the gauges do not reflow, the bars cover
them. They slide out again after the **hide delay** (section 9). A
press on a bar starts the delay again and goes on to the control under
it; the delay isn't held while a finger stays down. While revealed, the
tool row's Drive button reads **Exit drive** and leaves the view for
good. So an accidental tap costs the driver a glance, never the view.

Leaving the page by any route (a tab in the revealed bar, the status's
tap to the Log, anything else that takes the app off Dash) counts as
Exit drive: it clears `dashboards/phoneDrive` and restores the system
bars. Only a session's end leaves the setting on (section 7).

On the first three entries a hint, "Tap for the controls", shows over
the gauges for the hide delay's length (4 s when the delay is Never),
counted in `dashboards/driveHints`, and goes at a tap. After that it
never shows.

The bars also reveal by themselves and stay up, the delay held:

- when the link state becomes `fault` (the driver may need Retry or
  Disconnect);
- when `Bridge.logNotice` has words (the notice row and its Dismiss show
  as in the normal view).

A fault or a notice already up on entering reveals the bars at once, so
a notice can't sit hidden behind the view. The hold ends when the
condition clears or the bars are tapped away, and a later tap's reveal
has the ordinary delay, even with the fault still up. The bars do not
reveal for `quiet`, `requests`, `stopped` or `degraded`: the status
line's color says it, and a dropped request while driving is not an
action for the driver.

A session that ends (`disconnected`) ends the view and leaves the
setting on: the page returns to the normal Dash with its connect list.
`stopped` keeps the view, since a replay's user may rewind from the
reveal, and so does a recording that ends.

### 4. Android: Back across the app

`Main.qml`'s `goBack()` is the one route for Android's Back, from
`zoomRoot`'s key handler. On the Dash page with the view on, hidden
bars: Back reveals them; revealed bars: Back leaves the view for good.
On a Settings subpage it closes the subpage; from the Log it returns to
the page the Log was reached from; from Connection reached through Dash's
"Other address..." it returns to Dash; from an open recording it returns
to the Recs list. Anywhere else it answers false and the system takes
it: the app goes to the launcher with the session still running. A held
key's repeats are taken and do nothing, so one press is one Back.

The key arrives at `zoomRoot`, which takes the key focus at every page
change and when the focused item goes away or hides: a page left while
it held the focus would keep it though hidden, and Back would then reach
nothing. A window that has lost the front doesn't take the focus back,
so a field that had it keeps it on return.

`AndroidManifest.xml` sets `enableOnBackInvokedCallback="false"`. With
it true, a back swipe under gesture navigation never reaches the app;
with it false, Back arrives once per press of the three-button key, and
a back swipe arrives outside the view. In the view the system's bars are
hidden, and on the test phone under gesture navigation an edge swipe
first brings them back for about 3 s; how many swipes a thumb needs to
leave the view is not settled. So the tap and Exit drive are the
dependable exits, and Back is the extra one.

iOS has no back; the tap and Exit drive are the exits.

### 5. The system bars

The window goes full screen from QML (`Immersive.enter`, phone form
only): `Window.FullScreen`, which on Android hides the status and
navigation bars until a swipe brings them back for a few seconds. The
window's visibility is remembered on entering and put back on leaving
(`Immersive.leave`), since a phone's window is `Window.Maximized`, not
`Window.Windowed`. On iOS the same window state is all there is; it has
not run on a device. The status line sits under the top inset and the
panel ends above the bottom inset, as `Main.qml` reads them. A system
bar that peeks back draws over the page and the insets don't change, so
the line stays where it is, in the page's ground color rather than a
painted band.

### 6. The screen stays awake

This rule covers the phone form and the drive view (which a tablet has
without the phone form), not only the view. While a session is
connected and the app is in front, the screen does not sleep if the
phone has external power or the Dash page is showing:

| Session | Power | Page | Screen |
|---|---|---|---|
| Connected | External | Any | Stays on |
| Connected | Battery | Dash, or the drive view | Stays on |
| Connected | Battery | Any other | Sleeps as usual |
| None | Either | Any | Sleeps as usual |

The rule is one pure function in `KeepAwake.qml`,
`keepAwake(connected, powered, onDash)`; its properties are `hasPower`
and `dashShown`, since QML reads a property named `on` and a capital as
a signal handler. The desktop form never asks.

**External power is "plugged in", not "charging".** Android reads the
sticky `ACTION_BATTERY_CHANGED` intent's `plugged` extra, non-zero for
AC, USB, wireless or dock; its charging status would be wrong, since a
phone on USB at a battery limit, or on a weak car port, reports "not
charging". iOS reads `UIDevice.batteryState`, charging or full, with
battery monitoring on. Both are read every 5 s while a session is
connected and the app is in front, so no receiver or native callback is
needed, at the cost of up to 5 s of lag.

The flag (`PhoneWindow.setKeepScreenOn`): Android's
`FLAG_KEEP_SCREEN_ON` on the window, iOS's `idleTimerDisabled`. It
clears as soon as the rule turns false, and when the app leaves the
front, since the rule asks for the app in front; it is asked again on
return.

### 7. Remembered once connected

`dashboards/phoneDrive` is set on entering the view and cleared by Exit
drive, by Back from revealed bars and by leaving Dash (section 3). The
session's end does not clear it. The app never opens in the view: it
opens on the normal Dash, with the connect list when there is no
session, and enters the view by itself when a session connects if the
setting is on, the Dash page is showing and the URI is not `replay://`
(`Bridge.currentUri`). A session connected from another page stays on
that page. So a driver who left the view on finds the gauges full screen
the moment the adapter answers, and a driver who opens the app in the
garage sees the page.

**The connect list.** With no session on the phone form and out of edit
mode, Dash shows a list above the gauges on every page, in the edit
panel's slot, and the page scrolls past it, so a phone on its side
reaches the gauges. Its rows are the recent addresses,
`session/recentUris`: up to 5, newest first, written on a connect that
succeeded, a repeat moved to the top, seeded from `session/uri` when
empty or damaged. A row shows `Bridge.uriLabelFor`'s short form and
names its kind by scheme (Demo data, Recording, J2534 adapter, ELM327
adapter, CAN interface). The chosen row is a tapped row until the list
or the vehicle's address changes, else the picked vehicle's saved
address (listed first when it isn't recent, and not stored until it
connects), else the newest. One primary Connect connects the chosen row
through Settings, the only page that connects; it is off while a relay
holds the adapter (ADR-0047). A failed connect's words show on the
list. "Other address..." opens Settings' connect field. With no recent
address there are no rows, and the one Connect reads "Connect..." and
opens Settings. A `replay://` address is listed like any other; it just
never enters the view by itself. The list is the phone form's alone: the
desktop keeps Home.

### 8. Both orientations, the same pages

The view works upright (6 columns, ADR-0057's appendix) and on its
side (12). The status line is at the top in both. A rotation keeps the
view and the page shown.

A lock (section 9) holds the orientation while the view is on, so a
phone in a holder doesn't flip when the car turns. It applies only in
the view and is released on leaving it
(`PhoneWindow.lockOrientation`: `follow`, `upright` or `side`, anything
else `follow`). Android: `setRequestedOrientation` with
`SCREEN_ORIENTATION_SENSOR_PORTRAIT` or `SCREEN_ORIENTATION_SENSOR_LANDSCAPE`
(either way up), unspecified to release. iOS offers no lock
(`PhoneWindow.orientationLockAvailable` is false), so the setting is
hidden there.

### 9. Settings

Under Display, on a phone: **Hide the drive view's bars after** with
2 s, 4 s, 8 s and Never (the bars stay until tapped away), default 4 s,
stored as `ui/driveHideMs` (0 for Never). Never still obeys section 3's
own reveals. **Drive view orientation** with Follow the phone, Upright
and On the side, default Follow the phone, stored as
`ui/driveOrientation`, shown only where the lock is offered.

### 10. Tests

`QmlTest.driveview` drives the view in `Main` at a phone's size: Drive
off without a session and absent on the desktop form; entering hides
the bars and keeps the status line, the panel to the screen's edges with
its seams; the status line's words for each state, a log, a recording
and its marks, a long address; Stop log in the view and a log that ends
by itself; the tap, the delay, a touch restarting it, Never; a fault and
a log notice holding the bars, already up on entering or tapped away;
only `fault` raising them; `disconnected` ending the view and keeping
the setting; Exit drive, a tab and the status's tap leaving for good;
the auto-entry on Dash, never from another page or for `replay://`; the
hint on the first three entries and its length; the orientation lock in
the view alone; the screen kept on; the insets; a rotation; and Back on
every page, a held key, and the key focus after a page change, a hidden
control and a destroyed one. A fault and a quiet bus come from the fake
J2534 DLL (Windows). `QmlTest.driveplatform` runs section 6's table,
the flag, the power timer, the full-screen window's visibility kept and
put back, and the orientation's words. `QmlTest.phone`
(`test_theDriveSettingsFitAPhone`) and `QmlTest.settings` (the recent
addresses) cover sections 7 and 9. The platform calls (the full-screen
window, the power reading, the flag, the lock) are checked on the test
phone, not in CI; iOS's have no device to run on.

## Alternatives considered

- **Tap to exit.** Cheapest, and one stray touch drops the driver out
  of the view they chose. Rejected for the reveal.
- **A swipe to exit or reveal.** A vertical swipe fights the page's
  scroll, a horizontal one is the swipe between pages, and a swipe from
  the top edge is Android's shade. Rejected.
- **Back alone.** Android only. Rejected as the sole exit, kept as an
  extra one.
- **Reopen in the view at launch.** A dark full-screen page with no
  session and no controls is the wrong first screen in a garage.
  Rejected for "remembered once connected".
- **A drive page of its own** (a layout only the view shows). It would
  double the thing a user edits. Rejected: the view shows the pages the
  user built.

## Consequences

- The chrome in the view is one 28 dp line under the top inset; the
  system's bars are gone with the app's. A page whose rows end above the
  screen's bottom leaves the rest empty: the view shows the panel's rows
  as the user built them.
- Three platform calls per platform (the full-screen window, the screen
  flag and the power reading) and a back-key route enter the app, each
  small. Android's are checked on hardware, not in CI; iOS's have no
  device to run on.
- A connected phone on battery with Dash showing keeps its screen lit
  for as long as the page stays up. That is the chosen trade: seeing the
  gauges beats the battery and the heat.
- The log notice, Stop log and Disconnect keep their places in the bars,
  so the normal view's tests cover them; the drive view adds the
  reveal's tests, not new controls.
- A user who sets the delay to Never gets a view that behaves like the
  normal Dash with the bars overlaid until tapped: no harm, and the one
  setting answers "the bars vanished and I didn't ask".

## Decided 2026-11-08

1. Keep awake: section 6's rule (external power on any page, Dash on
   battery, only while connected).
2. The bars reveal by themselves on `fault`.
3. Only `disconnected` ends the view; `stopped` keeps it, since a replay
   user may rewind from the reveal.
4. The delay: 2, 4, 8 s and Never, default 4 s.
5. The status line is at the top in both orientations.
6. A tab pressed in the revealed bar is Exit drive.
7. Auto-entry happens only with Dash showing, never for `replay://`.
8. An orientation lock in Settings, default Follow the phone.
9. The Dash's connect list holds the recent addresses with one chosen;
   "Other address..." opens Settings' connect field.
10. The "Tap for the controls" hint shows on the first three entries.
