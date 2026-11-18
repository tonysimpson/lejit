"""The C builder: generated C as a tree of lines and blocks, laid out
when it is rendered, so no translator code counts indentation."""
import re


class Code:
    """A sequence of statements; depth is how deep its block nests in the
    function's body (1: the body itself)."""

    def __init__(self, depth=1):
        self.items = []
        self.depth = depth

    def sub(self):
        """A block nested in this one."""
        return Code(self.depth + 1)

    def __bool__(self):
        return bool(self.items)

    def line(self, text):
        self.items.append(('line', text))

    def extend(self, other):
        self.items.extend(other.items)

    def if_(self, cond, then, other=None):
        """if (cond) { then } [else { other }]"""
        self.items.append(('if', cond, then, other if other else None))

    def unbraced_if(self, cond, text):
        self.items.append(('uif', cond, text))

    def switch(self, expr, cases):
        """cases: [(label, Code)], each label 'case 1' or 'default'."""
        self.items.append(('switch', expr, cases))

    def block(self, head, body):
        """head { body }, for loops: 'for (;;)'."""
        self.items.append(('block', head, body))

    def render(self, depth):
        out = []
        pad = '  ' * depth
        for it in self.items:
            kind = it[0]
            if kind == 'line':
                out.append(pad + it[1])
            elif kind == 'if':
                out.append('%sif (%s) {' % (pad, it[1]))
                out += it[2].render(depth + 1)
                if it[3] is not None:
                    out.append(pad + '}')
                    out.append(pad + 'else {')
                    out += it[3].render(depth + 1)
                out.append(pad + '}')
            elif kind == 'uif':
                out.append('%sif (%s)' % (pad, it[1]))
                out.append('%s  %s' % (pad, it[2]))
            elif kind == 'switch':
                out.append('%sswitch (%s) {' % (pad, it[1]))
                for label, body in it[2]:
                    out.append('%s%s:' % (pad, label))
                    out += body.render(depth + 1)
                out.append(pad + '}')
            elif kind == 'block':
                out.append('%s%s {' % (pad, it[1]))
                out += it[2].render(depth + 1)
                out.append(pad + '}')
        return out


class Function:
    """A function: its head ('static int f(int x)'), its local
    declarations ({name: declaration}, written sorted by name, only those
    the body uses) and its body."""

    def __init__(self, head, decls, body, comment=None):
        self.head, self.decls, self.body = head, decls, body
        self.comment = comment

    def render(self):
        body = self.body.render(1)
        used = set(re.findall(r'\b\w+\b', '\n'.join(body)))
        lines = ['/* %s */' % self.comment] if self.comment else []
        lines.append(self.head + ' {')
        lines += ['  ' + d for n, d in sorted(self.decls.items()) if n in used]
        lines += body
        lines.append('}')
        return '\n'.join(lines) + '\n'
