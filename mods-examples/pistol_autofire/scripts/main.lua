-- Pistol auto-fire: switch the handgun records (0, and 19 which is the same weapon) between single shots and held-trigger fire.
local HANDGUNS = { 0, 19 }

local function apply()
  local mode = settings.get('auto') and 'auto' or 'semi'
  for _, id in ipairs(HANDGUNS) do
    uc.weapon(id).fire_mode = mode
  end
end

apply()
on('on_settings_changed', apply)
