-- Test-only: logs the camera and the direction of the nearest living zombie relative to it (the harness checks eye height and that the zombie is ahead).
local steps = 0
on('on_step', function()
  steps = steps + 1
  if steps % 60 ~= 0 then return end
  local ok, p = pcall(function() return uc.camera.position end)
  if not ok then return end
  local f = uc.camera.forward
  local best, bestd
  for _, e in ipairs(uc.enemies.list()) do
    if e.hp > 0 then
      local dx, dy, dz = e.x - p[1], e.y - p[2], e.z - p[3]
      local d = math.sqrt(dx * dx + dy * dy + dz * dz)
      if d < 400 and (not bestd or d < bestd) then bestd = d; best = (dx * f[1] + dy * f[2] + dz * f[3]) / d end
    end
  end
  if best then log.write(string.format('camera eye_y=%.1f forward=%.3f,%.3f,%.3f ahead=%.3f dist=%.1f', p[2], f[1], f[2], f[3], best, bestd)) end
end)
