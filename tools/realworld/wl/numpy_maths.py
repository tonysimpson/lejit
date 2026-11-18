"""Control: numpy maths (matmul, FFT, SVD, sort); almost no bytecode."""
import numpy as np

rng = np.random.default_rng(0)
a = rng.random((1200, 1200))
for i in range(4):
    b = a @ a.T
    f = np.fft.fft2(a)
    sv = np.linalg.svd(a[:400, :400], compute_uv=False)
    c = np.sort(rng.random(5_000_000))
print('matmul %.6e' % b.trace())
print('fft %.6e' % abs(f[1:50, 1:50]).sum())
print('svd %.6e %.6e' % (sv[0], sv[-1]))
print('sort %.9f %.9f' % (c[0], c[-1]))
