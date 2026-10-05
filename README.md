<h1 align="center">SeekDial</h1>

<p align="center"><b>Desktop control for the CMF Buds Pro 2 smart dial &mdash; seek, scroll and skip on Windows.</b></p>

A small, native Windows app that gives the dial on your earbuds a second job. Instead of only changing the volume, it can **seek videos back and forward**, **scroll feeds and pages**, or **skip Reels / Shorts** &mdash; switchable from a window, the tray, or global hotkeys. One standalone `.exe`: no runtime, no installer, no network access.

<!-- SCREENSHOT: on GitHub click the pencil (edit) on this file, delete this line, then drag your screenshot into the editor at this spot. GitHub inserts the image link for you. -->

> ⚠️ **Unofficial** &mdash; not affiliated with, sponsored by, or endorsed by Nothing Technology. CMF and Nothing are trademarks of their respective owners. Releases are unsigned: on first launch Windows SmartScreen may say "Windows protected your PC" &rarr; **More info** &rarr; **Run anyway** (or build it yourself, see below).

## Features

- **Four dial modes** &mdash; *Volume* (normal), *Seek* (&larr; / &rarr; back and forward in videos), *Scroll* (mouse wheel, for feeds and pages), *Reels* (&uarr; / &darr; previous and next short video). Scroll direction and distance are adjustable.
- **Global hotkeys** &mdash; switch modes from anywhere (default **Ctrl+Alt+S / D / R**); press the active mode's hotkey again to return to volume. Fully configurable; a modifier is required so they can't interfere with typing.
- **Auto-detects your earbuds** &mdash; matches the default output by name (default `CMF, Buds`). If the buds aren't the default output yet, the mode waits and activates the moment they connect, and deactivates cleanly when they disconnect or the default device changes.
- **Safe volume handling** &mdash; your real volume is saved and restored when you leave a mode, exit, log off, or unplug the earbuds. Keyboard volume keys and other apps' volume changes are never mistaken for the dial.
- **Tray app** &mdash; closing the window keeps it running in the tray (optional). Left-click opens it, right-click switches modes or exits.
- **Silent notifications** &mdash; mode-change balloons are sent with no sound.
- **Start with Windows** &mdash; one checkbox; launches minimized to the tray. Uses the per-user `Run` key, no admin rights.
- **Live status** &mdash; the window shows the current default output, whether a mode is active, and a dial-click counter, so you can see exactly what is happening.
- **Lightweight and private** &mdash; event-driven (no polling loop), near-zero idle CPU, no network access, no logging, settings stored locally in `%APPDATA%\SeekDial\settings.ini`.
- **Single instance, DPI-aware, native** &mdash; plain Win32 and Common Controls v6; launching it twice just brings the window forward.

## Modes

| Mode | Dial up | Dial down | Delivered to | Good for |
|---|---|---|---|---|
| **Volume** (default) | Volume up | Volume down | System | Normal use |
| **Seek** | Right Arrow | Left Arrow | Focused window | Video players, YouTube, Netflix |
| **Scroll** | Wheel down | Wheel up | Window under the mouse | Feeds, long pages |
| **Reels** | Down Arrow | Up Arrow | Focused window | Reels, Shorts, TikTok (web) |

## Install / run

Grab `SeekDial.exe` from the [Releases](../../releases) page. There is no installer: put it somewhere permanent (for example `C:\Tools\SeekDial`) and double-click it. If you tick **Start SeekDial with Windows**, don't move the exe afterwards or the startup entry will point to the old location.

## Before using

1. **Pair the earbuds** in Windows &rarr; Settings &rarr; Bluetooth & devices &rarr; Add device.
2. **Make them the default output:** Settings &rarr; System &rarr; Sound &rarr; Output. Prefer the stereo *Headphones* entry over a *Hands-Free* / *Headset* one.
3. **Launch SeekDial.** If the status says *Waiting*, click **Use current** so the app learns your earbuds' name.
4. **Pick a mode** (window, tray menu or hotkey). The status line should read *Active*, and the dial-click counter goes up when you turn the dial.

## Settings

| Setting | Meaning |
|---|---|
| Hotkeys | One per mode; click the box and press the combination. A cleared box resets to its default |
| Earbuds name contains | Comma-separated name fragments that identify your earbuds. **Use current** fills in the current default output's name |
| Scroll distance | Wheel notches per dial click in Scroll mode (1&ndash;20) |
| Reverse scroll direction | Dial up scrolls up instead of down |
| Keep volume steady | Re-center the volume after every click (default on). Turn off only if your earbuds make a sound each time the volume is reset; the volume then drifts and is re-centered near the ends |
| Show mode notifications | Silent tray balloons when the mode changes |
| Start with Windows | Launches minimized to the tray at login |
| Closing the window keeps it in the tray | Otherwise the close button exits the app |

