-- Random twinkles on a dim blue background.

function frame()
  led.fade(0.9)
  for i = 1, led.count do
    -- the background is the floor the twinkles fade down to
    local r, g, b = split(led.get(i))
    led.set(i, r, g, math.max(b, 24))
  end
  if math.random() < 0.3 then
    led.set(math.random(led.count), mix(WHITE, YELLOW, math.random()))
  end
end
