"""Delegating generators: yield from through recursive tree walks, and coroutine-style generators driven by send(). Variant 1 has another seed and float keys."""


class Node:
    __slots__ = ('key', 'left', 'right')

    def __init__(self, key):
        self.key = key
        self.left = self.right = None


def insert(root, key):
    if root is None:
        return Node(key)
    node = root
    while True:
        if key < node.key:
            if node.left is None:
                node.left = Node(key)
                return root
            node = node.left
        else:
            if node.right is None:
                node.right = Node(key)
                return root
            node = node.right


def walk(node):
    if node is not None:
        yield from walk(node.left)
        yield node.key
        yield from walk(node.right)


def leaves(node, depth=0):
    if node.left is None and node.right is None:
        yield depth
    for child in (node.left, node.right):
        if child is not None:
            yield from leaves(child, depth + 1)


def averager():
    total = count = 0
    avg = None
    while True:
        x = yield avg
        if x is None:
            return count, total
        total += x
        count += 1
        avg = total / count


def grouper(results, key):
    while True:
        results[key] = yield from averager()


def main(n, variant=0):
    root = None
    seed = 7 if variant == 0 else 2024
    for _ in range(n):
        seed = (seed * 69069 + 1) & 0xffffffff
        root = insert(root, seed % 100003 if variant == 0 else seed % 99991 / 8)
    ordered = list(walk(root))
    depths = list(leaves(root))
    results = {}
    for g, key in enumerate('abcd'):
        group = grouper(results, key)
        next(group)
        for v in ordered[g::4]:
            group.send(v)
        group.send(None)
    return ordered[:3], len(ordered), max(depths), sum(depths), sorted(results.items())


N = 20000

if __name__ == '__main__':
    print(main(N))
