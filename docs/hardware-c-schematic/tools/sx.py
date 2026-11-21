"""Minimal S-expression reader/writer for KiCad files."""
import re

class Sym(str):
    """Bare (unquoted) token."""
    __slots__ = ()

_tok = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))', re.S)

def parse(text):
    stack, cur = [], []
    pos, n = 0, len(text)
    while pos < n:
        m = _tok.match(text, pos)
        if not m:
            if text[pos:].strip() == "":
                break
            raise ValueError("bad sexpr at %d" % pos)
        pos = m.end()
        if m.group(1):
            stack.append(cur); cur = []
        elif m.group(2):
            done = cur; cur = stack.pop(); cur.append(done)
        elif m.group(3) is not None:
            cur.append(m.group(3).replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\"))
        else:
            cur.append(Sym(m.group(4)))
    return cur

def q(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'

def num(v):
    if isinstance(v, float):
        s = ("%.4f" % v).rstrip("0").rstrip(".")
        return "0" if s in ("-0", "") else s
    return str(v)

def dump(x, ind=0):
    """Serialize nested lists. str -> quoted, Sym -> bare, numbers -> bare."""
    if isinstance(x, list):
        simple = all(not isinstance(e, list) for e in x)
        if simple:
            return "(" + " ".join(dump(e) for e in x) + ")"
        out = "("
        first = True
        for e in x:
            if isinstance(e, list):
                out += "\n" + "\t" * (ind + 1) + dump(e, ind + 1)
            else:
                out += ("" if first else " ") + dump(e)
            first = False
        return out + "\n" + "\t" * ind + ")"
    if isinstance(x, Sym):
        return str(x)
    if isinstance(x, bool):
        return "yes" if x else "no"
    if isinstance(x, (int, float)):
        return num(x)
    return q(x)

def find(node, key):
    for e in node:
        if isinstance(e, list) and e and e[0] == key:
            return e
    return None

def findall(node, key):
    return [e for e in node if isinstance(e, list) and e and e[0] == key]
