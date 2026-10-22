-- Test-only: sets the health of the nearest living zombie to 0 (once) and logs its id; the harness watches what the game does with it.
local done = false
on('on_enemy_spawned', function(e) log.write('event spawned id=' .. e.id) end)
on('on_enemy_killed', function(e) log.write('event killed id=' .. e.id) end)
on('on_step', function()
  if done or not uc.player(1).present then return end
  local list = uc.enemies.list()
  local best, bestd
  for _, e in ipairs(list) do
    if e.hp > 0 then
      local d = e.x * e.x + e.z * e.z
      if not bestd or d < bestd then best, bestd = e, d end
    end
  end
  if best then
    done = true
    uc.enemy(best.id).hp = 0
    log.write('enemy killed id=' .. best.id .. ' alive_after=' .. tostring(uc.enemy(best.id).alive))
  end
end)
