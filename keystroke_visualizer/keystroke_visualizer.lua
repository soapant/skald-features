local sk = require("skapi")

local NAME = "keystroke_visualizer"

local display = ""
local last = ""
local count = 0
local held = {}
local shown = ""

local function poll()
  while true do
    local ok, ev = sk.poll_key()
    if not ok then
      break
    end
    if ev.down then
      if not held[ev.vk] and not ev["repeat"] then
        held[ev.vk] = true
        local label = sk.hotkey_to_string({ vk = ev.vk, mods = ev.mods }) or ""
        if label == last then
          count = count + 1
        else
          last = label
          count = 1
        end
        display = count > 1 and (label .. " " .. count .. "x") or label
      end
    else
      held[ev.vk] = nil
    end
  end
  if display ~= shown then
    sk.message({ name = NAME, message = display })
    shown = display
  end
end

local function toggle()
  if sk.overlay_active(NAME) then
    sk.dismiss_overlay(NAME)
    return
  end
  display = ""
  last = ""
  count = 0
  held = {}
  shown = ""
  sk.overlay({
      name = NAME,
      slot = sk.BOTTOM_CENTER,
      message = "",
      render = poll,
      fps = 10,
  })
end

sk.add_feature({
    display_name = "Key Visualizer",
    on_action = toggle,
    type = "Feature",
})
