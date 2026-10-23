// Pure helpers of the inspector: formatting, sorting, the event ring, address parsing and memory reads. Positions are [x, y, z] in the game's units (10 cm).

const fixed = (value, digits = 1) => (typeof value === 'number' && Number.isFinite(value) ? value.toFixed(digits).replace(/^-(0\.0+)$/, '$1') : '?');   // no "-0.0"
const two = (n) => String(n).padStart(2, '0');

export function clockText(seconds) {
  if (typeof seconds !== 'number' || !Number.isFinite(seconds) || seconds < 0) return '--:--';
  const whole = Math.floor(seconds);
  return `${two(Math.floor(whole / 60))}:${two(whole % 60)}`;
}

export function playerLine(slot, p) {
  const health = typeof p.health === 'number' ? `${Math.round(p.health * 100)}%` : '?';
  const reserve = p.reserve == null ? 'inf' : p.reserve;
  return `Player ${slot}: health ${health}  weapon ${p.weapon}  magazine ${p.magazine}  reserve ${reserve}  grenades ${p.grenades}`;
}

export function playerStatsLine(p) {
  return `   damage meter ${fixed(p.damage)}/50  hits ${p.hits}  crits ${p.critical_hits}  objects ${p.objects_destroyed}`;
}

const distance = (e, camera) => {
  if (!camera || !Array.isArray(camera.position)) return null;
  const [cx, cy, cz] = camera.position;
  return Math.hypot(e.x - cx, e.y - cy, e.z - cz) / 10;
};

export function enemyLine(e, camera) {
  const d = distance(e, camera);
  return `#${e.id} hp ${fixed(e.hp)}  ${d === null ? '?' : fixed(d)} m  (${fixed(e.x / 10)}, ${fixed(e.y / 10)}, ${fixed(e.z / 10)})`;
}

// The `limit` enemies nearest to the camera (ties by id); without a camera, the first ones by id.
export function nearest(enemies, camera, limit) {
  const keyed = enemies.map((e) => ({ e, d: distance(e, camera) }));
  keyed.sort((a, b) => ((a.d ?? 0) - (b.d ?? 0)) || (a.e.id - b.e.id));
  return keyed.slice(0, Math.max(0, limit)).map((k) => k.e);
}

const valueText = (v) => (typeof v === 'number' && !Number.isInteger(v) ? v.toFixed(1) : String(v));

export function eventLine(seconds, name, data) {
  const fields = Object.entries(data || {}).map(([k, v]) => `${k}=${valueText(v)}`).join(' ');
  return `[${clockText(seconds)}] ${name}${fields ? ' ' + fields : ''}`;
}

export class EventRing {
  constructor(limit) { this.items = []; this.limit = Math.max(1, limit | 0); }
  push(line) { this.items.push(line); this.trim(); }
  setLimit(limit) { this.limit = Math.max(1, limit | 0); this.trim(); }
  trim() { if (this.items.length > this.limit) this.items.splice(0, this.items.length - this.limit); }
  lines() { return this.items.slice(); }   // oldest first
  clear() { this.items = []; }
}

// `text` is a decimal number, 0x hex, or a symbol name that `lookup` knows.
export function parseAddress(text, lookup) {
  const t = String(text ?? '').trim();
  if (t === '') return { empty: true };
  let address;
  if (/^0x[0-9a-f]+$/i.test(t)) address = parseInt(t, 16);
  else if (/^[0-9]+$/.test(t)) address = parseInt(t, 10);
  else if (/^[A-Za-z_][A-Za-z0-9_.$@<>:]*$/.test(t)) {
    const found = lookup(t);
    if (found == null) return { error: `unknown symbol '${t}'` };
    address = found;
  } else return { error: `'${t}' is not an address (decimal or 0x...) or a symbol name` };
  if (!Number.isSafeInteger(address) || address > 0xFFFFFFFF) return { error: 'the address does not fit in 32 bits' };
  return { address };
}

export const WATCH_TYPES = ['u8', 'u16', 'u32', 's32', 'f32'];
const HEX_DIGITS = { u8: 2, u16: 4, u32: 8, s32: 8 };

export function readWatch(mem, address, type) {
  try {
    const value = mem[`read_${type}`](address);
    if (type === 'f32') return { text: Number.isFinite(value) ? String(value) : 'not a number' };
    return { text: `${value} (0x${(value >>> 0).toString(16).toUpperCase().padStart(HEX_DIGITS[type], '0')})` };
  } catch (e) {
    return { error: String(e && e.message || e) };
  }
}

// One line of JSON (null where a part does not exist) for bug reports and notes.
export function snapshot(state) {
  return JSON.stringify({
    players: state.players,
    game: state.game,
    camera: state.camera,
    enemies: state.enemies.map((e) => ({ id: e.id, hp: e.hp, x: e.x, y: e.y, z: e.z })),
  });
}
