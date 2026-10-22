-- Test-only mod: logs the arguments of GameWeaponUpdate now and then (the harness checks the log).
local calls = 0
hook.pre('GameWeaponUpdate', function(regs)
  calls = calls + 1
  if calls % 120 == 1 then log.write(string.format('hook r3=%08X r4=%d calls=%d', regs.r3, regs.r4, calls)) end
end)
