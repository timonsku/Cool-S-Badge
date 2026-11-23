-- Flickering flames, hottest at the bottom of the badge.

local heat = {}
for i = 1, led.count do heat[i] = 0 end

fps = 30

function frame()
  for i = 1, led.count do
    -- the lower the LED (y towards 1), the more heat it gets
    local target = led.y[i] ^ 2 * (0.6 + 0.4 * math.random())
    heat[i] = lerp(heat[i], target, 0.3)
    local h = heat[i]
    -- black, red, orange, yellow
    led.set(i, clamp(h * 3) * 255, clamp(h * 3 - 1) * 200, clamp(h * 3 - 2) * 80)
  end
end
