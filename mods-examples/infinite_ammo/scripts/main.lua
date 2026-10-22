-- Infinite ammo: the game API keeps the magazine full while the switch is on.
local function apply()
  uc.player(1).infinite_ammo = settings.get('player1')
  uc.player(2).infinite_ammo = settings.get('player2')
end

apply()                                  -- the saved choices, as the game starts
on('on_settings_changed', apply)         -- and whenever the player changes them in the Insert menu
