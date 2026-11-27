local sk = require("skapi")

local function get_zoxide_match(query)
    if not query then
        return nil
    end
    local clean_query = query:gsub("[\r\n]+", "")
    if clean_query == "" then
        return nil
    end

    local real_home = sk.resolve_user_path()
    local platform = sk.platform()
    local command

    if platform == "win32" then
        command = string.format(
            "cmd.exe /c \"set \"HOME=%s\" & set \"_ZO_DATA_DIR=%s\\AppData\\Local\\zoxide\" & zoxide query -- \"%s\" 2>&1\"",
            real_home, real_home, clean_query
        )
    elseif platform == "linux" then
        command = string.format(
            "export HOME=\"%s\"; export _ZO_DATA_DIR=\"%s/.local/share/zoxide\"; export PATH=\"%s/.local/bin:%s/.cargo/bin:/usr/local/bin:$PATH\"; zoxide query -- \"%s\" 2>&1",
            real_home, real_home, real_home, real_home, clean_query
        )
    else
        return nil
    end

    local out = sk.run_terminal_command(command)
    if not out then
        return nil
    end
    out = out:gsub("[\r\n]+$", "")
    if out == "" or out:find("no match found") then
        return nil
    end
    return out
end

local function zoxide_refresh(state)
    if not state then
        return
    end
    local lines = sk.store_get_all() or {}
    local items = lines.items or {}
    local count = lines.count or #items
    local eb = state.eb

    sk.populate_listbox({
        lb = state.lb,
        filter = eb and eb.filter or "",
        items = items,
        count = count,
    })
end

local function zoxide_edit(state)
    if not state then
        return
    end
    local lb = state.lb
    local edited = sk.edit_text_popup({
        initial_text = lb and lb.selected_text or "",
        lb = state.lb,
    })
    if not edited or edited == "" then
        return
    end

    local lines = sk.store_get_all() or {}
    local items = lines.items or {}
    if #items > 0 then
        items[#items] = edited
    else
        items[1] = edited
    end
    sk.store_replace_all(items, #items)
    zoxide_refresh(state)
end

local function zoxide_enter(state)
    if not state then
        return true
    end
    local lb = state.lb
    local text = lb and lb.selected_text or ""
    local eb = state.eb
    if text == "" and eb then
        text = eb.filter or ""
    end

    local lines = sk.store_get_all() or {}
    local items = lines.items or {}
    local found = false
    for i = 1, #items do
        if items[i] == text then
            found = true
            break
        end
    end
    if not found and text ~= "" then
        items[#items + 1] = text
        sk.store_replace_all(items, #items)
    end

    local path = get_zoxide_match(text)
    if path then
        sk.launch_terminal_at_path(path, nil)
    end
    return true
end

local function trigger_zoxide()
    local lines = sk.store_get_all() or {}
    sk.window({
        title = "Zoxide",
        items = lines.items or {},
        refresh = zoxide_refresh,
        enter = zoxide_enter,
        edit = zoxide_edit,
    })
end

sk.add_feature({
    display_name = "Zoxide",
    on_action = trigger_zoxide,
    icon = "/folder.png",
})
