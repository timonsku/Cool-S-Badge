-- A bar of light sweeps from the top of the S to the bottom and back,
-- using each LED's position (led.y) instead of its place in the chain.

local width = 0.15

function frame(t)
  local y = tri(t / 3)   -- 0 at the top, 1 at the bottom, 3 seconds per sweep
  for i = 1, led.count do
    local d = math.abs(led.y[i] - y)
    local level = clamp(1 - d / width)
    led.set(i, dim(ORANGE, level))
  end
end
