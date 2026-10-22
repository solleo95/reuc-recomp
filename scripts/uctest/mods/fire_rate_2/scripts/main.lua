-- Test-only: automatic handgun at twice the rate (weapon.fire_rate).
for _, id in ipairs({ 0, 19 }) do
  uc.weapon(id).fire_mode = 'auto'
  uc.weapon(id).fire_rate = 2
end
