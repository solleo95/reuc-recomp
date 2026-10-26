"""Analyses a UC_AUDIO_DUMP file: python -m uctest.audio_dump <dump> [<dump> ...].

The game writes every block it hands to the audio device as {steady ns u64, bytes u32, queued bytes u32, pcm int16 stereo 32 kHz}. This prints
the write cadence (gaps are what run the device dry), the queue the device had left at each write (the cushion against gaps), and the
discontinuities inside the signal (what a click or a game-side bug looks like).
"""
import array
import struct
import sys

RATE = 32000
FRAME_BYTES = 4


def read_dump(path):
    """Returns [(seconds since the first write, queued bytes before the write, pcm array('h'))]."""
    data = open(path, "rb").read()
    at, records = 0, []
    while at + 16 <= len(data):
        ns, size, queued = struct.unpack_from("<QII", data, at)
        at += 16
        pcm = array.array("h")
        pcm.frombytes(data[at:at + size])
        at += size
        records.append((ns / 1e9, queued, pcm))
    start = records[0][0] if records else 0.0
    return [(t - start, q, p) for t, q, p in records]


def percentile(values, share):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(len(ordered) * share))]


def report(path, windows=((0, 2), (2, 7), (7, 15), (15, 3600))):
    records = read_dump(path)
    audio = sum(len(p) for _, _, p in records) / 2 / RATE
    print(f"{path}: {len(records)} blocks, span {records[-1][0]:.1f} s, audio {audio:.1f} s")
    gaps = [(round((records[i][0] - records[i - 1][0]) * 1000), round(records[i][0], 1)) for i in range(1, len(records))
            if records[i][0] - records[i - 1][0] > 0.03]
    print(f"  write gaps over 30 ms: {len(gaps)} (ms, at s): {gaps[:20]}")
    for low, high in windows:
        queue = [q / FRAME_BYTES / RATE * 1000 for t, q, _ in records if low <= t < high]
        if queue:
            print(f"  queue ms at {low:>2}-{high:<4} s: min {min(queue):.0f}  p5 {percentile(queue, .05):.0f}  median {percentile(queue, .5):.0f}"
                  f"  max {max(queue):.0f}  empty writes {sum(1 for q in queue if q == 0)}")
    samples = array.array("h")
    for _, _, pcm in records:
        samples.extend(pcm)
    left = samples[0::2]
    for second in range(len(left) // RATE):
        part = left[second * RATE:(second + 1) * RATE]
        steps = [abs(part[i + 1] - part[i]) for i in range(len(part) - 1)]
        print(f"  second {second:>3}: peak {max(abs(x) for x in part):>6}  largest step {max(steps):>6}  steps over 8000: {sum(1 for s in steps if s > 8000)}")
        if second >= 59:
            break


if __name__ == "__main__":
    for dump in sys.argv[1:]:
        report(dump)
