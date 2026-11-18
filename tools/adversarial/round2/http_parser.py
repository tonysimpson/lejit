"""Byte-at-a-time HTTP/1.1 parser where each state is a function returning the next state, fed pipelined requests as bytes in one variant and as bytearray and memoryview chunks with chunked bodies, folded headers and malformed requests in the other: one megamorphic indirect call site in the hot loop and polymorphic buffers (Hoelzle, Chambers and Ungar, ECOOP 1991; Gal et al., PLDI 2009, on branchy loops)."""
import zlib

N = 180

CR, LF, SP, COLON, TAB = 13, 10, 32, 58, 9


class BadRequest(Exception):
    pass


class Parser:
    def __init__(self):
        self.done = []
        self.errors = []
        self.reset()

    def reset(self):
        self.state = st_method
        self.buf = bytearray()
        self.method = self.target = self.version = None
        self.headers = []
        self.hname = None
        self.body = bytearray()
        self.remaining = 0
        self.chunked = False

    def feed(self, data):
        state = self.state
        i = 0
        n = len(data)
        while i < n:
            b = data[i]
            try:
                state = state(self, b)
            except BadRequest as e:
                self.errors.append(str(e))
                self.reset()
                state = st_recover
            i += 1
            if state is st_body_bulk:
                take = min(self.remaining, n - i)
                self.body += data[i:i + take]
                self.remaining -= take
                i += take
                state = st_body_bulk_end(self) if self.remaining == 0 else st_body_bulk
        self.state = state

    def finish_headers(self):
        hs = dict((k.lower(), v) for k, v in self.headers)
        if hs.get('transfer-encoding', '').lower() == 'chunked':
            self.chunked = True
            return st_chunk_size
        length = hs.get('content-length')
        if length is not None:
            if not length.isdigit():
                raise BadRequest('bad content-length %r' % length)
            self.remaining = int(length)
            if self.remaining:
                return st_body_bulk
        return self.complete()

    def complete(self):
        self.done.append((self.method, self.target, self.version, tuple(self.headers),
                          bytes(self.body)))
        self.reset()
        return st_method


def st_method(p, b):
    if b == SP:
        p.method = p.buf.decode('ascii')
        if not p.method.isupper():
            raise BadRequest('bad method %r' % p.method)
        p.buf = bytearray()
        return st_target
    if b in (CR, LF) and not p.buf:
        return st_method
    if not (65 <= b <= 90):
        raise BadRequest('bad method byte %d' % b)
    p.buf.append(b)
    return st_method


def st_target(p, b):
    if b == SP:
        p.target = p.buf.decode('latin-1')
        p.buf = bytearray()
        return st_version
    if b < 33:
        raise BadRequest('bad target byte %d' % b)
    p.buf.append(b)
    return st_target


def st_version(p, b):
    if b == CR:
        return st_version
    if b == LF:
        p.version = p.buf.decode('ascii')
        if not p.version.startswith('HTTP/1.'):
            raise BadRequest('bad version %r' % p.version)
        p.buf = bytearray()
        return st_header_start
    p.buf.append(b)
    return st_version


def st_header_start(p, b):
    if b == CR:
        return st_header_start
    if b == LF:
        return p.finish_headers()
    if b in (SP, TAB):
        if not p.headers:
            raise BadRequest('fold before header')
        return st_fold
    p.buf.append(b)
    return st_header_name


def st_header_name(p, b):
    if b == COLON:
        p.hname = p.buf.decode('ascii')
        p.buf = bytearray()
        return st_header_ws
    if b in (CR, LF, SP):
        raise BadRequest('bad header name')
    p.buf.append(b)
    return st_header_name


def st_header_ws(p, b):
    if b in (SP, TAB):
        return st_header_ws
    return st_header_value(p, b)


