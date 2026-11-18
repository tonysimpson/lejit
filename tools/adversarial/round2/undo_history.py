"""Document editor with an undo history kept by copy.deepcopy snapshots of dataclasses, enums and namedtuples, saved through dataclasses.asdict and pickle round trips: most time in rarely repeated, type-dispatched standard-library Python, a flat profile where compilation is not repaid (Barrett et al., Virtual Machine Warmup Blows Hot and Cold, OOPSLA 2017; Blackburn et al., The DaCapo Benchmarks, OOPSLA 2006)."""
import copy
import dataclasses
import enum
import pickle
from collections import namedtuple
from typing import List

N = 80


class Style(enum.Enum):
    BODY = 'body'
    HEADING = 'heading'
    QUOTE = 'quote'


class Mark(enum.Flag):
    NONE = 0
    BOLD = 1
    ITALIC = 2
    CODE = 4


Span = namedtuple('Span', 'start end mark')


@dataclasses.dataclass
class Paragraph:
    text: str
    style: Style = Style.BODY
    spans: List[Span] = dataclasses.field(default_factory=list)
    meta: dict = dataclasses.field(default_factory=dict)


@dataclasses.dataclass
class Document:
    title: str
    paras: List[Paragraph] = dataclasses.field(default_factory=list)
    version: int = 0


class Editor:
    def __init__(self, doc, depth):
        self.doc = doc
        self.undo = []
        self.redo = []
        self.depth = depth

    def apply(self, op, *args):
        self.undo.append(copy.deepcopy(self.doc))
        if len(self.undo) > self.depth:
            del self.undo[0]
        self.redo.clear()
        getattr(self, 'op_' + op)(*args)
        self.doc.version += 1

    def op_insert(self, idx, text, style):
        self.doc.paras.insert(min(idx, len(self.doc.paras)), Paragraph(text, style))

    def op_append(self, idx, text):
        if self.doc.paras:
            p = self.doc.paras[idx % len(self.doc.paras)]
            p.text += text

    def op_mark(self, idx, start, end, mark):
        if self.doc.paras:
            p = self.doc.paras[idx % len(self.doc.paras)]
            p.spans.append(Span(start, min(end, len(p.text)), mark))

    def op_delete(self, idx):
        if self.doc.paras:
            del self.doc.paras[idx % len(self.doc.paras)]

    def op_restyle(self, idx, style):
        if self.doc.paras:
            i = idx % len(self.doc.paras)
            self.doc.paras[i] = dataclasses.replace(self.doc.paras[i], style=style)

    def op_tag(self, idx, key, value):
        if self.doc.paras:
            self.doc.paras[idx % len(self.doc.paras)].meta[key] = value

    def do_undo(self):
        if self.undo:
            self.redo.append(self.doc)
            self.doc = self.undo.pop()

    def do_redo(self):
        if self.redo:
            self.undo.append(self.doc)
            self.doc = self.redo.pop()


def summary(doc):
    return [(p.style.value, p.text[:24], len(p.text),
             [(s.start, s.end, s.mark.value) for s in p.spans], sorted(p.meta.items(), key=repr))
            for p in doc.paras]


def session(k, variant):
    ed = Editor(Document('doc%d' % k), 25 + k % 10)
    styles = list(Style)
    marks = [Mark.BOLD, Mark.ITALIC, Mark.BOLD | Mark.CODE, Mark.NONE]
    trace = []
    for step in range(80):
        r = (step * 31 + k * 7) % 100
        word = 'w%d ' % step if variant == 0 else ['caf\u00e9 ', 'na\u00efve ', '\u65e5\u672c ', 'w%d ' % step][step % 4]
        if r < 25:
            ed.apply('insert', step % 5, word * (1 + r % 3), styles[r % 3])
        elif r < 45:
            ed.apply('append', step, word)
        elif r < 55:
            ed.apply('mark', step, r % 7, r % 7 + 4, marks[r % 4])
        elif r < 62:
            ed.apply('delete', step)
        elif r < 70:
            ed.apply('restyle', step, styles[(r + 1) % 3])
        elif r < 78:
            value = r if variant == 0 else [r, (r, 'x'), {'nested': [r]}, frozenset([r % 3])][r % 4]
            ed.apply('tag', step, 'k%d' % (r % 3), value)
        elif r < 90:
            ed.do_undo()
        else:
            ed.do_redo()
        if step % 20 == 19:
            saved = dataclasses.asdict(ed.doc)
            blob = pickle.dumps(ed.doc, protocol=pickle.HIGHEST_PROTOCOL)
            back = pickle.loads(blob)
            trace.append((step, len(saved['paras']), back == ed.doc, back.version,
                          copy.copy(ed.doc).paras is ed.doc.paras))
    return summary(ed.doc), ed.doc.version, len(ed.undo), len(ed.redo), trace


def main(n, variant=0):
    return [session(k, variant) for k in range(n)]
