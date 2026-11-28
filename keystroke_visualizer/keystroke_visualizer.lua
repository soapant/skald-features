local sk = require("skapi")

local NAME = "keystroke_visualizer"
local W = 230
local H = 50

local display = ""
local last = ""
local count = 0

local function render()
  while true do
    local ok, ev = sk.poll_key()
    if not ok then
      break
    end
    if ev.down and not ev["repeat"] then
      local label = sk.hotkey_to_string({ vk = ev.vk, mods = ev.mods }) or ""
      if label == last then
        count = count + 1
      else
        last = label
        count = 1
      end
      display = count > 1 and (label .. " " .. count .. "x") or label
    end
  end

  sk.draw_text({
      x = 60,
      y = 10,
      w = W,
      h = H,
      text = display,
      align = "LEFT",
  })
end

local function toggle()
  if sk.message_active(NAME) then
    sk.dismiss_message(NAME)
    return
  end
  display = ""
  last = ""
  count = 0
  sk.overlay({
      name = NAME,
      slot = sk.BOTTOM_CENTER,
      render = render,
      w = W,
      h = H,
      fps = 10,
  })
end

sk.add_feature({
    display_name = "Key Visualizer",
    on_action = toggle,
    type = "Feature",
})
