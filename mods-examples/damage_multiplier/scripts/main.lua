-- Damage multiplier: remembers each weapon's original damage the first time the game has filled the table, then writes original * scale.
local base = nil

local function apply()
  if not base then
    base = {}
    for id = 0, 19 do base[id] = uc.weapon(id).damage end
  end
  local scale = settings.get('scale')
  for id = 0, 19 do
    uc.weapon(id).damage = base[id] * scale   -- the API keeps the value even when the game refills its table
  end
end

on('on_step', function()
  if not base and uc.weapon(0).damage > 0 then apply() end   -- wait until the game has loaded its weapon data
end)

on('on_settings_changed', function()
  if base then apply() end
end)
