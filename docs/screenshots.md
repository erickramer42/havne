# Screenshots

Every page of Havne on the desktop and the phone, all on the simulated
vehicle (`mock://demo`) in US units. Dark is the default look; Night
dims it for a dark cabin in amber or red (Settings, Display). The
gallery shows those three: Dark, and Night in amber and red. Display's
Look row also offers Light, dark text on white, and the desktop's
Dashboard is one panel with its pages as tabs, neither of which these
shots show. The README shows a few of these.

- **Desktop:** the Windows app at 1280x800.
- **Phone:** Live, Codes, the Log and Settings are from the test phone
  (Android 16). The Dashboard is the phone layout with its built-in
  default page, the same page drawn by the desktop build in the phone's
  form at the phone's width.

## Desktop

### Dashboard

![The Dashboard through a quarter mile: a launch from the two-step near 3,900 rpm, upshifts that refill the shift lights, and past 100 mph at the line](images/dashboard.gif)

*A quarter mile on the warmed-up vehicle: the launch, each upshift, and
coolant and oil climbing toward the fan's cut-in.*

| Night, amber | Night, red |
|---|---|
| [![The Dashboard in Night's amber style in a 2nd-gear pull at 5,925 rpm](images/desktop-dashboard-amber.png)](images/desktop-dashboard-amber.png) | [![The Dashboard in Night's red style in a 2nd-gear pull at 6,642 rpm](images/desktop-dashboard-red.png)](images/desktop-dashboard-red.png) |

### Live

![Live: rpm, speed, MAF and lambda through a 3rd-gear pull onto the limiter](images/live.png)

*Four signals plotted through a 3rd-gear pull onto the limiter, with the
2nd-gear pull still on the chart.*

| Night, amber | Night, red |
|---|---|
| [![Live in Night's amber style, rpm, speed, MAF and lambda on the limiter](images/desktop-live-amber.png)](images/desktop-live-amber.png) | [![Live in Night's red style, rpm, speed, MAF and lambda on the limiter](images/desktop-live-red.png)](images/desktop-live-red.png) |

### Diagnostics

![Diagnostics: the inspection verdict, ECU 0x7E8's stored, pending and permanent codes with their titles, and the readiness monitors](images/desktop-diagnostics.png)

*The inspection verdict (it would fail: the ECU commands the
check-engine light on), each code with its SAE J2012 title, and the
readiness monitors, each done this drive and complete since the clear.*

### Log

| Activity | Frames |
|---|---|
| [![The Log's activity view: two connects and, between them, the disconnect's Delivery line](images/desktop-log-activity.png)](images/desktop-log-activity.png) | [![The Log's frame table in developer mode, newest first](images/desktop-log-frames.png)](images/desktop-log-frames.png) |
| Each session's lines; a disconnect adds its Delivery line (none dropped, and the GUI thread's wait) | In developer mode, the raw frames: time, length, standard or extended ID, payload |

### Recordings

![The Recordings page: a recording of just over 3 minutes open in the viewer, four signals plotted from the cold start](images/desktop-recordings.png)

*A recording in the viewer: rpm and speed through the first loop's
pulls, coolant and oil warming from the cold start.*

### Link health

![Live over the fake J2534 driver with every request refused: the "Requests failing" status and banner, and the chart waiting for data](images/desktop-link-health.png)

*Every request refused by the fake J2534 driver: the link reads
"Requests failing", not idle, and the banner says what to check.*

### Home and Settings

| Home | Settings |
|---|---|
| [![Home with a session running](images/desktop-home.png)](images/desktop-home.png) | [![Settings with Display open beside the list: display size, Night and its style, units](images/desktop-settings-display.png)](images/desktop-settings-display.png) |
| The session's state and the core's version | Display beside the list: size, Night off, on or scheduled, its style, units |

## Phone

### Dashboard

| Dark | Night, amber | Night, red |
|---|---|---|
| [![The phone layout's Dashboard in a 2nd-gear pull at 6,252 rpm](images/phone-dashboard-dark.png)](images/phone-dashboard-dark.png) | [![The phone layout's Dashboard in Night's amber style at 5,359 rpm](images/phone-dashboard-amber.png)](images/phone-dashboard-amber.png) | [![The phone layout's Dashboard in Night's red style at 6,475 rpm](images/phone-dashboard-night.png)](images/phone-dashboard-night.png) |

*The phone's joined panel: the tach beside speed, coolant and oil, MAF
with its trace, then intake air, load and throttle.*

### Live

| Dark | Night, amber | Night, red |
|---|---|---|
| [![Live on the test phone on the limiter, rpm, speed, MAF and lambda over a minute of the chart](images/phone-live.png)](images/phone-live.png) | [![Live on the test phone in Night's amber style in a pull at 6,558 rpm](images/phone-live-amber.png)](images/phone-live-amber.png) | [![Live on the test phone in Night's red style on the limiter](images/phone-live-red.png)](images/phone-live-red.png) |

![Live on the test phone on its side: the chart takes the page through a 2nd-gear and a 3rd-gear pull](images/phone-live-landscape.png)

*On its side, the chart takes the page: a 2nd-gear pull, then the
3rd-gear pull onto the limiter.*

### Codes, Log and Settings

| Codes | Log | Settings |
|---|---|---|
| [![Codes on the test phone: the inspection verdict and ECU 0x7E8's codes with their titles](images/phone-codes.png)](images/phone-codes.png) | [![The Log's frame table on the test phone](images/phone-log.png)](images/phone-log.png) | [![Settings, Display on the test phone: size, Night, units, and the drive view's options](images/phone-settings-display.png)](images/phone-settings-display.png) |
| The verdict, then each ECU's codes | The status opens the Log; here its frames | Display, with the drive view's bars and orientation |
