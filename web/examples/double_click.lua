-- Double-click the top right button for the next colour, the top left one for the previous.
-- A single press does nothing.

DOUBLE_CLICK = 0.3   -- seconds: let go and press again quicker than this for a double click
hue = 0              -- the colour, from 0 to 1 round the colour wheel

function input(button, pressed, seconds)
  -- on a press, seconds is how long the button was up since it was let go
  if pressed and seconds < DOUBLE_CLICK then
    if button == 'right' then hue = hue + 0.1 else hue = hue - 0.1 end
  end
end

function frame(t)
  led.fill(hsv(hue, 1, 0.5))
end
