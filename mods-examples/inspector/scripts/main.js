// Inspector (JavaScript): a read-only window onto everything the mod API exposes. Open it with the gear button of this mod in the Mods tab.
// Copy this folder as a starting point for your own debugging tools; the helpers are in lib/inspect.js.
import { EventRing, WATCH_TYPES, clockText, enemyLine, eventLine, nearest, parseAddress, playerLine, readWatch, snapshot } from './lib/inspect.js';

const attempt = (read, fallback = null) => { try { return read(); } catch (e) { return fallback; } };   // a part that does not exist (no mission, no scene) is null, never an error

const ring = new EventRing(settings.get('events_kept'));
for (const name of ['on_weapon_fired', 'on_damage_taken', 'on_reload', 'on_player_died', 'on_enemy_spawned', 'on_enemy_killed', 'on_enemy_headshot']) {
  on(name, (data) => ring.push(eventLine(attempt(() => uc.game.time), name.slice(3), data)));
}
on('on_settings_changed', () => ring.setLimit(settings.get('events_kept')));

const readPlayer = (slot) => attempt(() => {
  const p = uc.player(slot);
  if (!p.present) return null;
  return {
    health: p.health, damage: p.damage, weapon: p.weapon, magazine: p.magazine, reserve: attempt(() => p.reserve), grenades: p.grenades,
    hits: p.hits, critical_hits: p.critical_hits, objects_destroyed: p.objects_destroyed,
  };
});

const readState = () => ({
  players: [readPlayer(1), readPlayer(2)],
  game: attempt(() => ({ time: uc.game.time, paused: uc.game.paused, stars: uc.game.stars })),
  camera: attempt(() => ({ position: uc.camera.position, forward: uc.camera.forward })),
  enemies: attempt(() => uc.enemies.list(), []),
});

const logSnapshot = (state) => log.write(snapshot(state));

// A snapshot every `log_every` game seconds (30 steps each); 0 is off.
let steps = 0;
on('on_step', () => {
  const every = settings.get('log_every');
  if (!(every > 0)) { steps = 0; return; }
  if (++steps >= every * 30) { steps = 0; logSnapshot(readState()); }
});

const meters = (v) => (Array.isArray(v) ? v.map((x) => (x / 10).toFixed(1)).join(', ') : '?');
const note = (text) => ui.text(`! ${text}`);

const watches = Array.from({ length: 4 }, () => ({ text: '', type: 2 }));
const lookup = (name) => attempt(() => uc.symbols.lookup(name));

function drawWatches() {
  watches.forEach((w, i) => {
    w.text = ui.input_text(`Address or symbol ${i + 1}`, w.text);
    ui.same_line();
    w.type = ui.combo(`Type ${i + 1}`, w.type, WATCH_TYPES);
    const parsed = parseAddress(w.text, lookup);
    if (parsed.empty) { ui.text('  type an address like 0x80100200, a decimal number or a symbol name'); return; }
    if (parsed.error) { note(parsed.error); return; }
    const read = readWatch(uc.mem, parsed.address, WATCH_TYPES[w.type]);
    if (read.error) { note(read.error); return; }
    const symbol = attempt(() => uc.symbols.name_of(parsed.address));
    ui.text(`  0x${parsed.address.toString(16).toUpperCase()}${symbol ? ` (${symbol})` : ''} = ${read.text}`);
  });
}

ui.draw(() => {
  const state = readState();
  const present = state.players.map((p, i) => (p ? playerLine(i + 1, p) : null)).filter(Boolean);
  if (present.length === 0) ui.text('not in a mission'); else present.forEach((line) => ui.text(line));

  if (ui.collapsing_header('Mission and camera')) {
    if (!state.game) note('the mission clock cannot be read');
    else ui.text(`Mission time ${clockText(state.game.time)}  paused ${state.game.paused}  stars ${state.game.stars}`);
    if (!state.camera) note('no camera: not in a scene');
    else ui.text(`Camera at (${meters(state.camera.position)}) m, looking (${state.camera.forward.map((x) => x.toFixed(2)).join(', ')})`);
  }
  if (ui.collapsing_header('Pad')) {
    const held = attempt(() => uc.input.held);
    if (held === null) note('the pad cannot be read');
    else ui.text(`Held buttons 0x${(held >>> 0).toString(16).toUpperCase().padStart(8, '0')}  fire held ${attempt(() => uc.input.fire_held)}  aim (${meters(attempt(() => uc.input.aim))})`);
  }
  if (ui.collapsing_header('Enemies')) {
    const alive = state.enemies.filter((e) => e.hp > 0).length;
    if (state.enemies.length === 0) ui.text('no enemies');
    else {
      ui.text(`${alive} of ${state.enemies.length} zombies alive; nearest first (positions in meters)`);
      const shown = nearest(state.enemies, state.camera, settings.get('enemies_shown'));
      shown.forEach((e) => ui.text(enemyLine(e, state.camera)));
      if (state.enemies.length > shown.length) ui.text(`... and ${state.enemies.length - shown.length} more`);
    }
  }
  if (ui.collapsing_header('Events')) {
    if (ui.button('Clear events')) ring.clear();
    const lines = ring.lines().reverse();   // newest first
    if (lines.length === 0) ui.text('no events yet'); else lines.forEach((line) => ui.text(line));
  }
  if (ui.collapsing_header('Memory watch')) drawWatches();
  if (ui.collapsing_header('Snapshot')) {
    if (ui.button('Log snapshot')) logSnapshot(state);
    ui.text('One line of JSON in debug\\mods.log, for bug reports.');
  }
});
