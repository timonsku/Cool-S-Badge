-- Slowly breathes in one colour, which drifts around the colour wheel.

function frame(t)
  local level = 0.05 + 0.95 * wave(t / 4)   -- one breath every 4 seconds
  led.fill(hsv(t / 60, 1, level))
end
