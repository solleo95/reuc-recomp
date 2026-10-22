// Test-only: an interval of one game second and a timeout of two; the harness counts the lines (they follow game steps, so a pause stops them).
let ticks = 0;
setInterval((tag) => { ticks += 1; log.write(`timer tick=${ticks} ${tag}`); }, 1000, 'every');
setTimeout(() => log.write('timer once'), 2000);
