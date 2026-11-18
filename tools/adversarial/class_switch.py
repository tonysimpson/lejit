"""__class__ assignment: connections that change class as their state changes (the state pattern)."""


class Connection:
    def __init__(self, ident):
        self.ident = ident
        self.sent = 0
        self.opened = 0
        self.errors = 0

    def handle(self, event):
        return 0


class Closed(Connection):
    def handle(self, event):
        if event == 'open':
            self.__class__ = Opening
            return 1
        return 0


class Opening(Connection):
    def handle(self, event):
        if event == 'ack':
            self.opened += 1
            self.__class__ = Open
            return 2
        if event == 'error':
            self.errors += 1
            self.__class__ = Closed
        return 0


class Open(Connection):
    def handle(self, event):
        if event == 'send':
            self.sent += 1
            self.__class__ = Sending
            return 3
        if event == 'close':
            self.__class__ = Closing
        return 0


class Sending(Connection):
    def handle(self, event):
        if event == 'ack':
            self.__class__ = Open
            return 4
        if event == 'error':
            self.errors += 1
            self.__class__ = Closed
        return 0


class Closing(Connection):
    def handle(self, event):
        if event == 'ack':
            self.__class__ = Closed
            return 5
        return 0


EVENTS = ['open', 'ack', 'send', 'ack', 'send', 'error', 'close', 'ack', 'open', 'ack']


def main(n):
    conns = [Closed(i) for i in range(64)]
    score = 0
    for step in range(n):
        for c in conns:
            event = EVENTS[(step + c.ident * 3) % len(EVENTS)]
            score += c.handle(event)
    states = {}
    for c in conns:
        states[type(c).__name__] = states.get(type(c).__name__, 0) + 1
    return (score, sum(c.sent for c in conns), sum(c.opened for c in conns),
            sum(c.errors for c in conns), sorted(states.items()))


N = 3000

if __name__ == '__main__':
    print(main(N))
