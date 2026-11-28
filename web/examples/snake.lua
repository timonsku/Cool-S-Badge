-- A snake winds along the S and eats the apples in its way, growing with each one.
-- The top right button makes it faster, the top left one slower. Needs firmware v0.10+.

-- Shared between input() and frame(): input() changes it, frame() moves the snake by it.
speed = 6            -- LEDs per second
local SLOWEST, FASTEST = 1, 40
local START_LENGTH, LONGEST = 3, 20

local head = 1       -- where the head is along the chain, with a fraction
local length = START_LENGTH
local apple

local function wrap(i)
  return (i - 1) % led.count + 1
end

-- whether LED i is part of the snake
local function onSnake(i)
  for k = 0, length - 1 do
    if wrap(math.floor(head) - k) == i then return true end
  end
  return false
end

local function newApple()
  repeat apple = math.random(led.count) until not onSnake(apple)
end
newApple()

-- Called as soon as a top button is pressed or let go, between frames.
function input(button, pressed)
  if not pressed then return end
  if button == 'right' then
    speed = math.min(speed * 1.5, FASTEST)
  else
    speed = math.max(speed / 1.5, SLOWEST)
  end
end

function frame(t, dt)
  local before = math.floor(head)
  head = head + speed * dt
  -- every LED the head passed this frame, so a fast snake does not jump over the apple
  for i = before + 1, math.floor(head) do
    if wrap(i) == apple then
      length = length + 1
      if length > LONGEST then
        length = START_LENGTH
      end
      newApple()
    end
  end

  led.clear()
  led.set(apple, RED)
  for k = 0, length - 1 do
    -- bright at the head, dimmer towards the tail
    led.set(math.floor(head) - k, hsv(0.3, 1, 1 - 0.8 * k / length))
  end
end
