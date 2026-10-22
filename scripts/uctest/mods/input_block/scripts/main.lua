-- Test-only: the trigger never reaches the game (input.block_held); if the game ever saw it held the mod logs it.
on('on_step', function()
  uc.input.block_held = 0x400
  if uc.input.fire_held then log.write('input fire_held=SEEN') end
end)
