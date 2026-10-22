// God mode (JavaScript): invulnerability per player, plus a free-drawing button in the mod's tab.
const apply = () => {
  uc.player(1).invulnerable = settings.get('player1');
  uc.player(2).invulnerable = settings.get('player2');
};

apply();
on('on_settings_changed', apply);

ui.draw(() => {
  ui.text('Damage meter of player 1 (0 unhurt, 50 dead):');
  const p = uc.player(1);
  if (!p.present) {
    ui.text('not in a mission');
    return;
  }
  ui.text(String(Math.round(p.damage * 10) / 10));
  ui.same_line();
  if (ui.button('Heal now')) p.damage = 0;
});
