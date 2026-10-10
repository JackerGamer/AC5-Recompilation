-- Dead zone probe: finds how far each stick axis, and each pressure trigger,
-- has to move before the game reacts.
--
-- In flight, press the probe's button (L3 by default -- T with the keyboard
-- layout here; set it with "button" in mod.toml) to start the next item.  The mod takes over that axis or trigger and pushes it a little
-- further every step, logging each value.  The moment the aircraft (or the
-- camera) starts to respond, press the button again: the result is logged and kept in
-- saves/mods/deadzone_probe.txt.  Then press it for the next item.  An item
-- that gets all the way to 100% with no press is recorded as "no response".
-- Hands off the controls being probed while it runs.

local NEUTRAL = 127                      -- PS2_PAD_ANALOG_NEUTRAL

local ITEMS = {
    ["ly+"] = { field = "ly", sign = 1,  what = "左摇杆 下（俯仰：机头抬起）" },
    ["ly-"] = { field = "ly", sign = -1, what = "左摇杆 上（俯仰：机头压低）" },
    ["lx+"] = { field = "lx", sign = 1,  what = "左摇杆 右（滚转：向右）" },
    ["lx-"] = { field = "lx", sign = -1, what = "左摇杆 左（滚转：向左）" },
    ["rx+"] = { field = "rx", sign = 1,  what = "右摇杆 右（视角：向右看）" },
    ["rx-"] = { field = "rx", sign = -1, what = "右摇杆 左（视角：向左看）" },
    ["ry+"] = { field = "ry", sign = 1,  what = "右摇杆 下（视角：向下看）" },
    ["ry-"] = { field = "ry", sign = -1, what = "右摇杆 上（视角：向上看）" },
    ["l2"]  = { trigger = "l2", other = "r2", what = "L2 压力（偏航：向左）" },
    ["r2"]  = { trigger = "r2", other = "l2", what = "R2 压力（偏航：向右）" },
}

local function cfg_num(key, default, lo, hi)
    local v = tonumber(ac5.config(key) or "")
    if not v then return default end
    return math.max(lo, math.min(hi, v))
end

local function say(fmt, ...)
    ac5.log(string.format("[死区测量] " .. fmt, ...))
end

local sequence = {}
for name in string.gmatch(ac5.config("sequence") or "ly+,lx+,rx+,ry+,l2,r2", "[^,%s]+") do
    if ITEMS[name] then
        sequence[#sequence + 1] = name
    else
        say("忽略未知的测量项 '%s'", name)
    end
end
local step_pct = cfg_num("step", 0.8, 0.1, 10.0)
local interval = math.floor(cfg_num("interval", 15, 5, 600))
local reaction = cfg_num("reaction", 1.0, 0.0, 5.0)

local next_item = 1
local cur = nil                          -- the item being probed, or nil
local level, fields = 0.0, 0             -- level: 0..1 of full travel
local BUTTON_NAME = string.lower(ac5.config("button") or "l3")
if not ac5.BTN[BUTTON_NAME] then
    say("button '%s' is not a PS2 button; using l3", BUTTON_NAME)
    BUTTON_NAME = "l3"
end
local BUTTON = ac5.BTN[BUTTON_NAME]
local KEY = BUTTON_NAME:upper()
local held_was = false
local results = {}

local function item_range(item)
    if item.trigger then return 255 end
    return item.sign > 0 and (255 - NEUTRAL) or NEUTRAL      -- 128 or 127
end

local function units(item, frac) return math.floor(frac * item_range(item) + 0.5) end

local function record(name, text)
    results[#results + 1] = string.format("%-4s %s", name, text)
    ac5.store(name, text)
end

local function start()
    if next_item > #sequence then
        say("全部 %d 项已测完。要重测，请重新启动游戏。", #sequence)
        return
    end
    local name = sequence[next_item]
    cur = { name = name, def = ITEMS[name] }
    level, fields = 0.0, 0
    say("第 %d/%d 项：%s。每 %.2f 秒推进 %.1f%%，看到反应时按 %s。",
        next_item, #sequence, cur.def.what, interval / 60.0, step_pct, KEY)
end

local function summary()
    say("======== 测量结果 ========")
    for _, line in ipairs(results) do say("%s", line) end
    say("结果已保存在 saves/mods/deadzone_probe.txt。测完请在 mod.toml 里设 enabled = false。")
end

local function finish(full)
    local name, def = cur.name, cur.def
    cur = nil
    next_item = next_item + 1
    if full then
        record(name, "推到 100% 仍未按键：没有反应，或没在飞行中")
        say("%s：推到 100%% 仍未停止，记为“无反应”。", def.what)
    else
        local back = reaction * 60.0 / interval * step_pct / 100.0
        local lo = math.max(0.0, level - back)
        record(name, string.format("%.1f%%-%.1f%% (%d-%d/%d)", lo * 100, level * 100,
                                   units(def, lo), units(def, level), item_range(def)))
        say("%s：停止于 %.1f%%，扣除反应时间后，游戏开始响应的位置约在 %.1f%%–%.1f%%。",
            def.what, level * 100, lo * 100, level * 100)
    end
    if next_item > #sequence then
        summary()
    else
        say("下一项：%s。准备好后按 %s。", ITEMS[sequence[next_item]].what, KEY)
    end
end

local function apply(p)
    local def = cur.def
    if def.trigger then
        local v = units(def, level)
        local bt, bo = ac5.BTN[def.trigger], ac5.BTN[def.other]
        p.buttons = p.buttons & ~bo
        p[def.other] = 0
        if v > 0 then p.buttons = p.buttons | bt else p.buttons = p.buttons & ~bt end
        p[def.trigger] = v
    else
        p[def.field] = NEUTRAL + def.sign * units(def, level)
    end
end

ac5.on_input(function(pads)
    local p = pads[1]
    if not p then return end
    local held = (p.buttons & BUTTON) ~= 0
    p.buttons = p.buttons & ~BUTTON              -- the button belongs to the probe while it is installed
    if held and not held_was then
        if cur then finish(false) else start() end
    end
    held_was = held
    if not cur then return end

    fields = fields + 1
    if fields >= interval then
        fields = 0
        level = level + step_pct / 100.0
        if level >= 1.0 then
            level = 1.0
            apply(p)
            finish(true)
            return
        end
        say("%5.1f%%", level * 100)
    end
    apply(p)
end)

say("已加载，共 %d 项：%s。飞行中按 %s 开始第一项（键鼠方案下请在 F4 菜单里给 %s 绑一个键）。",
    #sequence, table.concat(sequence, ", "), KEY, KEY)
