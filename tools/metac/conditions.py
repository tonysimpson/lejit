"""Rules of the general translator for policy items (METACOMPILER.md
"str and descriptors"). Mixed into gen_abstract.Walk, which calls each
hook with a line; each answers None when the expression is not one it
knows.

  - likely_key: the 'likely' item whose text, read as C, is a branch's
    condition.
  - ('promote', 'known_only'): a parameter's type is the one Ni knows
    (Psyco_KnownType); where it knows none, CPython's function runs.
  - ('shortcut', 'skip_check', C): a test the caller has made already
    (pobject.c:generic_getattr calls a descriptor's __get__ only on an
    instance of its type) holds.
"""

GA = None       # gen_abstract, set by it


class ConditionRules:

    # -- likely items --

    def likely_key(self, e):
        """The 'likely' item whose text, read as C, is the expression e."""
        want = GA.cparse.c_expr(e)
        for text in self.likely:
            try:
                x = GA.cparse.parse_expr(GA.ctokens.tokenize(text)[0], self.s.decls.types)
            except GA.cparse.ParseError:
                continue
            if GA.cparse.c_expr(x) == want:
                return text
        return None

    # -- policy items --

    def assumed(self, e):
        """('shortcut', 'skip_check', C): the caller has made the test C
        (casts aside), so it holds."""
        for item in self.tr.policy_items(self.f.name):
            if tuple(item[:2]) != ('shortcut', 'skip_check') or len(item) < 3:
                continue
            try:
                x = GA.cparse.parse_expr(GA.ctokens.tokenize(item[2])[0], self.s.decls.types)
            except GA.cparse.ParseError:
                continue
            if strip_casts(x) == strip_casts(e):
                return True
        return None

    def known_only(self):
        return ('promote', 'known_only') in [tuple(i[:2]) for i in
                                             self.tr.policy_items(self.f.name)]


def strip_casts(e):
    """e with casts and the release form's assert(...) left out, for
    comparing a macro's expansion with the body's."""
    if isinstance(e, tuple) and e:
        if e[0] == 'cast':
            return strip_casts(e[2])
        return tuple(strip_casts(x) if isinstance(x, (tuple, list)) else x for x in e)
    if isinstance(e, list):
        return [strip_casts(x) for x in e]
    return e
