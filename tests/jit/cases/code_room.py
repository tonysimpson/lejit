# Code whose single pieces are long: each must fit in the room left past the
# code buffer's limit, wherever it starts (psyco.h:GUARANTEED_MINIMUM), or
# make room as it goes. A checked build asserts it for every piece; a
# release build stopped with "psyco: code buffer overflowing" when one
# happened to start just before the limit.
import functools


def seen(*args, **kwargs):
    return len(args), sorted(kwargs)


def many(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14,
         a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27,
         a28, a29, a30, a31, a32, a33, a34, a35, a36, a37, a38, a39, a40,
         a41, a42, a43, a44, a45, a46, a47, a48, a49, a50, a51, a52, a53,
         a54, a55, a56, a57, a58, a59, a60, a61, a62, a63, a64, a65, a66,
         a67, a68, a69, a70, a71, a72, a73, a74, a75, a76, a77, a78, a79,
         a80, a81, a82, a83, a84, a85, a86, a87, a88, a89, a90, a91, a92,
         a93, a94, a95, a96, a97, a98, a99, a100, a101, a102, a103, a104,
         a105, a106, a107, a108, a109, a110, a111, a112, a113, a114, a115,
         a116, a117, a118, a119, a120, a121, a122, a123, a124, a125, a126,
         a127, a128, a129, a130, a131, a132, a133, a134, a135, a136, a137,
         a138, a139, a140, a141, a142, a143, a144, a145, a146, a147, a148,
         a149, a150, a151, a152, a153, a154, a155, a156, a157, a158, a159):
    return a0 + a80 + a159


# a residual call of many values, each stored into the array it passes
def wide_residual(n):
    fs = [seen, functools.partial(seen, 0), seen, max, min, seen,
          functools.partial(seen, 1), seen]
    out = []
    for i in range(n):
        f = fs[i % len(fs)]
        a, b, c = i, i * 3, -i
        out.append(repr(f(a, b, c, a + 1, b + 1, c + 1, a + 2, b + 2, c + 2,
                          a + 3, b + 3, c + 3, a + 4, b + 4, c + 4, a + 5,
                          b + 5, c + 5, a + 6, b + 6, c + 6, a + 7, b + 7,
                          c + 7, a + 8, b + 8, c + 8, a + 9, b + 9)))
    return out


# a call of a Python function with many arguments
def wide_direct(n):
    out = []
    for i in range(n):
        a, b = i, i + 1
        out.append(many(a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b,
                        a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b,
                        a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b,
                        a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b,
                        a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b,
                        a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b,
                        a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b,
                        a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b,
                        a, b, a, b, a, b, a, b, a, b, a, b, a, b, a, b))
    return out


# releasing an object while floats are live in registers: the release
# saves and restores them around the call that may free it
def release_with_floats(n):
    out = []
    for i in range(n):
        x = i + 0.5
        p, q, r, s, t, u, v = (x * 1.5, x * 2.5, x * 3.5, x * 4.5, x * 5.5,
                               x * 6.5, x * 7.5)
        k = [i, str(i)]
        k = None
        out.append(p + q + r + s + t + u + v)
        out.append(k)
    return out


CASES = [
    ('wide_residual', [(1,), (16,)], {}),
    ('wide_direct', [(1,), (4,)], {}),
    ('release_with_floats', [(1,), (6,)], {}),
]
