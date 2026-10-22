-- Test-only: replaces the per-step weapon update with nothing (the original must not run) and counts the post hook that follows each replacement.
local posts = 0
hook.replace('GameWeaponUpdate', function(regs) end)
hook.post('GameWeaponUpdate', function(regs)
  posts = posts + 1
  if posts % 120 == 1 then log.write('post calls=' .. posts) end
end)
