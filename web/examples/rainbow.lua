-- A rainbow that flows along the LED chain.

fps = 50

function frame(t)
  for i = 1, led.count do
    led.set(i, hsv(i / led.count - t * 0.2))
  end
end
