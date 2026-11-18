"""C types over the front end's AST: each declaration's type as text the
generated C can use ('PyObject *', 'const char *', 'binaryfunc'), and the
type of an expression from the declarations in scope. Typedefs are
followed only to answer a question (is it an object, a function pointer,
an integer); the text keeps the name the source uses.

The AST is never trusted for sizes or offsets (the prelude drops packed
and aligned): nothing here computes one.
"""
from pycparser import c_ast, c_generator

INTEGER_WORDS = {'char', 'short', 'int', 'long', 'signed', 'unsigned', '_Bool'}
# method tables a type points to: the translator holds them at compile time
TABLES = {'PyNumberMethods', 'PySequenceMethods', 'PyMappingMethods',
          'PyAsyncMethods', 'PyBufferProcs'}
TYPE_OBJECT = {'PyTypeObject', '_typeobject'}

_gen = c_generator.CGenerator()


def text(t, name=None):
    """Type node t (a TypeDecl, PtrDecl, ArrayDecl, FuncDecl, Typename or
    Decl) as C: a type name, or with `name` a declaration of it."""
    if isinstance(t, (c_ast.Typename, c_ast.Decl)):
        t = t.type
    if name is None:
        return _gen._generate_type(t, emit_declname=False).strip()
    return _gen._generate_type(named(t, name)).strip()


def named(t, name):
    """A copy of type node t declaring `name`."""
    if isinstance(t, c_ast.TypeDecl):
        return c_ast.TypeDecl(name, t.quals, t.align, t.type)
    if isinstance(t, c_ast.PtrDecl):
        return c_ast.PtrDecl(t.quals, named(t.type, name))
    if isinstance(t, c_ast.ArrayDecl):
        return c_ast.ArrayDecl(named(t.type, name), t.dim, t.dim_quals)
    if isinstance(t, c_ast.FuncDecl):
        return c_ast.FuncDecl(t.args, named(t.type, name))
    return t


def ptr(t):
    return c_ast.PtrDecl([], t)


INT = c_ast.TypeDecl(None, [], None, c_ast.IdentifierType(['int']))
LONG = c_ast.TypeDecl(None, [], None, c_ast.IdentifierType(['long']))
CHAR_P = ptr(c_ast.TypeDecl(None, ['const'], None, c_ast.IdentifierType(['char'])))
VOID_P = ptr(c_ast.TypeDecl(None, [], None, c_ast.IdentifierType(['void'])))


