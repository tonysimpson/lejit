"""The targeted fuzzer's building blocks (combine.py).

A block is a Python fragment that makes LeJIT take one decision and then
invalidates it. Its code is written as Python with three kinds of
placeholder, filled in by `instantiate`:
  P_name   a name of the block's own; P_ becomes the block's prefix (b0_,
           b1_...), so the same block can appear twice in a program
  NEXT(r)  the hook: a call of the next block's P_run (innermost: ident),
           made from inside the decision's code, so that what the next
           block does happens in the middle of this one
  S_name   a slot, replaced by one of the block's values for it (Python
           source: an expression, or a statement where the slot is one),
           picked by the program's seed
and it must define
  P_run(r)   makes the decision, called once a round with the round
             number; returns a value with repr() free of addresses that
             vary (they are masked, but ordering must not depend on them)
  P_break()  the invalidation, called once, before the round the program
             schedules it for; some set a flag P_run acts on mid-call
Blocks print nothing, read no time, ids or randomness, and keep what they
accumulate bounded. `leaks` says why LeJIT grows with the block by
design (the leak oracle skips programs holding such a block).
"""
import importlib, os, pkgutil, re


class Block:
    def __init__(self, name, area, decision, invalidation, code, slots=None, leaks=None):
        self.name, self.area = name, area
        self.decision, self.invalidation = decision, invalidation
        self.code = code.strip('\n')
        self.slots = slots or {}
        self.leaks = leaks
        used = set(re.findall(r'\bS_(\w+)', self.code))
        if used != set(self.slots):
            raise ValueError('%s: slots %s used, %s given' % (name, sorted(used), sorted(self.slots)))
        if 'def P_run(' not in self.code or 'def P_break(' not in self.code:
            raise ValueError('%s: needs P_run and P_break' % name)
        if self.code.count('NEXT(') != 1:
            raise ValueError('%s: needs exactly one NEXT(r) hook' % name)

    def instantiate(self, prefix, next_name, rng):
        """(source, {slot: value chosen})"""
        chosen = {k: rng.choice(v) for k, v in sorted(self.slots.items())}
        src = re.sub(r'\bS_(\w+)', lambda m: chosen[m.group(1)], self.code)
        src = re.sub(r'\bP_', prefix + '_', src)
        src = src.replace('NEXT(', next_name + '(')
        return src, chosen


def load():
    """every block, in a fixed order (module name, then position in it)"""
    blocks = []
    here = os.path.dirname(__file__)
    for m in sorted(pkgutil.iter_modules([here]), key=lambda m: m.name):
        mod = importlib.import_module(__name__ + '.' + m.name)
        blocks.extend(mod.BLOCKS)
    names = [b.name for b in blocks]
    dup = {n for n in names if names.count(n) > 1}
    if dup:
        raise ValueError('duplicate block names: %s' % sorted(dup))
    return blocks
