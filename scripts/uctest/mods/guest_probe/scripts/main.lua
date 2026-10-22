-- Test-only: calls a game function, gives the player a submachine gun and switches to it, then logs the weapon and the magazine.
local steps = 0
on('on_step', function()
  if not uc.player(1).present then return end
  steps = steps + 1
  if steps == 60 then
    local record = uc.guest.call(0x8006C8C8, { 2 })
    local index = uc.game.give_weapon(1, 2, 7)
    local current = uc.game.select_weapon(1, index)
    log.write(string.format('guest record=%08X index=%d current=%s', record, index, tostring(current)))
  end
  if steps > 60 and steps % 15 == 0 then
    log.write(string.format('guest weapon=%d magazine=%d', uc.player(1).weapon, uc.player(1).magazine))
  end
end)
