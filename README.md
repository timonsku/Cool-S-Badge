# Cool-S-Badge

Targets nRF Connect 3.1.1

For Assembly check the docs folder.
TRIPPLE CHECK THE PRO MICRO ORIENTATION before soldering in place

Update the firmware with the latest version comitted to the root of the repo
Download Nordic Device Manager and select the zip file in the DFU tab.
Takes about 30sec after upload before it comes back online.
https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-Device-Manager


Use "iPixel Color" app in "DIY Drawing" mode to apply LED colors.
Animation support coming soon, bit buggy right now.
Brightness adjustment in Settings menu works but does not persists right now.
Colors persist through reboots if you pressed OK in the editor.

Click the big plus sign to find devices. They are called "LED_XXXX". With XXXX being the last two HEX bytes of the MAC address.

The iOS app seems to have trouble finding devices. Might be due to too many BLE devices, try a less chatty place if thats the case.

Android: https://play.google.com/store/apps/details?id=com.wifiled.ipixels&hl=en

iOS: https://apps.apple.com/us/app/ipixel-color/id1562961996
