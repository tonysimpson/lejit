"""Abstract values (doc/GENERATOR_DESIGN.md "Abstract values"): what the
translator knows of each C value on one path.

Binding times: gen (the generator folds it: `const`), meta (a C value of
the meta: `c` is its C), dynamic (a vinfo: `c` names it), pointer (a
run-time base and a meta offset; not used by the families moved so far).

Ownership of a variable holding a dynamic value, as a reference count the
meta holds: 'borrowed' (none: a parameter, or a copy of one), 'new' (one,
released at its last use and at every exit), 'mixed' (one on some paths:
own_NAME holds it or NULL), 'none' (no value: released, or never
assigned).

A value's error convention (`conv`, a Conv) says how the C would test it
and what Ni already knows: the conventions of conventions.py.

The path's error state: Path.failed is set where a call's error test held
or a compile-time exception was raised; on such a path no residual call is
made and nothing resumes.
"""
import copy


class Conv:
    """How a call's result shows its error. kind: a conventions.py
    convention; checkable: the C tests it, and the meta's NULL vinfo is
    that error; notimpl: it may be NotImplemented; always_error: it is
    always the error (a meta that only raises)."""

    def __init__(self, kind=None, checkable=False, notimpl=False, always_error=False,
                 check_now=False, never=False):
        self.kind, self.checkable, self.notimpl = kind, checkable, notimpl
        self.always_error, self.check_now, self.never = always_error, check_now, never

    def but(self, **kw):
        c = copy.copy(self)
        c.__dict__.update(kw)
        return c

    def __eq__(self, other):
        return isinstance(other, Conv) and self.__dict__ == other.__dict__

    def __hash__(self):
        return hash(tuple(sorted(self.__dict__.items())))


class Var:
    """A C variable of the function as the meta holds it."""

    def __init__(self, name, ctype, bt, cname, isparam=False):
        self.name, self.ctype, self.bt, self.cname = name, ctype, bt, cname
        self.isparam = isparam
        self.own = 'borrowed' if isparam else 'none'
        self.conv = None
        self.tp = None            # meta table: the meta variable of its type
        self.alias = None         # dynamic: the parameter it copies
        self.known_null = False   # assigned NULL: returned, it is the error
        self.singleton = None     # assigned an immortal object (types.SINGLETONS)
        self.rng = None           # an integer's (lo, hi), where known
        self.word_of = None       # the object variable whose word it holds

    def clone(self):
        v = Var.__new__(Var)
        v.__dict__.update(self.__dict__)
        return v

    def owned(self):
        return self.own in ('new', 'mixed')


class Val:
    """The value of an expression."""

    def __init__(self, bt, c, ctype, tp=None, lv=False, owned=False, conv=None,
                 var=None, const=None, rng=None, last=False):
        self.bt, self.c, self.ctype = bt, c, ctype
        self.tp = tp              # meta: the meta variable of the type it is inside
        self.lv = lv              # meta: an address inside a type (a slot's)
        self.owned = owned        # dynamic: a new reference the statement must release
        self.conv = conv
        self.var = var            # dynamic: the variable it is read from
        self.const = const        # gen: True or False
        self.rng = rng            # an integer's (lo, hi), where known
        self.last = last          # a word's last read in the statement
        self.made = None          # a representation's virtual: its type


class Path:
    """One path through the function."""

    def __init__(self):
        self.vars = {}
        self.known = set()        # object variables whose type is promoted
        self.called = False       # a run-time call may have been emitted
        self.slots = {}           # slot address -> meta variable read
        self.ended = False        # the path returned
        self.early = False        # some path may have returned before here
        self.failed = False       # the error state: an error is pending

    def copy(self):
        p = Path()
        p.vars = {k: v.clone() for k, v in self.vars.items()}
        p.known, p.called = set(self.known), self.called
        p.slots, p.ended, p.early = dict(self.slots), self.ended, self.early
        p.failed = self.failed
        return p

    def take(self, other):
        """Become other (a copy taken earlier)."""
        self.__dict__.update(other.__dict__)
