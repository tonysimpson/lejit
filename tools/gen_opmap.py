import opcode

print("#ifndef LE_OPCODE_NAMES_H")
print("#define LE_OPCODE_NAMES_H")
rev_map = {value: name for (name, value) in opcode.opmap.items()}
print()
print("static const char *OPCODE_NAMES[255] = {")
print(",\n".join('    "{}"'.format(rev_map[i]) if i in rev_map else '    "<BAD>"' for i in range(255)))
print("};")
print()
print('#define LE_OPCODE_NAME(i) (((i) > 0 && (i) < 256) ? OPCODE_NAMES[(i)] : "<BAD>")')
print()
print("#endif /* LE_OPCODE_NAMES_H */")
