"""Billing loop over line items that are almost all small ints, where one item in k is a bool, IntEnum, int subclass, float, NaN or a product past 2**63, with k set by the variant: guards that fail periodically, a deoptimisation storm (Hoelzle, Chambers and Ungar, PLDI 1992; V8 deoptimisation loops, Meurer, An Introduction to Speculative Optimization in V8, 2017)."""
import enum
import math

N = 200


class Pack(enum.IntEnum):
    SINGLE = 1
    PAIR = 2
    DOZEN = 12


class Cents(int):
    def __add__(self, other):
        return Cents(int(self) + int(other))

    __radd__ = __add__

    def __mul__(self, other):
        return Cents(int(self) * int(other))

    __rmul__ = __mul__


def odd_value(j, slot):
    kind = j % 6
    if slot == 'qty':
        return [True, Pack.DOZEN, Cents(3), 2.5, 7, 3 << 62][kind]
    return [Cents(199), 12.75, float('nan'), 1 << 61, False, Pack.PAIR][kind]


def make_items(i, count, every):
    items = []
    for j in range(count):
        qty = (j * 7 + i) % 9 + 1
        price = (j * 31 + i * 3) % 500 + 50
        if every and j % every == every - 1:
            if (j // every) % 2:
                qty = odd_value(j // every, 'qty')
            else:
                price = odd_value(j // every, 'price')
        items.append((qty, price, j % 4))
    return items


def line_total(qty, price, band):
    amount = qty * price
    if band == 1:
        amount = amount - amount // 10 if isinstance(amount, int) else amount * 0.9
    elif band == 2:
        amount = amount + 25
    elif band == 3 and qty > 5:
        amount = amount * 2 - price
    return amount


def invoice(items):
    subtotal = 0
    skipped = 0
    largest = 0
    kinds = {}
    for qty, price, band in items:
        amt = line_total(qty, price, band)
        if isinstance(amt, float) and math.isnan(amt):
            skipped += 1
            continue
        subtotal = subtotal + amt
        if amt > largest:
            largest = amt
        k = type(amt).__name__
        kinds[k] = kinds.get(k, 0) + 1
    tax = subtotal * 20 // 100 if isinstance(subtotal, int) else round(subtotal * 0.2, 2)
    return (subtotal if isinstance(subtotal, int) else round(subtotal, 4), tax, largest,
            skipped, sorted(kinds.items()))


def main(n, variant=0):
    every = [0, 97, 3][variant]
    out = []
    for i in range(n * 10):
        out.append(invoice(make_items(i, 400 + i % 50, every)))
    return out
