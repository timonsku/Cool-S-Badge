-- A dot runs along the LEDs. The top right button makes it faster, the top left one slower.

speed = 10        -- LEDs per second
position = 1      -- where the dot is

-- the badge calls this when a top button is pressed or let go
function input(button, pressed)
  if pressed and button == 'right' then speed = speed + 5 end
  if pressed and button == 'left' then speed = math.max(speed - 5, 5) end
end

-- the badge calls this 50 times a second
function frame(t, dt)
  position = position + speed * dt
  led.clear()
  led.set(position, WHITE)
end
