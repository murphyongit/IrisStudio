-- S1iris: the match HUD's parts, for the editor's Look tab.
--
-- Installed as s1x\ui_scripts\s1iris\__init__.lua; S1x loads it when the game's menus start.
-- It tells the editor what parts the HUD has (in dvars: s1iris_hud_count, s1iris_hud_0 and so on, each
-- "depth name") and hides the parts the editor names in the dvar s1iris_hud_hide. Nothing is changed
-- while that dvar is empty.

if game and game.issingleplayer and game:issingleplayer() then
	return
end

local MAX_DEPTH = 3
local MAX_PARTS = 150

local function clean(text)
	return (tostring(text):gsub("[^%w_%-%.]", "_"))
end

local function children(element)
	local list = {}
	local ok, child = pcall(function() return element:getFirstChild() end)
	local guard = 0
	while ok and child and guard < 200 do
		list[#list + 1] = child
		guard = guard + 1
		local current = child
		ok, child = pcall(function() return current:getNextSibling() end)
	end
	return list
end

-- every part down to MAX_DEPTH: {element, depth, name}; a part with no id of its own is named by its place
local function parts(hud)
	local found = {}
	local function walk(element, depth, prefix)
		if depth > MAX_DEPTH or #found >= MAX_PARTS then
			return
		end
		for index, child in ipairs(children(element)) do
			local id = nil
			pcall(function() id = child.id end)
			local name = prefix .. ((type(id) == "string" and id ~= "") and clean(id) or ("part" .. index))
			if #found < MAX_PARTS and id ~= "s1iris_ticker" then
				found[#found + 1] = {element = child, depth = depth, name = name}
				walk(child, depth + 1, name .. ".")
			end
		end
	end
	walk(hud, 0, "")
	return found
end

local function hidden_names()
	local names = {}
	local ok, text = pcall(function() return Engine.GetDvarString("s1iris_hud_hide") end)
	if ok and type(text) == "string" then
		for name in text:gmatch("%S+") do
			names[name] = true
		end
	end
	return names
end

local told = ""
local function tell(found)
	local lines = {}
	for index, part in ipairs(found) do
		lines[index] = part.depth .. " " .. part.name
	end
	local signature = table.concat(lines, "|")
	if signature == told then
		return
	end
	told = signature
	for index, line in ipairs(lines) do
		Engine.Exec("set s1iris_hud_" .. (index - 1) .. " \"" .. line .. "\"")
	end
	Engine.Exec("set s1iris_hud_count \"" .. #lines .. "\"")
end

local hidden_now = {}
local ticks = 0

local function tick(hud)
	ticks = ticks + 1
	local found = parts(hud)
	if ticks % 20 == 1 then
		tell(found)
	end
	local wanted = hidden_names()
	for _, part in ipairs(found) do
		local element = part.element
		if wanted[part.name] then
			-- again every tick: the HUD's own code shows a part again when it changes
			pcall(function()
				element:registerAnimationState("s1iris_hidden", {alpha = 0})
				element:animateToState("s1iris_hidden")
			end)
			hidden_now[part.name] = true
		elseif hidden_now[part.name] then
			pcall(function()
				element:registerAnimationState("s1iris_shown", {alpha = 1})
				element:animateToState("s1iris_shown")
			end)
			hidden_now[part.name] = nil
		end
	end
end

local function attach(hud)
	told = ""
	hidden_now = {}
	local ticker = LUI.UIElement.new({
		leftAnchor = true,
		topAnchor = true,
		left = 0,
		top = 0,
		width = 1,
		height = 1
	})
	ticker.id = "s1iris_ticker"
	ticker:addElement(LUI.UITimer.new(100, "s1iris_tick"))
	ticker:registerEventHandler("s1iris_tick", function()
		pcall(tick, hud)
	end)
	hud:addElement(ticker)
end

local build = LUI.MenuBuilder.m_types_build["mp_hud"]
if build then
	LUI.MenuBuilder.m_types_build["mp_hud"] = function(...)
		local hud = build(...)
		pcall(attach, hud)
		return hud
	end
end
