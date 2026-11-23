-- A rainbow scrolls down the badge, from the top of the S to the bottom.
-- Colour follows each LED's height (led.y), not its place in the chain.

local speed = 0.3    -- rainbows per second passing an LED
local stripes = 1.5  -- how many rainbows fit from top to bottom

function frame(t)
  for i = 1, led.count do
    led.set(i, hsv(led.y[i] * stripes - t * speed))
  end
end
