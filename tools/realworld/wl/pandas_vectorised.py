"""Control: vectorised pandas (groupby-agg, np.where, merge, sort) on 2M rows; almost no bytecode."""
import hashlib, io
import numpy as np
import pandas as pd

rng = np.random.default_rng(0)
n = 2_000_000
df = pd.DataFrame({'a': rng.integers(0, 1000, n), 'b': rng.random(n), 'c': rng.choice(['x', 'y', 'z', 'w'], n)})
csv = df.head(300_000).to_csv(index=False)
for k in range(3):
    d = pd.read_csv(io.StringIO(csv))
    g = df.groupby(['c', 'a']).agg(s=('b', 'sum'), m=('b', 'mean'), n=('b', 'size'))
    df['e'] = np.where(df['c'] == 'x', df['a'] * df['b'], df['b'])
    m = df.merge(g.reset_index()[['c', 'a', 'm']], on=['c', 'a'])
    s = df.sort_values(['c', 'b'], kind='stable')
out = g.round(6).to_csv() + '%d %d %.6f' % (len(m), len(d), s['e'].iloc[:1000].sum())
print('frames', hashlib.sha256(out.encode()).hexdigest())
