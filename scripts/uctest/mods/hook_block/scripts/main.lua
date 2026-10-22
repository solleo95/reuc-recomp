-- Test-only mod: clears the "allowed" flag (r4) of GameWeaponUpdate; the harness checks whether firing stops.
hook.pre('GameWeaponUpdate', function(regs)
  regs.r4 = 0
end)
