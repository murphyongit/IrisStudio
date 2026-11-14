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

-- What a kill puts on screen (the kill feed's lines, the score popups, the medals) is added to the HUD as
-- it happens and takes itself off after a few seconds of the clock on the wall, not of the recording: gone
-- back in, the recording left them standing. The editor counts its rewinds in the dvar s1iris_hud_flush;
-- when the count changes, what was added under those parts is closed.
-- What was "added" is told from what is always there by waiting: once nothing under them has changed for
-- ten seconds, whatever is there is the HUD's own, and anything not among it later was added since.
local WATCHED = {"obituary", "pointsPopup", "splashesHud", "killstreakEarned"}
local own = nil
local quiet, last_count, last_flush = 0, -1, nil

local function below(element, into, parent_of, depth)
	if depth > 8 or #into > 600 then
		return
	end
	for _, child in ipairs(children(element)) do
		into[#into + 1] = child
		parent_of[child] = element
		below(child, into, parent_of, depth + 1)
	end
end

local function flush(found)
	local roots = {}
	for _, part in ipairs(found) do
		if part.depth == 2 then
			for _, word in ipairs(WATCHED) do
				if part.name:find(word, 1, true) then
					roots[#roots + 1] = part.element
				end
			end
		end
	end
	local all, parent_of = {}, {}
	for _, root in ipairs(roots) do
		all[#all + 1] = root
		below(root, all, parent_of, 0)
	end
	if #all ~= last_count then
		last_count = #all
		quiet = 0
	else
		quiet = quiet + 1
	end
	if quiet == 100 then
		own = {}
		for _, element in ipairs(all) do
			own[element] = true
		end
	end
	local ok, count = pcall(function() return Engine.GetDvarString("s1iris_hud_flush") end)
	if not ok or type(count) ~= "string" then
		return
	end
	if last_flush == nil then
		last_flush = count
	elseif count ~= last_flush then
		last_flush = count
		if own then
			for _, element in ipairs(all) do
				local parent = parent_of[element]
				-- (only what was added straight onto something of the HUD's own: a line, a popup)
				if not own[element] and parent and own[parent] then
					pcall(function() element:close() end)
				end
			end
		end
		quiet = 0
	end
end

local function tick(hud)
	ticks = ticks + 1
	local found = parts(hud)
	if ticks % 20 == 1 then
		tell(found)
	end
	pcall(flush, found)
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

-- Hidden at once, and kept hidden: the HUD's own code shows a part again whenever it changes, and a popup
-- (a scorestreak made ready, say) is a new part each time, on screen before the slower tick has seen it.
local function hide(element)
	pcall(function()
		element:registerAnimationState("s1iris_hidden", {alpha = 0})
		element:animateToState("s1iris_hidden")
	end)
end

-- every frame: only the hiding
local function fast(hud)
	local wanted = hidden_names()
	if next(wanted) == nil then
		return
	end
	for _, part in ipairs(parts(hud)) do
		if wanted[part.name] then
			hide(part.element)
			hidden_now[part.name] = true
		end
	end
end

-- What is added straight onto one of the HUD's layers ("static", "shakeable") while its name is among the
-- hidden ones never shows at all: the layer's own way of taking a new part is wrapped.
local function watch(layer, name)
	local ok, add = pcall(function() return layer.addElement end)
	if not ok or type(add) ~= "function" then
		return
	end
	pcall(function()
		layer.addElement = function(self, child, ...)
			local result = add(self, child, ...)
			pcall(function()
				local id = child.id
				if type(id) == "string" and id ~= "" and id ~= "s1iris_ticker" and hidden_names()[name .. "." .. clean(id)] then
					hide(child)
				end
			end)
			return result
		end
	end)
end

local function attach(hud)
	told = ""
	hidden_now = {}
	for _, part in ipairs(parts(hud)) do
		if part.depth == 0 then
			watch(part.element, part.name)
		end
	end
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
	ticker:addElement(LUI.UITimer.new(1, "s1iris_fast"))
	ticker:registerEventHandler("s1iris_fast", function()
		pcall(fast, hud)
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