## Build from source

Requires MinGW-w64 (`g++` and `windres`) on Windows.

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT   # once, then open a new terminal
build.bat                                          # produces SeekDial.exe
```

`build.bat` compiles the resources (icon, manifest, version info) and `seekdial.cpp` into a statically linked executable. MSVC users can compile `seekdial.cpp` with `/DUNICODE /D_UNICODE` and link `ole32 user32 shell32 comctl32 gdi32 advapi32`, adding the resource file.

## How it works

The dial doesn't send a key or a custom event. It sends a Bluetooth AVRCP *absolute volume* command, which Windows applies as an ordinary system volume change. SeekDial listens for those changes through the Core Audio API and, while a dial mode is active:

1. saves your real volume and parks the system volume at 50%, so the dial always has room to move both ways;
2. compares each new volume to the parked one to get the direction;
3. re-centers the volume and sends the matching key press or wheel tick;
4. restores your real volume when you leave the mode.

Because the dial *is* the system volume, normal volume and dial actions can't be active at the same time.

| File | Role |
|---|---|
| `seekdial.cpp` | The whole app: audio notifications, device tracking, hotkeys, input injection, window, tray, settings |
| `seekdial.rc` | Resource script: icon, manifest and version info |
| `seekdial.manifest` | Visual styles (Common Controls v6) and DPI awareness |
| `seekdial.ico` | App icon |
| `build.bat` | One-step build with MinGW-w64 |
| `LICENSE` | MIT license |

| Purpose | Windows API |
|---|---|
| Volume read/write and change events | Core Audio: `IAudioEndpointVolume`, `IAudioEndpointVolumeCallback` |
| Default-device tracking and name | `IMMDeviceEnumerator`, `IMMNotificationClient`, `IPropertyStore` |
| Global hotkeys | `RegisterHotKey` |
| Volume-key detection | `SetWindowsHookEx(WH_KEYBOARD_LL)` |
| Arrow keys and wheel | `SendInput` |
| Tray icon and silent balloons | `Shell_NotifyIcon` (`NIIF_NOSOUND`) |
| Window and controls | Win32 + Common Controls v6 (hotkey, up-down), system-DPI aware |

## Notes

- **Arrow keys go to the focused window**, so typing while in Seek or Reels mode moves the text cursor. Switch back to Volume when you're not using the dial.
- **Seek length depends on the site.** Most players skip 10 seconds per arrow press; YouTube skips 5.
- **Reels / Shorts behavior varies by site.** If Scroll mode skips several videos at once, use Reels mode or lower the scroll distance to 1.
- Works for browsers and desktop apps on the PC the earbuds are connected to; it cannot control a phone.
- If the process is killed from Task Manager while a dial mode is active, the volume stays at 50%.
- Developed for the CMF Buds Pro 2; other earbuds that send Bluetooth absolute volume should work if their name matches. Reports are welcome.
- The keyboard hook only checks whether a key is one of the three volume keys; it never records keystrokes. Run SeekDial as a normal user, not as administrator.

### Troubleshooting

| Problem | Try |
|---|---|
| Status says *Waiting* | The status line shows the current default output. Set your earbuds as the default output, then click **Use current** |
| A hotkey warning appears | Another app owns that combination; pick a different one |
| Dial does nothing | Check that the Windows volume slider moves when you turn the dial. If it doesn't, the earbuds aren't sending absolute volume to Windows |
| Sounds or beeps | Notifications are silent by default. If the earbuds beep on each click, untick **Keep volume steady**. An arrow key sent to a window that doesn't accept it may trigger Windows' default error sound |
| Nothing scrolls in Scroll mode | Scroll goes to the window under the mouse cursor; hover over the page. For Seek and Reels, click the page once so it has focus |
| Antivirus flags the exe | Unsigned executables that install a keyboard hook can trigger heuristics. Build from source yourself |

## Contributing

Issues and pull requests are welcome, especially device names for other earbuds and sites with unusual seek or scroll behavior.

## Credits

Built with the Windows Core Audio and Win32 APIs, compiled with [MinGW-w64](https://www.mingw-w64.org/) (WinLibs build). App icon created for this project. Earbud and product names belong to Nothing Technology Limited.

## License

[MIT](LICENSE)
