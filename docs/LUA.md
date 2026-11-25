# Lua scripts for LED animations


Firmware v0.7 and newer supports LED animations written in [Lua](https://www.lua.org/manual/5.4/) (version 5.5).
A script is a plain `.lua` text file of at most 12 KB.

Write it in the web UI's Code tab and press "Upload and run" (Ctrl+S): the badge starts it immediately, the editor
shows any error together with its line. Scripts are kept in the `lua` folder on the badge. Pick them with the buttons
like a GIF or PNG, the top buttons switch to the Lua mode, the bottom buttons step through the scripts. Holding the
bottom two for 3 seconds makes the running script the one the Lua mode starts with, as does "Make default" in the
web UI's Media tab.

Ready-made scripts are in [web/examples](../web/examples), and in the editor's Open menu.

## **Two ways to write a script**

Define `frame`
---
The badge calls `frame(t, dt)` 50 times a second and shows the LEDs after each call. `t` is the
time in seconds since the script started, `dt` the seconds since the previous frame. Set the global `fps` to change
the rate (1 to 100).

```lua
fps = 30

function frame(t, dt)
  led.fill(hsv(t / 10))   -- the whole badge goes once round the colour wheel every 10 seconds
end
```

Write your own loop
---
Draw, call `led.show()`, and `sleep()` between frames. Code at the top of a script runs once,
so this is also where `frame` scripts set things up.

```lua
local i = 1
while true do
  led.clear()
  led.set(i, RED)
  led.show()
  i = i + 1
  sleep(50)
end
```

A script that returns without defining `frame` leaves its last `led.show()` on the LEDs.

## LEDs

The badge has `led.count` (30) LEDs, numbered from 1 in the order the data line runs through them. Numbers wrap
around: `0` is the last LED and `led.count + 1` the first, which makes chases simple.

| Function | What it does |
| --- | --- |
| `led.set(i, color)` or `led.set(i, r, g, b)` | Sets LED `i`. |
| `led.add(i, color)` | Adds to what LED `i` already has, clipped at full brightness. |
| `led.get(i)` | The colour of LED `i`. |
| `led.fill(color)` | Sets every LED. |
| `led.clear()` | Turns every LED off. |
| `led.fade(f)` | Multiplies every LED by `f`: `0.8` each frame leaves a short trail. |
| `led.rotate(n)` | Moves every colour `n` LEDs along the chain (default 1, negative goes back). |
| `led.show()` | Shows what was drawn. Not needed with `frame`. Drawing continues on the same picture. |
| `led.x[i]`, `led.y[i]` | Where LED `i` is, from 0 to 1: `x` from left to right, `y` from top to bottom, as the badge is worn. |

Use `led.x` and `led.y` for effects that follow the shape instead of the chain, for example a bar moving down the badge
or a rainbow scrolling across it. See `scan.lua` and `rainbow_scroll.lua`.

Brightness and gamma correction are applied by the badge, so scripts use the full 0 to 255 range. The brightness
setting from the iPixel app applies to scripts as well.

## Colours

A colour is a number `0xRRGGBB`, like `0xff8000` for orange. These are predefined: `BLACK`, `WHITE`, `RED`, `GREEN`,
`BLUE`, `YELLOW`, `CYAN`, `MAGENTA`, `ORANGE`, `PURPLE`, `PINK`.

| Function | What it returns |
| --- | --- |
| `rgb(r, g, b)` | A colour from red, green and blue, each 0 to 255. |
| `hsv(h, s, v)` | A colour from hue, saturation and brightness. `h` goes round the colour wheel once from 0 to 1 and wraps around, `s` and `v` are 0 to 1 and default to 1. |
| `mix(a, b, t)` | From colour `a` at `t = 0` to colour `b` at `t = 1`. |
| `dim(color, f)` | The colour with every channel times `f`. |
| `split(color)` | Its red, green and blue: `local r, g, b = split(led.get(1))`. |

## Time and shapes

| Function | What it does |
| --- | --- |
| `sleep(ms)` | Waits `ms` milliseconds. |
| `now()` | Seconds since the script started. |
| `millis()` | Milliseconds since the script started. |
| `wave(x)` | A smooth wave from 0 up to 1 and back to 0 as `x` goes from 0 to 1, repeating. `wave(t / 4)` breathes every 4 seconds. |
| `tri(x)` | The same in straight lines. |
| `clamp(x, lo, hi)` | `x` limited to `lo`..`hi`, which default to 0 and 1. |
| `lerp(a, b, t)` | From `a` at `t = 0` to `b` at `t = 1`. |

## Lua functions

The base library, `math` (including `math.random`) and `table` are available. Left out to fit the limited flash: `string`, `io`, `os`, `coroutine`, `utf8`, `debug`, `require`, `load`,
`loadfile`, `dofile` and `xpcall`. Numbers are 32-bit: integers up to about 2 billion and single-precision floats.
`print` writes to the USB console.

## Limits

- A script gets 40 KB of memory. The examples use about 17 KB.
- Nesting is limited to 32 levels of function calls from C and of parentheses or blocks in the source.
- Scripts run when nothing else needs the processor, so Bluetooth, the buttons and uploads keep working while a script
  loops without a break. Switching mode, uploading or live drawing stops the script.

## Debugging

Every other LED turns red when a script fails. The error, with the file name and line, is logged on the badge's USB serial console.
