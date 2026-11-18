"""Special methods defined in Python (slot wrappers, never inlined): Vector and Matrix classes with arithmetic, indexing, equality, hashing and iteration."""


class Vector:
    __slots__ = ('x', 'y', 'z')

    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def __add__(self, o):
        return Vector(self.x + o.x, self.y + o.y, self.z + o.z)

    def __sub__(self, o):
        return Vector(self.x - o.x, self.y - o.y, self.z - o.z)

    def __mul__(self, k):
        return Vector(self.x * k, self.y * k, self.z * k)

    __rmul__ = __mul__

    def __neg__(self):
        return Vector(-self.x, -self.y, -self.z)

    def __matmul__(self, o):
        return self.x * o.x + self.y * o.y + self.z * o.z

    def __abs__(self):
        return (self @ self) ** 0.5

    def __getitem__(self, i):
        return (self.x, self.y, self.z)[i]

    def __iter__(self):
        yield self.x
        yield self.y
        yield self.z

    def __eq__(self, o):
        return isinstance(o, Vector) and self.x == o.x and self.y == o.y and self.z == o.z

    def __hash__(self):
        return hash((self.x, self.y, self.z))

    def __lt__(self, o):
        return abs(self) < abs(o)

    def __bool__(self):
        return bool(self.x or self.y or self.z)


class Matrix:
    def __init__(self, rows):
        self.rows = rows

    def __getitem__(self, ij):
        i, j = ij
        return self.rows[i][j]

    def __len__(self):
        return len(self.rows)

    def __mul__(self, v):
        return Vector(*(Vector(*row) @ v for row in self.rows))

    def __contains__(self, value):
        return any(value in row for row in self.rows)


def main(n):
    rot = Matrix([[0.0, -1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]])
    pts = [Vector(i % 7 - 3, i % 5 - 2, i % 3 - 1) for i in range(50)]
    seen = set()
    acc = 0.0
    for step in range(n):
        centre = Vector(0, 0, 0)
        for p in pts:
            centre = centre + p
        centre = centre * (1.0 / len(pts))
        moved = [rot * (p - centre) + centre for p in pts]
        acc += sum(abs(m) for m in moved if m)
        acc += sum(x for x in moved[step % 50]) + moved[0][1] + rot[1, 0]
        acc += len(rot) + (1.0 in rot)
        seen.update(Vector(round(m.x), round(m.y), round(m.z)) for m in moved)
        pts = sorted(moved)[:50]
    return round(acc, 4), len(seen), Vector(1, 2, 3) == Vector(1, 2, 3), -pts[0] @ pts[-1] != 0


N = 400

if __name__ == '__main__':
    print(main(N))
