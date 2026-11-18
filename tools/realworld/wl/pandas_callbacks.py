"""pandas with Python callbacks: read_csv, apply(axis=1), map, groupby().apply, merge, pivot."""
import hashlib, io
import numpy as np
import pandas as pd

rng = np.random.default_rng(0)
n = 100_000
df = pd.DataFrame({'a': rng.integers(0, 100, n), 'b': rng.random(n), 'c': rng.choice(['x', 'y', 'z', 'w'], n),
                   'd': pd.date_range('2024-01-01', periods=n, freq='min')})
csv = df.to_csv(index=False)
for k in range(3):
    d = pd.read_csv(io.StringIO(csv), parse_dates=['d'])
    d['e'] = d.apply(lambda r: r['a'] * r['b'] if r['c'] == 'x' else r['b'], axis=1)
    d['f'] = d['c'].map(lambda s: s.upper() + '!')
    g = d.groupby('c').apply(lambda g: (g['b'] * g['a']).sum() / len(g), include_groups=False)
    m = d.merge(d.groupby('c', as_index=False)['b'].mean(), on='c', suffixes=('', '_m'))
    p = m.pivot_table(index=m['d'].dt.hour, columns='c', values='e', aggfunc='mean')
out = g.round(9).to_csv() + p.round(9).to_csv() + d['f'].value_counts().sort_index().to_csv()
print('frames', hashlib.sha256(out.encode()).hexdigest())
