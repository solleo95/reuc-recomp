-- Test-only: logs weapon / health / reserve of player 1 once it is in the game, then sets the health to one half (the harness reads the damage meter).
local present_steps, logged, set = 0, false, false
on('on_step', function()
  if not uc.player(1).present then return end
  present_steps = present_steps + 1
  if present_steps >= 30 and not logged then
    logged = true
    log.write(string.format('player weapon=%d health=%.2f reserve=%s', uc.player(1).weapon, uc.player(1).health, tostring(uc.player(1).reserve)))
  end
  if present_steps >= 120 and not set then
    set = true
    uc.player(1).health = 0.5
  end
end)