class Unit:
    """The file-scope declarations of one translation unit: typedefs,
    structs (with their fields, in order), functions and variables."""

    def __init__(self, ast):
        self.typedefs, self.structs = {}, {}
        self.funcs, self.vars, self.defs = {}, {}, {}
        self.enums = {}
        for d in ast.ext:
            self._decl(d)
        for n in _walk(ast):
            if isinstance(n, c_ast.Struct) and n.decls is not None and n.name:
                self.structs[n.name] = n
            elif isinstance(n, c_ast.Enumerator):
                self.enums[n.name] = n

    def _decl(self, d):
        if isinstance(d, c_ast.FuncDef):
            self.defs[d.decl.name] = d
            self.funcs[d.decl.name] = d.decl.type
        elif isinstance(d, c_ast.Typedef):
            self.typedefs[d.name] = d.type
        elif isinstance(d, c_ast.Decl) and d.name:
            if isinstance(d.type, c_ast.FuncDecl):
                self.funcs.setdefault(d.name, d.type)
            else:
                self.vars[d.name] = d.type

    # ---- questions about a type ----

    def resolve(self, t):
        """t with typedef names followed, outermost first, until it is not
        a TypeDecl naming a typedef."""
        seen = 0
        while isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.IdentifierType) \
                and t.type.names[-1] in self.typedefs and seen < 50:
            t = self.typedefs[t.type.names[-1]]
            seen += 1
        return t

    def struct_name(self, t):
        """The struct a type names, through typedefs ('PyObject' gives
        '_object'), or None."""
        t = self.resolve(t)
        if isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.Struct):
            return t.type.name
        return None

    def pointee(self, t):
        t = self.resolve(t)
        return t.type if isinstance(t, c_ast.PtrDecl) else None

    def fields(self, struct):
        """[(name, type)] of a struct's fields, or None if not defined."""
        s = self.structs.get(struct)
        if s is None or s.decls is None:
            return None
        return [(d.name, d.type) for d in s.decls]

    def field(self, struct_type, name):
        """The type of field `name` of the struct struct_type names (an
        anonymous struct through its typedef)."""
        t = self.resolve(struct_type)
        if isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.Struct) \
                and t.type.decls is not None:
            fs = [(d.name, d.type) for d in t.type.decls]
        else:
            fs = self.fields(self.struct_name(struct_type) or '')
        for n, t in fs or ():
            if n == name:
                return t
        return None

    def is_type_object(self, t):
        p = self.pointee(t)
        return p is not None and (self.struct_name(p) in TYPE_OBJECT or _name(p) in TYPE_OBJECT)

    def is_object(self, t):
        """A pointer to an object struct: one whose first field is an
        object header (ob_base), through typedefs; type objects are not
        (the translator holds them at compile time)."""
        p = self.pointee(t)
        if p is None or isinstance(self.resolve(p), c_ast.PtrDecl):
            return False
        return self._object_struct(p)

    def _object_struct(self, t, depth=0):
        t = self.resolve(t)
        if depth > 20 or not (isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.Struct)):
            return False
        s = t.type.name
        if s in TYPE_OBJECT:
            return False
        if s == '_object':
            return True
        fs = [(d.name, d.type) for d in t.type.decls] if t.type.decls is not None \
            else self.fields(s or '')
        if not fs or fs[0][0] != 'ob_base':
            return False
        return self._object_struct(fs[0][1], depth + 1)

    def is_table(self, t):
        p = self.pointee(t)
        return p is not None and _name(p) in TABLES | {self.struct_name(p)} & TABLES

    def is_fnptr(self, t):
        t = self.resolve(t)
        return isinstance(t, c_ast.PtrDecl) and isinstance(self.resolve(t.type), c_ast.FuncDecl)

    def fn_type(self, t):
        """The FuncDecl a function or function-pointer type stands for."""
        t = self.resolve(t)
        if isinstance(t, c_ast.FuncDecl):
            return t
        if isinstance(t, c_ast.PtrDecl):
            f = self.resolve(t.type)
            if isinstance(f, c_ast.FuncDecl):
                return f
        return None

    def is_int(self, t):
        t = self.resolve(t)
        if isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.IdentifierType):
            return bool(set(t.type.names) & INTEGER_WORDS) and 'double' not in t.type.names \
                and 'float' not in t.type.names
        return isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.Enum)

    def is_double(self, t):
        t = self.resolve(t)
        return isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.IdentifierType) \
            and t.type.names[-1] in ('double', 'float')

    def is_char_ptr(self, t):
        p = self.pointee(t)
        p = self.resolve(p) if p is not None else None
        return isinstance(p, c_ast.TypeDecl) and isinstance(p.type, c_ast.IdentifierType) \
            and p.type.names[-1] == 'char'

    def is_void(self, t):
        t = self.resolve(t)
        return isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.IdentifierType) \
            and t.type.names == ['void']

    def is_meta_type(self, t):
        """A type whose values the meta holds as C values: type objects,
        their method tables, function pointers, C strings."""
        return self.is_type_object(t) or self.is_table(t) or self.is_fnptr(t) \
            or self.is_char_ptr(t)

    def is_object_array_ptr(self, t):
        """PyObject **: a pointer into an object's items."""
        p = self.pointee(t)
        return p is not None and self.is_object(p)

    # ---- the type of an expression ----

    def type_of(self, e, scope):
        """The type node of expression e, its locals' types in `scope`
        ({name: type}); None where it can't tell."""
        if isinstance(e, c_ast.ID):
            if e.name in scope:
                return scope[e.name]
            if e.name in self.vars:
                return self.vars[e.name]
            if e.name in self.funcs:
                return self.funcs[e.name]
            if e.name in self.enums:
                return INT
            return None
        if isinstance(e, c_ast.Constant):
            if e.type == 'string':
                return CHAR_P
            if e.type in ('double', 'float'):
                return c_ast.TypeDecl(None, [], None, c_ast.IdentifierType(['double']))
            return LONG if 'l' in e.value.lower().lstrip('0x') else INT
        if isinstance(e, c_ast.Cast):
            return e.to_type.type
        if isinstance(e, c_ast.StructRef):
            b = self.type_of(e.name, scope)
            if b is None:
                return None
            if e.type == '->':
                b = self.pointee(b)
            return self.field(b, e.field.name) if b is not None else None
        if isinstance(e, c_ast.FuncCall):
            f = self.type_of(e.name, scope)
            fd = self.fn_type(f) if f is not None else None
            return fd.type if fd is not None else None
        if isinstance(e, c_ast.UnaryOp):
            if e.op == '*':
                b = self.type_of(e.expr, scope)
                return self.pointee(b) if b is not None else None
            if e.op == '&':
                b = self.type_of(e.expr, scope)
                return ptr(b) if b is not None else None
            if e.op in ('!', 'sizeof'):
                return INT if e.op == '!' else LONG
            return self.type_of(e.expr, scope)
        if isinstance(e, c_ast.ArrayRef):
            b = self.resolve(self.type_of(e.name, scope))
            if isinstance(b, (c_ast.PtrDecl, c_ast.ArrayDecl)):
                return b.type
            return None
        if isinstance(e, c_ast.BinaryOp):
            if e.op in ('==', '!=', '<', '>', '<=', '>=', '&&', '||'):
                return INT
            return self.type_of(e.left, scope)
        if isinstance(e, c_ast.TernaryOp):
            return self.type_of(e.iftrue, scope)
        if isinstance(e, c_ast.ExprList):
            return self.type_of(e.exprs[-1], scope)
        return None


def _name(t):
    if isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.IdentifierType):
        return t.type.names[-1]
    return None


def _walk(n):
    yield n
    for _, c in n.children():
        yield from _walk(c)


def strip_casts(e):
    while isinstance(e, c_ast.Cast):
        e = e.expr
    return e
