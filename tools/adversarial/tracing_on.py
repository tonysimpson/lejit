"""Tracing on: a coverage-style sys.settrace line tracer around ordinary application code. Variant 1 processes other documents."""
import sys


class Coverage:
    def __init__(self):
        self.lines = {}
        self.calls = {}

    def tracer(self, frame, event, arg):
        if event == 'call':
            name = frame.f_code.co_name
            self.calls[name] = self.calls.get(name, 0) + 1
            return self.local
        return None

    def local(self, frame, event, arg):
        if event == 'line':
            key = (frame.f_code.co_name, frame.f_lineno)
            self.lines[key] = self.lines.get(key, 0) + 1
        return self.local


def tokenize(text):
    out = []
    word = ''
    for ch in text:
        if ch.isalnum():
            word += ch
        else:
            if word:
                out.append(word)
                word = ''
    if word:
        out.append(word)
    return out


def score(words):
    total = 0
    for w in words:
        total += len(w) * (2 if w[0].isupper() else 1)
    return total


def process(docs):
    return sum(score(tokenize(d)) for d in docs)


DOCS = [['The Quick brown fox, number %d, jumps over the Lazy dog.' % i for i in range(20)],
        ['%d: Pack my Box with five dozen liquor Jugs; sphinx of black quartz!' % (i * 7)
         for i in range(17)]]


def main(n, variant=0):
    cov = Coverage()
    sys.settrace(cov.tracer)
    try:
        total = 0
        for i in range(n):
            total += process(DOCS[variant])
    finally:
        sys.settrace(None)
    return total, sorted(cov.calls.items()), len(cov.lines), sum(cov.lines.values())


N = 30

if __name__ == '__main__':
    print(main(N))
