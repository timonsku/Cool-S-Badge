# Turns the first run in battery.log into a 101-entry curve_mv table, percent = share of
# the run time left (constant load, so charge is proportional to time).
import sys
rows = []
for line in open(sys.argv[1]):
    if line.startswith('#'):
        if rows: break  # only the first boot: later ones are brown-out restarts
        continue
    t, mv, unloaded = map(int, line.split(','))
    if not unloaded: rows.append((t, mv))
t0, t1 = rows[0][0], rows[-1][0]
ts = [r[0] for r in rows]
# moving average over +-4 readings (5 mV steps), edges kept as measured
k = 4
mvs = [sum(r[1] for r in rows[max(0, i - k):i + k + 1]) / len(rows[max(0, i - k):i + k + 1]) for i in range(len(rows))]
def at(t):
    for i in range(len(ts) - 1):
        if ts[i] <= t <= ts[i + 1]:
            f = (t - ts[i]) / (ts[i + 1] - ts[i])
            return mvs[i] + f * (mvs[i + 1] - mvs[i])
    return mvs[-1]
table = [round(at(t0 + (1 - p / 100) * (t1 - t0))) for p in range(101)]
table[0], table[100] = rows[-1][1], rows[0][1]
for p in range(1, 101):  # strictly rising, as percent_from_mv expects
    table[p] = max(table[p], table[p - 1] + 1)
print(f'run {t0}..{t1} s = {(t1 - t0) / 3600:.2f} h, {len(rows)} readings, {rows[0][1]} -> {rows[-1][1]} mV')
for row in range(0, 101, 10):
    print('\t' + ', '.join(str(v) for v in table[row:row + 10]) + ',' + ' ' * (2 if row < 100 else 52) + f'/* {row} % */')
