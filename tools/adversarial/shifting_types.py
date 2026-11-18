"""Merge points whose locals change form as the loop runs: a spreadsheet column fold where accumulators turn from int to float to big int."""


def cells(n):
    out = []
    seed = 99
    for i in range(n):
        seed = (seed * 48271) % 2147483647
        k = seed % 8
        if k == 0:
            out.append(None)
        elif k == 1:
            out.append(seed % 2 == 0)
        elif k == 2:
            out.append(seed % 1000)
        elif k == 3:
            out.append(seed % 1000 / 8)
        elif k == 4:
            out.append('cell%d' % (seed % 50))
        elif k == 5:
            out.append(seed * 1000003)
        elif k == 6:
            out.append(-(seed % 300))
        else:
            out.append((seed % 7, seed % 11))
    return out


def fold(column):
    total = 0
    product = 1
    last = None
    longest = ''
    count = 0
    for c in column:
        if c is None:
            last = None
            continue
        if isinstance(c, bool):
            total += c
        elif isinstance(c, int):
            total += c
            product = (product * (c or 1)) % 10 ** 40
        elif isinstance(c, float):
            total += c
            last = c
        elif isinstance(c, str):
            if len(c) > len(longest):
                longest = c
            last = c
        else:
            total += c[0] - c[1]
            last = c
        count += 1
    return total, product, last, longest, count


def running(column):
    best = None
    for c in column:
        if isinstance(c, (int, float)) and not isinstance(c, bool):
            if best is None or c > best:
                best = c
            elif c < 0:
                best = best + c / 2
    return best


def main(n):
    column = cells(n)
    results = []
    for k in range(4):
        part = column[k::4]
        total, product, last, longest, count = fold(part)
        results.append((round(total, 3), product % 1000003, repr(last), longest, count,
                        round(running(part), 3)))
    return results


N = 200000

if __name__ == '__main__':
    print(main(N))
