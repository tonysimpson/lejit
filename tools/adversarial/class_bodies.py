"""Class bodies doing real work, which are never compiled: lookup tables and methods built inside class statements. Variant 1 uses other polynomials and units, and a bytearray payload."""


def make_codec(poly):
    class Codec:
        table = []
        for byte in range(256):
            crc = byte
            for _ in range(8):
                crc = (crc >> 1) ^ poly if crc & 1 else crc >> 1
            table.append(crc)
        squares = {k: k * k for k in range(64)}
        names = []
        for k in range(32):
            names.append('field_%02d' % k)
        del byte, crc, k

        def checksum(self, data):
            crc = 0xffffffff
            for b in data:
                crc = self.table[(crc ^ b) & 0xff] ^ (crc >> 8)
            return crc ^ 0xffffffff
    return Codec


def make_units(steps):
    class Units:
        scale = {}
        base = 1
        for name, factor in steps:
            scale[name] = base
            base *= factor
        del name, factor, base
    return Units


VARIANTS = [
    (bytes(range(64)), (0x82f63b78, 0xedb88320),
     [(name, 10) for name in ('mm', 'cm', 'dm', 'm', 'dam', 'hm', 'km')], 'km'),
    (bytearray(range(200, 136, -1)), (0xeb31d82e, 0xd5828281, 0xc8df352f),
     [('in', 12), ('ft', 3), ('yd', 22), ('ch', 10), ('fur', 8), ('mi', 1)], 'mi'),
]


def main(n, variant=0):
    payload, polys, steps, top = VARIANTS[variant]
    total = 0
    for i in range(n):
        Codec = make_codec(polys[i % len(polys)])
        Units = make_units(steps)
        total += Codec().checksum(payload) % 100000
        total += Codec.squares[i % 64] + len(Codec.names) + Units.scale[top]
    return total


N = 300

if __name__ == '__main__':
    print(main(N))
