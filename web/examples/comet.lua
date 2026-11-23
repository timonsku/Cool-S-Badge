-- A comet with a fading tail runs along the chain.
-- Written as a loop with sleep() instead of frame(), both work.

local pos = 1
local color = CYAN

while true do
  led.fade(0.75)          -- what was drawn before fades into a tail
  led.set(pos, WHITE)     -- the head
  led.add(pos - 1, color) -- LED numbers wrap around: 0 is the last LED
  led.show()
  pos = pos + 1
  if pos > led.count then
    pos = 1
    color = hsv(math.random())
  end
  sleep(40)
end
