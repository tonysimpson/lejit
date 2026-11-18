"""Megamorphic call and attribute sites: one drawing loop over twenty-four shape classes."""
import math


class Shape:
    sides = 0

    def __init__(self, a, b):
        self.a = a
        self.b = b

    def describe(self):
        return self.sides * 1000 + int(self.area() * 10) % 1000

    def scaled(self, k):
        return type(self)(self.a * k, self.b * k)


SPECS = [
    ('Square', 4, lambda s: s.a * s.a, lambda s: 4 * s.a),
    ('Rectangle', 4, lambda s: s.a * s.b, lambda s: 2 * (s.a + s.b)),
    ('Circle', 0, lambda s: math.pi * s.a * s.a, lambda s: 2 * math.pi * s.a),
    ('Ellipse', 0, lambda s: math.pi * s.a * s.b, lambda s: math.pi * (s.a + s.b)),
    ('Triangle', 3, lambda s: s.a * s.b / 2, lambda s: s.a + s.b + math.hypot(s.a, s.b)),
    ('Equilateral', 3, lambda s: math.sqrt(3) / 4 * s.a ** 2, lambda s: 3 * s.a),
    ('Pentagon', 5, lambda s: 1.72048 * s.a * s.a, lambda s: 5 * s.a),
    ('Hexagon', 6, lambda s: 2.59808 * s.a * s.a, lambda s: 6 * s.a),
    ('Heptagon', 7, lambda s: 3.63391 * s.a * s.a, lambda s: 7 * s.a),
    ('Octagon', 8, lambda s: 4.82843 * s.a * s.a, lambda s: 8 * s.a),
    ('Nonagon', 9, lambda s: 6.18182 * s.a * s.a, lambda s: 9 * s.a),
    ('Decagon', 10, lambda s: 7.69421 * s.a * s.a, lambda s: 10 * s.a),
    ('Rhombus', 4, lambda s: s.a * s.b / 2, lambda s: 2 * math.hypot(s.a, s.b)),
    ('Kite', 4, lambda s: s.a * s.b / 2, lambda s: s.a + s.b + abs(s.a - s.b)),
    ('Trapezium', 4, lambda s: (s.a + s.b) * s.b / 2, lambda s: s.a + s.b * 3),
    ('Parallelogram', 4, lambda s: s.a * s.b * 0.8, lambda s: 2 * (s.a + s.b)),
    ('Annulus', 0, lambda s: math.pi * abs(s.a * s.a - s.b * s.b), lambda s: 2 * math.pi * (s.a + s.b)),
    ('Sector', 1, lambda s: s.a * s.a * s.b / 20, lambda s: 2 * s.a + s.a * s.b / 10),
    ('Segment', 1, lambda s: s.a * s.a * (s.b / 10 - math.sin(s.b / 10)) / 2, lambda s: s.a * s.b / 10),
    ('Star', 10, lambda s: 1.12257 * s.a * s.a, lambda s: 10 * s.b),
    ('Cross', 12, lambda s: 5 * s.a * s.a, lambda s: 12 * s.a),
    ('Arrow', 7, lambda s: s.a * s.b * 0.7, lambda s: 2 * s.a + 3 * s.b),
    ('Crescent', 0, lambda s: math.pi * s.a * s.a * 0.3, lambda s: math.pi * (s.a + s.b) * 0.9),
    ('Lune', 0, lambda s: math.pi * s.b * s.b * 0.25, lambda s: math.pi * s.b * 1.5),
]

CLASSES = [type(name, (Shape,), {'sides': sides, 'area': area, 'perimeter': perimeter})
           for name, sides, area, perimeter in SPECS]


def scene(count):
    return [CLASSES[(i * 7) % len(CLASSES)](1 + i % 5, 2 + i % 3) for i in range(count)]


def draw(shapes):
    area = perimeter = 0.0
    corners = codes = 0
    for s in shapes:
        area += s.area()
        perimeter += s.perimeter()
        corners += s.sides
        codes += s.describe()
        if isinstance(s, (CLASSES[0], CLASSES[5])):
            corners += 1
    return area, perimeter, corners, codes


def main(n):
    shapes = scene(240)
    total = [0.0, 0.0, 0, 0]
    for i in range(n):
        r = draw(shapes)
        for k in range(4):
            total[k] += r[k]
        shapes[i % len(shapes)] = shapes[i % len(shapes)].scaled(1.0 + (i % 3) / 10)
    return round(total[0], 3), round(total[1], 3), total[2], total[3]


N = 300

if __name__ == '__main__':
    print(main(N))
