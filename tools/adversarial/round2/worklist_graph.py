"""Graph passes that mutate the containers they are iterating: appending to the list being walked, removing from it (skipping elements), dicts and sets changing size mid-loop (RuntimeError), dict delete-and-reinsert at equal size, and deques rotated while indexed: iterator state and lengths a compiler may cache (LuaJIT NYI list: next and pairs; Python Language Reference, the for statement, on modifying a sequence while looping over it)."""
from collections import deque
import zlib

N = 600


def build(i, size, variant):
    graph = {}
    for a in range(size):
        key = a if variant == 0 else ('n', a)
        succ = [(a * (3 + 2 * variant) + 1) % size, (a * 7 + i) % size, (a + 1 + variant) % size]
        graph[key] = [s if variant == 0 else ('n', s) for s in succ if s != a]
    return graph


def bfs_growing(graph, start):
    order = [start]
    seen = {start}
    for node in order:
        for s in graph[node]:
            if s not in seen:
                seen.add(s)
                order.append(s)
    return order


def prune_while_iterating(nodes, graph):
    lst = list(nodes)
    visited = []
    for x in lst:
        visited.append(x)
        if len(graph[x]) > 1 and graph[x][0] in lst:
            lst.remove(graph[x][0])
    return len(visited), len(lst), visited[:12]


def relabel_dict(graph):
    labels = {k: 0 for k in graph}
    first = next(iter(graph))
    errors = 0
    passes = 0
    while True:
        passes += 1
        try:
            for k in labels:
                labels[k] += 1
                if labels[k] == passes and k == first and passes < 3:
                    labels[('extra', passes)] = 0
        except RuntimeError:
            errors += 1
            if errors > 3:
                break
            continue
        break
    return errors, passes, len(labels), sum(v for v in labels.values())


def swap_keys(graph, limit):
    d = {k: len(v) for k, v in list(graph.items())[:limit]}
    seen = []
    for k in d:
        seen.append(k)
        if len(seen) <= 3:
            v = d.pop(k)
            d[('moved', k)] = v
        if len(seen) > 4 * limit:
            break
    return len(seen), list(d)[:6], seen[-3:]


def set_changes(graph):
    s = set(range(0, 40, 3))
    raised = None
    try:
        for x in s:
            if x % 2:
                s.add(x + 100)
    except RuntimeError as e:
        raised = str(e)
    return raised, len(s)


def deque_rotate(graph, rounds):
    dq = deque(sorted(graph, key=str)[:20])
    picked = []
    for r in range(rounds):
        dq.rotate(r % 5 - 2)
        picked.append(dq[r % len(dq)])
        if r % 4 == 0:
            dq.append(dq.popleft())
    try:
        for x in dq:
            if x == dq[3]:
                dq.append(x)
    except RuntimeError as e:
        picked.append(str(e))
    return picked


def topo_with_worklist(graph):
    indeg = {k: 0 for k in graph}
    for k, vs in graph.items():
        for v in vs:
            indeg[v] += 1
    work = [k for k, d in indeg.items() if d <= 2]
    out = []
    for k in work:
        out.append(k)
        for v in graph[k]:
            indeg[v] -= 1
            if indeg[v] == 2:
                work.append(v)
        if len(work) > 3 * len(graph):
            break
    return len(out), out[:10]


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    results = []
    for i in range(n):
        size = 200 + 13 * (i % 20)
        g = build(i, size, variant)
        start = next(iter(g))
        order = bfs_growing(g, start)
        results.append((len(order), order[-5:], prune_while_iterating(order, g),
                        relabel_dict(g), swap_keys(g, 8 + i % 5), set_changes(g),
                        deque_rotate(g, 30 + i % 20), topo_with_worklist(g)))
    return digest(results, 3)
