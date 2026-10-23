// Pure helpers of the randomizer: a seeded generator, the factors, the weapon plan, the zombie rule and the start-weapon pool. Nothing here touches the game.

const WEAPON = 0, ENEMY = 1, START = 2;   // what a draw is for (part of the generator's seed, so changes do not depend on the order they are made in)
export const KINDS = { WEAPON, ENEMY, START };

function mulberry32(state) {
  let a = state >>> 0;
  return () => {
    a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// A generator for one thing (a weapon record, a zombie, a player): numbers in [0, 1), the same for the same (seed, kind, id).
export function rng(seed, kind, id) {
  let h = Math.imul(seed | 0, 2654435761) ^ Math.imul(kind + 1, 40503) ^ Math.imul(id + 1, 69069);
  h = Math.imul(h ^ (h >>> 16), 2246822507);
  h = Math.imul(h ^ (h >>> 13), 3266489909);
  const draw = mulberry32((h ^ (h >>> 16)) >>> 0);
  draw(); draw();   // the first outputs of nearby seeds are alike
  return draw;
}

// log-uniform in [1 / (1 + 2s), 1 + 2s] with s = strength / 100; exactly 1 at strength 0.
export function factor(draw, strength) {
  const s = Math.max(0, Math.min(100, strength)) / 100;
  if (s === 0) return 1;
  return Math.exp((draw * 2 - 1) * Math.log(1 + 2 * s));
}

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

// The planned record for a base record {damage, fire_rate, capacity, reload_time}; a field the game leaves at 0 (or below) stays as it is. The API's limits hold.
export function planWeapon(seed, strength, id, base) {
  const draw = rng(seed, WEAPON, id);
  const f = [factor(draw(), strength), factor(draw(), strength), factor(draw(), strength), factor(draw(), strength)];   // always four draws: a field that is absent does not shift the others
  return {
    damage: base.damage > 0 ? clamp(base.damage * f[0], 0.1, 10000) : base.damage,
    fire_rate: base.fire_rate > 0 ? clamp(base.fire_rate * f[1], 0.05, 60) : base.fire_rate,
    capacity: base.capacity > 0 ? clamp(Math.round(base.capacity * f[2]), 1, 999) : base.capacity,
    reload_time: base.reload_time > 0 ? clamp(base.reload_time * f[3], 0.1, 1000) : base.reload_time,
  };
}

export function enemyHp(seed, strength, id, hp) {
  if (!(hp > 0)) return hp;
  return Math.max(1, Math.round(hp * factor(rng(seed, ENEMY, id)(), strength) * 10) / 10);
}

// Records of the table that are real guns (a magazine and a rate); the table has a few that are not.
export function startPool(base) {
  const ids = [];
  base.forEach((b, id) => { if (b.capacity > 0 && b.fire_rate > 0) ids.push(id); });
  return ids;
}

export function startWeapon(seed, slot, pool) {
  if (pool.length === 0) return null;
  return pool[Math.floor(rng(seed, START, slot)() * pool.length)];
}
