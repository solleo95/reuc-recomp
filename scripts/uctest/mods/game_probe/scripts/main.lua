-- Test-only: logs the mission clock, the hit counters and the pause flag twice a second of game steps (the harness checks that they move and that pause shows).
local steps = 0
on('on_step', function()
  steps = steps + 1
  if steps % 15 ~= 0 or not uc.player(1).present then return end
  log.write(string.format('game time=%d paused=%s hits=%d crit=%d objects=%d stars=%d', uc.game.time, tostring(uc.game.paused), uc.player(1).hits, uc.player(1).critical_hits, uc.player(1).objects_destroyed, uc.game.stars))
end)
