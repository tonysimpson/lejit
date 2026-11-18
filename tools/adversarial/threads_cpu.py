"""CPU-bound threads: four workers contending for the GIL, each running a loop that compiles well on its own. Variant 1 runs three workers with other seeds."""
import threading


def crunch(seed, steps):
    x = seed
    total = 0
    for i in range(steps):
        x = (x * 1103515245 + 12345) & 0x7fffffff
        if x % 3 == 0:
            total += x % 1000
        else:
            total -= x % 7
    return total


def worker(results, index, seed, chunks, steps):
    acc = 0
    for c in range(chunks):
        acc += crunch(seed + c, steps)
    results[index] = acc


def main(n, variant=0):
    count, base, chunks = [(4, 1000, 20), (3, 7919, 27)][variant]
    results = [None] * count
    threads = [threading.Thread(target=worker, args=(results, k, base * k + 1, chunks, n))
               for k in range(count)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return results


N = 5000

if __name__ == '__main__':
    print(main(N))