def st_header_value(p, b):
    if b == CR:
        return st_header_value
    if b == LF:
        p.headers.append((p.hname, p.buf.decode('latin-1').rstrip()))
        p.buf = bytearray()
        return st_header_start
    p.buf.append(b)
    return st_header_value


def st_fold(p, b):
    if b in (SP, TAB):
        return st_fold
    name, value = p.headers.pop()
    p.hname = name
    p.buf = bytearray(value.encode('latin-1') + b' ')
    return st_header_value(p, b)


def st_body_bulk(p, b):
    p.body.append(b)
    p.remaining -= 1
    return st_body_bulk_end(p) if p.remaining == 0 else st_body_bulk


def st_body_bulk_end(p):
    return p.complete()


def st_chunk_size(p, b):
    if b == CR:
        return st_chunk_size
    if b == LF:
        text = p.buf.decode('ascii').split(';')[0].strip()
        p.buf = bytearray()
        try:
            p.remaining = int(text, 16)
        except ValueError:
            raise BadRequest('bad chunk size %r' % text) from None
        return st_chunk_data if p.remaining else st_trailer
    p.buf.append(b)
    return st_chunk_size


def st_chunk_data(p, b):
    p.body.append(b)
    p.remaining -= 1
    return st_chunk_data if p.remaining else st_chunk_end


def st_chunk_end(p, b):
    if b == CR:
        return st_chunk_end
    if b == LF:
        return st_chunk_size
    raise BadRequest('missing chunk terminator')


def st_trailer(p, b):
    if b == CR:
        return st_trailer
    if b == LF:
        return p.complete()
    return st_trailer


def st_recover(p, b):
    if b == LF:
        p.buf = bytearray()
        return st_recover_blank
    return st_recover


def st_recover_blank(p, b):
    if b == CR:
        return st_recover_blank
    if b == LF:
        return st_method
    return st_recover


def make_stream(k, variant):
    reqs = []
    for j in range(25):
        path = '/api/v%d/items/%d?q=%d' % (j % 3, k * 25 + j, j * 7)
        if variant == 1 and j % 4 == 1:
            body = ''.join('%x\r\n%s\r\n' % (len(s), s) for s in
                           ('chunk%d' % j, 'x' * (j % 9 + 1), 'end'))
            reqs.append('POST %s HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n'
                        'X-Long: part one\r\n  part two\r\n\r\n%s0\r\n\r\n' % (path, body))
        elif variant == 1 and j % 7 == 3:
            reqs.append('get %s HTTP/1.1\r\nHost: h\r\n\r\n' % path)
        elif variant == 1 and j % 11 == 5:
            reqs.append('PUT %s HTTP/1.1\r\nContent-Length: zz\r\n\r\n' % path)
        elif j % 3 == 2:
            body = 'payload=%d&name=item%d' % (j * k, j)
            reqs.append('POST %s HTTP/1.1\r\nHost: example\r\nContent-Length: %d\r\n'
                        'Content-Type: application/x-www-form-urlencoded\r\n\r\n%s'
                        % (path, len(body), body))
        else:
            reqs.append('GET %s HTTP/1.1\r\nHost: example\r\nAccept: */*\r\n'
                        'User-Agent: bench/%d\r\n\r\n' % (path, j))
    return ''.join(reqs).encode('latin-1')


def chunks(data, k, variant):
    if variant == 0:
        return [data]
    out = []
    i = 0
    step = 37 + k % 23
    toggle = 0
    while i < len(data):
        piece = data[i:i + step]
        toggle += 1
        out.append(bytearray(piece) if toggle % 2 else memoryview(piece))
        i += step
        step = step * 3 % 97 + 5
    return out


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    results = []
    for k in range(n * 4):
        p = Parser()
        for c in chunks(make_stream(k, variant), k, variant):
            p.feed(c)
        results.append(([(m, t, len(h), h[-1], b[:24]) for m, t, v, h, b in p.done],
                         p.errors))
    return digest(results, 2)
