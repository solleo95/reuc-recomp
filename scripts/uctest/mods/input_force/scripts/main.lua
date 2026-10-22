-- Test-only: the trigger is held for the player (input.force_held), and the mod logs what the game sees (input.fire_held) once.
local logged = false
on('on_step', function()
  if not uc.player(1).present then return end
  uc.input.force_held = 0x400
  if uc.input.fire_held and not logged then logged = true; log.write('input fire_held=true held=' .. uc.input.held) end
end)
