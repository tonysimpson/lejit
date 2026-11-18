"""Class bodies doing real work, which are never compiled: lookup tables and methods built inside class statements."""


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


def make_units():
    class Units:
        scale = {}
        base = 1
        for name in ('mm', 'cm', 'dm', 'm', 'dam', 'hm', 'km'):
            scale[name] = base
            base *= 10
        del name, base
    return Units


def main(n):
    total = 0
    payload = bytes(range(64))
    for i in range(n):
        Codec = make_codec(0xedb88320 if i % 2 else 0x82f63b78)
        Units = make_units()
        total += Codec().checksum(payload) % 100000
        total += Codec.squares[i % 64] + len(Codec.names) + Units.scale['km']
    return total


N = 300

if __name__ == '__main__':
    print(main(N))
