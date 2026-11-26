-- A pride flag's stripes scroll in a figure 8 around the S.
-- Pick the flag below, or 'all' to show each one in turn, you can just delete some of
-- them to pick a subset

local flag = 'rainbow'
-- rainbow, trans, bi, pan, lesbian, gay, ace, aro, nonbinary, genderfluid,
-- genderqueer, agender, intersex, or 'all'

local speed = 0.15   -- flags per second passing an LED
local repeats = 1    -- how many times the flag fits around the figure 8
local blend = 0.3    -- how soft the edge between two stripes is, 0 to 1 of a stripe
local cycle = 8      -- seconds per flag with 'all'
local boost = 0.5    -- 0 to 1: deepens pale colours, which LEDs show washed out; white and grey stay

local FLAGS = {
  rainbow     = { 0xe40303, 0xff8c00, 0xffed00, 0x008026, 0x004cff, 0x732982 },
  trans       = { 0x5bcefa, 0xf5a9b8, 0xffffff, 0xf5a9b8, 0x5bcefa },
  bi          = { 0xd60270, 0xd60270, 0x9b4f96, 0x0038a8, 0x0038a8 },
  pan         = { 0xff218c, 0xffd800, 0x21b1ff },
  lesbian     = { 0xd52d00, 0xff9a56, 0xffffff, 0xd362a4, 0xa30262 },
  gay         = { 0x078d70, 0x26ceaa, 0x98e8c1, 0xffffff, 0x7bade2, 0x5049cc, 0x3d1a78 },
  ace         = { 0x000000, 0xa3a3a3, 0xffffff, 0x800080 },
  aro         = { 0x3da542, 0xa7d379, 0xffffff, 0xa9a9a9, 0x000000 },
  nonbinary   = { 0xfcf434, 0xffffff, 0x9c59d1, 0x2c2c2c },
  genderfluid = { 0xff76a4, 0xffffff, 0xc011d7, 0x000000, 0x2f3cbe },
  genderqueer = { 0xb57edc, 0xffffff, 0x4a8123 },
  agender     = { 0x000000, 0xbcc4c7, 0xffffff, 0xb7f684, 0xffffff, 0xbcc4c7, 0x000000 },
  intersex    = { 0xffd800, 0xffd800, 0x7902aa, 0xffd800, 0xffd800 },  -- a purple ring on yellow
}
local ORDER = { 'rainbow', 'trans', 'bi', 'pan', 'lesbian', 'gay', 'ace', 'aro',
                'nonbinary', 'genderfluid', 'genderqueer', 'agender', 'intersex' }

if flag ~= 'all' and not FLAGS[flag] then error('unknown flag ' .. flag) end

-- the same colour with its saturation moved towards full by `boost`
local function saturate(c)
  local r, g, b = split(c)
  local max, min = math.max(r, g, b), math.min(r, g, b)
  if max == 0 or (max - min) / max < 0.1 then return c end  -- black, white and greys
  local d = max - min
  local h
  if max == r then h = ((g - b) / d) % 6
  elseif max == g then h = (b - r) / d + 2
  else h = (r - g) / d + 4 end
  local s = 1 - (1 - d / max) * (1 - boost)
  return hsv(h / 6, s, max / 255)
end
for _, stripes in pairs(FLAGS) do
  for i, c in ipairs(stripes) do stripes[i] = saturate(c) end
end

-- led.x and led.y go from 0 to 1 on each axis
local W, H = 22.5, 102.1
local function gap(a, b)
  local dx, dy = (led.x[a] - led.x[b]) * W, (led.y[a] - led.y[b]) * H
  return math.sqrt(dx * dx + dy * dy)
end

-- where each LED lies along the figure 8, from 0 to 1
local place = {}
local length = 0
for i = 1, led.count do
  place[i] = length
  length = length + gap(i, i % led.count + 1)  -- the last step closes the 8
end
for i = 1, led.count do place[i] = place[i] / length end

-- the colour at u along the flag, which repeats every 1
local function flagAt(stripes, u)
  local n = #stripes
  local x = (u % 1) * n
  local k = math.floor(x)
  local f = x - k
  local c = stripes[k + 1]
  if blend > 0 and f > 1 - blend then
    return mix(c, stripes[(k + 1) % n + 1], (f - (1 - blend)) / blend)
  end
  return c
end

function frame(t)
  local stripes = FLAGS[flag] or FLAGS[ORDER[math.floor(t / cycle) % #ORDER + 1]]
  for i = 1, led.count do
    led.set(i, flagAt(stripes, place[i] * repeats - t * speed))
  end
end
