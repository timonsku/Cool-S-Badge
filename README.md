# Cool-S-Badge

Targets nRF Connect 3.1.1

For Assembly check the docs folder.
TRIPPLE CHECK THE PRO MICRO ORIENTATION before soldering in place

Always update the firmware with the latest version committed to the root of the repo.

To build the firmware, fetch the Lua module first: `git submodule update --init --recursive`

Web UI
----------------
https://timonsku.github.io/Cool-S-Badge/ manages GIF, PNG and Lua files on the badge and updates its firmware over Bluetooth.
Needs Chrome or Edge (Windows, macOS, Android, ChromeOS) or Bluefy on iOS (https://apps.apple.com/us/app/bluefy-web-ble-browser/id1492822055). Close the iPixel app first, the badge takes one connection at a time.
The Media tab needs firmware v0.7+.

The badge shows uploaded files with its buttons, see "Buttons" below. It needs firmware newer than v0.6 for that.
The LEDs show the first 30 pixels of an image, row by row. PNGs over 1024 pixels or 16 KB are not shown.

"Download backup" saves every file on the badge as a .zip laid out as on its storage: `settings.json`, `default.gif`, `default.png`, `default.lua`, `gif/`, `png/`, `lua/` and anything else there is (hidden temporary files are left out). "Restore backup" writes such a .zip back, for example after the storage was erased. Files on the badge with other names are kept.

A zip of the unpacked backup folder, made with the operating system's own tools, also restores. Restart the badge afterwards to load restored settings. Firmware older than v0.7 can only back up and restore the files named above in the root and in the `gif` and `png` folders.

"Erase storage" formats the whole storage, settings included, as on a new badge. It offers to save a backup first. The badge restarts, erases the storage while starting and comes back with default settings. Needs firmware v0.7+, and is refused while a firmware update is still on trial.

The Draw tab paints pictures and animations on a model of the badge. One frame uploads as a 30×1 PNG, more frames as a 30×1 GIF. The drawing is kept in the browser between visits.

With "Live preview on badge", the badge shows the frame being drawn, and Play sends the animation to the badge, which plays it from RAM (up to 16 KB, about 300 frames) without saving anything. Only "Upload" writes to flash. Playing animations on the badge needs firmware v0.7+.

The Code tab is an editor for LED animation scripts in Lua, see "Lua scripts" below: open an example, a script from the badge or a file, then "Upload and run" (Ctrl+S) shows it on the badge and points at the line of any error. The Media tab's Lua folder lists the scripts on the badge, with Edit to open one in the editor. Needs firmware v0.7+.

To run it locally: `python -m http.server -d web 8000` and open http://localhost:8000

Lua scripts
----------------
Firmware v0.7 and newer runs LED animations written in Lua. Scripts are plain `.lua` text files of up to 12 KB, kept in the `lua` folder (write and upload them in the web UI's Code tab) or as `default.lua`. A minimal script:

```lua
function frame(t)              -- called 50 times a second, t is in seconds
  for i = 1, led.count do
    led.set(i, hsv(i / led.count - t * 0.2))
  end
end
```

The script reference is in [docs/LUA.md](docs/LUA.md), example scripts are in [web/examples](web/examples).
A script that fails shows every other LED red; the reason is logged on the USB console.

Firmware update approval
----------------
Applies to firmware newer than v0.6.
- An update has to be allowed on the badge. When it breathes yellow, press each of the four buttons twice within 30 seconds.
  If it breathes yellow and you did not start an update, don't press anything, it will time out by itself.
- New firmware confirms itself after running properly for 30 seconds. If it crashes or hangs before that, the previous firmware comes back by itself. Switching the badge off during those 30 seconds also brings the previous one back.
- In Device Manager use "Test and Confirm". "Confirm only" makes an update permanent before it has ever run.
- USB recovery stays available: hold the top right button while the badge resets.

OTA Firmware Update
----------------
Download Nordic Device Manager and select the zip file in the DFU/Image tab.
Takes about 30sec after upload before it comes back online.
https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-Device-Manager

Step by step instructions can be found in [OTA.md](docs/OTA/OTA.md)

How to set colors
----------------
Use the new WebUI as described above, its by far the easiest way. Alternatively use the "iPixel Color" app in "DIY Drawing" mode to apply LED colors.
Brightness adjustment in Settings menu works.
Colors persist through reboots if you pressed OK in the editor.
Animations are also supported. Both animation and static images can be loaded from the gallery view after creating them.

Click the big plus sign to find devices. They are called "LED_XXXXXXXX". With XXXXXXXX being the last four bytes of the MAC address.
If nothing changes when you draw you are probably disconnected, the app does not always automatically reconnect.

Android: https://play.google.com/store/apps/details?id=com.wifiled.ipixels&hl=en

iOS: https://apps.apple.com/us/app/ipixel-color/id1562961996


Turn On/Off
----------------
Hold the top two buttons for a second, until the LEDs go dark, then let go to turn off.
Press any button except the top right one to turn back on.
Firmware up to v0.6 used the bottom two buttons for this.

Buttons
----------------
Applies to firmware newer than v0.6.
- Top right / top left: next / previous mode: static picture, GIF, Lua script. Modes without any files are skipped.
- Bottom right / bottom left: next / previous file of that kind. The badge goes through its default (default.png, default.gif or default.lua) and the files in the png, gif or lua folder the web UI manages.
- Hold the bottom two for 3 seconds: the file that is showing becomes the new default. The LEDs flash white to confirm.
- Hold all four for 10 seconds: restart into USB recovery.
- Live drawings from the iPixel app are saved as default.png. Older firmware kept them in default.dat; the first start on the new firmware converts that file.