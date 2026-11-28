local sk = require("skapi")

local tags = {
    { "s", "Skald", sk.rgb(137, 180, 250) },
    { "pm", "MinecraftModpack", sk.rgb(250, 179, 135) },
    { "pb", "CocBot", sk.rgb(249, 226, 175) },
    { "pi", "IndieGame", sk.hex(0x690D13) },
    { "m", "Misc", sk.rgb(225, 225, 225) },
    { "l", "Learning", sk.rgb(166, 227, 161) },
    { "b", "Bored", sk.rgb(203, 166, 247) },
    { "d", "DontForget", sk.rgb(255, 109, 108) },
    { "my", "Maybe", sk.rgb(150, 5, 142) },
    { "bg", "Bug", sk.rgb(250, 9, 2) },
    { "v", "Nvim", sk.rgb(57, 235, 20) },
    { "f", "Fun", sk.rgb(210, 9, 102) },
    { "r", "Research", sk.rgb(235, 60, 235) },
    { "u", "Urgent", sk.rgb(255, 0, 35) },
    { "ne", "NotImplementedYet", sk.rgb(155, 255, 35) },
    { "ft", "Feature", sk.rgb(250, 205, 102) },
    { "c", "Change", sk.rgb(150, 105, 12) },
    { "sk", "SkApi", sk.rgb(10, 105, 12) },
}

local function tag_for(flag)
    for _, entry in ipairs(tags) do
        if flag == entry[1] then
            return entry[2]
        end
    end
    return nil
end

local function tag_color(text)
    for _, entry in ipairs(tags) do
        if text and text:find(entry[2], 1, true) then
            return entry[3]
        end
    end
    return nil
end

local function parse_input(input)
    local words = {}
    for word in input:gmatch("%S+") do
        words[#words + 1] = word
    end
    local found = {}
    local clean = {}
    local i = 1
    while i <= #words do
        local word = words[i]
        local flag = word:match("^%-%-?(.+)$")
        if flag then
            local label = tag_for(flag)
            if label then
                found[#found + 1] = "[" .. label .. "]"
            end
        elseif word:sub(1, 1) ~= "-" then
            clean[#clean + 1] = word
        end
        i = i + 1
    end
    return table.concat(clean, " "), found
end

local function split_line(line)
    local text, tag_part = line:match("^(.-)%s*%|%|(.*)$")
    if not text then
        return line, {}
    end
    local out = {}
    for tag in tag_part:gmatch("[^|]+") do
        out[#out + 1] = tag
    end
    return text, out
end

local function load_items()
    local list = sk.store_get_all()
    local items = {}
    for i = 1, list.count or 0 do
        items[i] = list.items[i]
    end
    return items
end

local function save_items(items)
    sk.store_replace_all(items, #items)
end

local function clean_text(line)
    local text = split_line(line)
    return text
end

local function add_todo(input)
    if not input or input == "" then
        return
    end
    local text, found = parse_input(input)
    if text == "" then
        return
    end
    local items = load_items()
    for _, line in ipairs(items) do
        if clean_text(line) == text then
            return
        end
    end
    local entry = text
    if #found > 0 then
        entry = entry .. "||" .. table.concat(found, "|")
    end
    items[#items + 1] = entry
    save_items(items)
end

local function remove_todo(item)
    if not item then
        return
    end
    local target = clean_text(item)
    local items = load_items()
    local kept = {}
    for _, line in ipairs(items) do
        if clean_text(line) ~= target then
            kept[#kept + 1] = line
        end
    end
    save_items(kept)
end

local function refresh(state)
    if not state then
        return
    end
    local eb = state.eb
    local items = load_items()
    sk.populate_listbox({
        lb = state.lb,
        filter = eb and eb.filter or "",
        items = items,
        count = #items,
    })
end

local function enter(state)
    if sk.is_selected(state.lb) then
        remove_todo(state.lb and state.lb.selected_text)
    else
        add_todo(state.eb and state.eb.filter or "")
    end
    return true
end

local function ctrl_enter(state)
    if sk.is_selected(state.lb) then
        remove_todo(state.lb and state.lb.selected_text)
    else
        add_todo(state.eb and state.eb.filter or "")
        sk.set_edit_text("")
    end
    refresh(state)
    return false
end

local function render(state)
    sk.place_editbox({})
    if not sk.listbox_begin({ lb = state.lb, item_height = FontSize * 3 }) then
        return
    end
    while true do
        local ok, ctx = sk.listbox_next()
        if not ok then
            break
        end
        local row_h = ctx.h > 0 and ctx.h or item_h
        local line_h = math.max(12, math.floor((row_h - 3) / 3))
        local text, tag_list = split_line(ctx.text or "")
        sk.draw_text({
            text = text,
            x = ctx.x + 11,
            y = ctx.y + 4,
            align = "LEFT",
        })
        local y = ctx.y + line_h + 10
        local x_offset = 0
        for _, tag in ipairs(tag_list) do
            sk.draw_text({
                text = tag,
                x = 14 + x_offset,
                y = y,
                color = tag_color(tag),
                align = "LEFT",
                pad = 0,
                no_hscroll = true,
            })
            x_offset = x_offset + #tag * math.floor(line_h / 2) + 34
        end
    end
    sk.listbox_end()
end

local function edit(state)
    local selected = state.lb and state.lb.selected_text or ""
    local edited = sk.edit_text_popup({ initial_text = selected, lb = state.lb })
    if not edited or edited == "" then
        return
    end
    local items = load_items()
    for i, line in ipairs(items) do
        if clean_text(line) == clean_text(selected) then
            items[i] = edited
            break
        end
    end
    save_items(items)
    refresh(state)
end

sk.add_feature({
    display_name = "Todo",
    on_action = function()
        return sk.window({
            title = "Todo",
            refresh = refresh,
            enter = enter,
            ctrl_enter = ctrl_enter,
            render = render,
            edit = edit,
        })
    end,
    icon = "/list-check.png",
})
