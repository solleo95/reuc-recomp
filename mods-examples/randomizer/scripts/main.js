// Randomizer (JavaScript): weapon stats, zombie hit points and the starting weapon from a seed. The same seed always gives the same game.
// The game's own weapon values are read once (the base); every plan is made from the base, so changing the seed or the strength never compounds.
import { enemyHp, planWeapon, startPool, startWeapon } from './lib/random.js';

const attempt = (read, fallback = null) => { try { return read(); } catch (e) { return fallback; } };
const START_DELAY = 90;   // steps (3 game seconds)
const WEAPONS = 20, FIELDS = ['damage', 'fire_rate', 'capacity', 'reload_time'];

let base = null;         // the game's values per weapon id, read once its table is filled
let plan = [];           // what is written now
const zombies = new Set();   // ids already handled in this scene
const changed = [];      // the latest zombie changes {id, from, to}
const started = {};      // slot -> weapon id given at the start of the mission
let present = false;     // was player 1 in the mission at the last step
let pendingStart = false;
let waited = 0;          // steps since the mission began: the HUD needs a moment before it follows a weapon switch

const seed = () => settings.get('seed');
const strength = () => settings.get('strength');

const readBase = () => Array.from({ length: WEAPONS }, (_, id) => {
  const w = uc.weapon(id);
  return { damage: w.damage, fire_rate: w.fire_rate, capacity: w.capacity, reload_time: w.reload_time };
});

// Writes the plan (or the base when weapons are switched off); only fields the game has above 0 (a rate of 0 is not a value the API accepts).
function writeWeapons() {
  if (!base) return;
  const on = settings.get('weapons');
  plan = base.map((b, id) => (on ? planWeapon(seed(), strength(), id, b) : b));
  plan.forEach((p, id) => {
    const w = uc.weapon(id);
    for (const field of FIELDS) if (base[id][field] > 0) w[field] = p[field];
  });
}

function randomizeZombie(id, hp) {
  if (zombies.has(id) || !(hp > 0) || !settings.get('enemies')) return;
  zombies.add(id);
  const to = enemyHp(seed(), strength(), id, hp);
  uc.enemy(id).hp = to;
  changed.push({ id, from: hp, to });
  if (changed.length > 12) changed.shift();
}

function giveStartWeapons() {
  const pool = startPool(base);
  for (const slot of [1, 2]) {
    if (!attempt(() => uc.player(slot).present)) continue;
    const id = startWeapon(seed(), slot, pool);
    if (id === null) continue;
    const index = attempt(() => uc.game.give_weapon(slot, id));
    if (index !== null) attempt(() => uc.game.select_weapon(slot, index));
    started[slot] = id;
  }
}

on('on_enemy_spawned', (e) => randomizeZombie(e.id, e.hp));
on('on_settings_changed', () => writeWeapons());

let steps = 0;
on('on_step', () => {
  if (!base && attempt(() => uc.weapon(0).damage, 0) > 0) { base = attempt(readBase); writeWeapons(); }
  const here = !!attempt(() => uc.player(1).present);
  if (here && !present) { zombies.clear(); changed.length = 0; pendingStart = true; waited = 0; }   // a mission starts: its zombies and its loadout are new
  if (!here && present) { Object.keys(started).forEach((k) => delete started[k]); }
  present = here;
  if (pendingStart && base && ++waited >= START_DELAY) {
    pendingStart = false;
    if (settings.get('start_weapon')) giveStartWeapons();
  }
  if (here && ++steps % 6 === 0) for (const e of attempt(() => uc.enemies.list(), [])) randomizeZombie(e.id, e.hp);   // zombies that were there before the mod saw the scene
});

function newSeed() {
  let next = 1 + Math.floor(Math.random() * 999999);
  if (next === seed()) next = next % 999999 + 1;
  settings.set('seed', next);
  writeWeapons();
}

const one = (v) => (typeof v === 'number' ? (Math.round(v * 10) / 10).toString() : '?');

ui.draw(() => {
  ui.text(`Seed ${seed()}  strength ${strength()}%`);
  ui.same_line();
  if (ui.button('New seed')) newSeed();
  if (!base) ui.text('Waiting for the game to load its weapon table.');
  if (base && ui.collapsing_header('Weapons')) {
    plan.forEach((p, id) => {
      const b = base[id];
      if (!(b.capacity > 0)) return;   // not a gun
      ui.text(`#${id}: damage ${one(b.damage)} -> ${one(p.damage)}  rate ${one(b.fire_rate)} -> ${one(p.fire_rate)}  magazine ${b.capacity} -> ${p.capacity}  reload ${one(b.reload_time)} -> ${one(p.reload_time)}`);
    });
  }
  if (ui.collapsing_header('Zombies')) {
    if (changed.length === 0) ui.text('no zombie changed in this mission yet');
    changed.forEach((c) => ui.text(`zombie ${c.id}: hit points ${one(c.from)} -> ${one(c.to)}`));
  }
  if (ui.collapsing_header('Start weapon')) {
    const slots = Object.keys(started);
    if (slots.length === 0) ui.text(settings.get('start_weapon') ? 'none given yet in this mission' : 'switched off');
    slots.forEach((slot) => ui.text(`player ${slot} started with weapon #${started[slot]}`));
  }
});
