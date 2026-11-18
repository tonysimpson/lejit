"""Character-level string work in Python: a tokeniser that iterates, slices and tests membership one character at a time."""

SOURCE = '''
def area(shape, scale=1.5):
    # compute the area
    if shape.kind == "circle" and shape.r >= 0:
        return 3.14159 * shape.r ** 2 * scale
    return shape.w * shape.h + 0x1f - 0b101 + 1e-3
names = ['alpha', "beta", 'gam\\'ma'] + [x for x in range(10) if x != 3]
'''

KEYWORDS = {'def', 'if', 'and', 'return', 'for', 'in', 'not', 'or', 'else', 'while'}
OPERATORS = ('**', '==', '!=', '>=', '<=', '+', '-', '*', '/', '=', '<', '>', '(', ')',
             '[', ']', ':', ',', '.')


def tokenise(text):
    tokens = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in ' \t':
            i += 1
        elif ch == '\n':
            tokens.append(('NL', ''))
            i += 1
        elif ch == '#':
            while i < n and text[i] != '\n':
                i += 1
        elif ch.isalpha() or ch == '_':
            j = i
            while j < n and (text[j].isalnum() or text[j] == '_'):
                j += 1
            word = text[i:j]
            tokens.append(('KW' if word in KEYWORDS else 'NAME', word))
            i = j
        elif ch.isdigit():
            j = i
            while j < n and (text[j].isalnum() or text[j] == '.' or text[j] == '-' and text[j - 1] in 'eE'):
                j += 1
            tokens.append(('NUM', text[i:j]))
            i = j
        elif ch in '"\'':
            j = i + 1
            while text[j] != ch:
                j += 2 if text[j] == '\\' else 1
            tokens.append(('STR', text[i + 1:j]))
            i = j + 1
        else:
            for op in OPERATORS:
                if text.startswith(op, i):
                    tokens.append(('OP', op))
                    i += len(op)
                    break
            else:
                raise SyntaxError(ch)
    return tokens


def main(n):
    counts = {}
    width = 0
    for k in range(n):
        for kind, value in tokenise(SOURCE.replace('10', str(k % 50))):
            counts[kind] = counts.get(kind, 0) + 1
            width += len(value)
    return sorted(counts.items()), width


N = 1500

if __name__ == '__main__':
    print(main(N))
