#!/usr/bin/env python3
"""Lumo: small, readable, no surprises.

Rules: declare with `let`, everything is an expression, `if`/`while`/`and`/`or`
need real bools, no implicit conversions, `x |> f(y)` means `f(x, y)`.

Usage: python3 lumo.py [file.lu]   (no file = REPL)
"""
import operator, re, sys

class LumoError(Exception): pass

# ---------- lexer ----------

KEYWORDS = {"let", "fn", "if", "else", "while", "for", "in", "return",
            "break", "continue", "true", "false", "nil", "and", "or", "not"}
TOKEN_RE = re.compile(r"""
  (?P<ws>[ \t\r]+) | (?P<comment>\#[^\n]*) | (?P<nl>\n)
| (?P<num>\d+(\.\d+)?)
| (?P<str>"(\\.|[^"\\])*")
| (?P<name>[A-Za-z_]\w*)
| (?P<op>\|>|==|!=|<=|>=|[-+*/%<>=(){}\[\],;])
""", re.X)
# a newline ends a statement only after a token that can end one
ENDS_STMT = {"num", "str", "name", ")", "]", "}", "true", "false", "nil",
             "return", "break", "continue"}
# ...and never right before a token that continues the previous line
CONTINUES = {"|>", "else", "and", "or"}

def lex(src):
    toks, line, pos = [], 1, 0
    while pos < len(src):
        m = TOKEN_RE.match(src, pos)
        if not m:
            raise LumoError(f"line {line}: unexpected character {src[pos]!r}")
        kind, text, pos = m.lastgroup, m.group(), m.end()
        if kind == "nl":
            if toks and toks[-1][0] in ENDS_STMT:
                toks.append((";", "\n", line))
            line += 1
            continue
        if kind in ("ws", "comment"):
            continue
        if kind == "num":
            tok = ("num", float(text) if "." in text else int(text), line)
        elif kind == "str":
            s = re.sub(r"\\(.)", lambda e: {"n": "\n", "t": "\t"}.get(e[1], e[1]), text[1:-1])
            tok = ("str", s, line)
        elif kind == "name":
            tok = (text if text in KEYWORDS else "name", text, line)
        else:
            tok = (text, text, line)
        if tok[0] in CONTINUES and toks and toks[-1][1] == "\n":
            toks.pop()
        toks.append(tok)
    toks += [(";", "\n", line), ("eof", None, line)]
    return toks

# ---------- parser ----------

LEVELS = [("or",), ("and",), ("==", "!=", "<", "<=", ">", ">="), ("+", "-"), ("*", "/", "%")]
NOT_LEVEL = 2  # `not a == b` means `not (a == b)`

class Parser:
    def __init__(s, toks): s.t, s.i = toks, 0
    def peek(s, k=0): return s.t[s.i + k]
    def at(s, *kinds): return s.peek()[0] in kinds

    def eat(s, kind):
        tok = s.peek()
        if tok[0] != kind:
            raise LumoError(f"line {tok[2]}: expected {kind!r}, got {describe(tok)}")
        s.i += 1
        return tok

    def skip_semis(s):
        while s.at(";"): s.i += 1

    def stmts(s, end):
        out = []
        s.skip_semis()
        while not s.at(end):
            out.append(s.stmt())
            if not s.at(end): s.eat(";")
            s.skip_semis()
        return out

    def program(s): return ("seq", s.stmts("eof"))

    def block(s):
        s.eat("{")
        body = s.stmts("}")
        s.eat("}")
        return ("block", body)

    def stmt(s):
        line = s.peek()[2]
        if s.at("let"):
            s.i += 1
            name = s.eat("name")[1]
            s.eat("=")
            return ("let", name, s.expr(), line)
        if s.at("fn") and s.peek(1)[0] == "name":
            s.i += 1
            name = s.eat("name")[1]
            return ("let", name, s.fn_rest(name), line)
        if s.at("return"):
            s.i += 1
            return ("return", ("lit", None) if s.at(";", "}") else s.expr())
        if s.at("break", "continue"):
            return (s.eat(s.peek()[0])[0],)
        if s.at("while"):
            s.i += 1
            return ("while", s.expr(), s.block(), line)
        if s.at("for"):
            s.i += 1
            name = s.eat("name")[1]
            s.eat("in")
            return ("for", name, s.expr(), s.block(), line)
        e = s.expr()
        if s.at("="):
            s.i += 1
            if e[0] not in ("var", "index"):
                raise LumoError(f"line {line}: can't assign to that")
            return ("assign", e, s.expr(), line)
        return e

    def expr(s):
        left = s.binary()
        while s.at("|>"):
            line = s.eat("|>")[2]
            rhs = s.binary()
            left = (("call", rhs[1], [left] + rhs[2], line) if rhs[0] == "call"
                    else ("call", rhs, [left], line))
        return left

    def binary(s, lvl=0):
        if lvl == len(LEVELS):
            return s.unary()
        if lvl == NOT_LEVEL and s.at("not"):
            line = s.eat("not")[2]
            return ("un", "not", s.binary(lvl), line)
        left = s.binary(lvl + 1)
        while s.at(*LEVELS[lvl]):
            op, _, line = s.peek()
            s.i += 1
            left = ("bin", op, left, s.binary(lvl + 1), line)
        return left

    def unary(s):
        if s.at("-"):
            line = s.eat("-")[2]
            return ("un", "-", s.unary(), line)
        e = s.primary()
        while True:
            if s.at("("):
                line = s.eat("(")[2]
                e = ("call", e, s.items(")"), line)
            elif s.at("["):
                line = s.eat("[")[2]
                idx = s.expr()
                s.eat("]")
                e = ("index", e, idx, line)
            else:
                return e

    def items(s, close):
        out = []
        s.skip_semis()
        while not s.at(close):
            out.append(s.expr())
            s.skip_semis()
            if not s.at(close):
                s.eat(",")
                s.skip_semis()
        s.eat(close)
        return out

    def primary(s):
        tok = s.peek()
        kind, val, line = tok
        s.i += 1
        if kind in ("num", "str"): return ("lit", val)
        if kind in ("true", "false", "nil"): return ("lit", {"true": True, "false": False, "nil": None}[kind])
        if kind == "name": return ("var", val, line)
        if kind == "[": return ("list", s.items("]"))
        if kind == "fn": return s.fn_rest("fn")
        if kind == "if": return s.if_rest(line)
        if kind == "(":
            e = s.expr()
            s.eat(")")
            return e
        raise LumoError(f"line {line}: unexpected {describe(tok)}")

    def fn_rest(s, name):
        s.eat("(")
        params = []
        while not s.at(")"):
            params.append(s.eat("name")[1])
            if not s.at(")"): s.eat(",")
        s.eat(")")
        return ("fn", name, params, s.block())

    def if_rest(s, line):
        cond, then, other = s.expr(), s.block(), None
        if s.at("else"):
            s.i += 1
            other = s.if_rest(s.eat("if")[2]) if s.at("if") else s.block()
        return ("if", cond, then, other, line)

def describe(tok):
    return {"\n": "end of line", None: "end of file"}.get(tok[1], repr(tok[1]))

# ---------- runtime ----------

class Env:
    def __init__(s, parent=None): s.vars, s.parent = {}, parent
    def find(s, name, line):
        e = s
        while e:
            if name in e.vars: return e
            e = e.parent
        raise LumoError(f"line {line}: '{name}' is not defined (declare it with let)")

class Fn:
    def __init__(s, name, params, body, env): s.name, s.params, s.body, s.env = name, params, body, env

class Return(Exception):
    def __init__(s, value): s.value = value
class Break(Exception): pass
class Continue(Exception): pass

def is_num(v): return isinstance(v, (int, float)) and not isinstance(v, bool)
def is_int(v): return isinstance(v, int) and not isinstance(v, bool)

def type_name(v):
    if v is None: return "nil"
    if isinstance(v, bool): return "bool"
    if is_num(v): return "num"
    if isinstance(v, str): return "str"
    if isinstance(v, list): return "list"
    return "fn"

def show(v, nested=False):
    if v is None: return "nil"
    if isinstance(v, bool): return "true" if v else "false"
    if isinstance(v, str): return '"' + v + '"' if nested else v
    if isinstance(v, list): return "[" + ", ".join(show(x, True) for x in v) + "]"
    if isinstance(v, Fn): return f"<fn {v.name}>"
    if callable(v): return f"<builtin {v.__name__[2:]}>"
    return repr(v)

def want_bool(v, line, what):
    if isinstance(v, bool): return v
    raise LumoError(f"line {line}: {what} needs a bool, got {type_name(v)}")

def check_index(obj, idx, line):
    if not is_int(idx):
        raise LumoError(f"line {line}: index must be a whole number, got {type_name(idx)}")
    if not -len(obj) <= idx < len(obj):
        raise LumoError(f"line {line}: index {idx} out of range (length {len(obj)})")

def equal(a, b):
    if type_name(a) != type_name(b): return False
    if isinstance(a, list):
        return len(a) == len(b) and all(equal(x, y) for x, y in zip(a, b))
    return a == b

CMP = {"<": operator.lt, "<=": operator.le, ">": operator.gt, ">=": operator.ge}
MATH = {"+": operator.add, "-": operator.sub, "*": operator.mul, "%": operator.mod}

def binop(op, a, b, line):
    if op == "==": return equal(a, b)
    if op == "!=": return not equal(a, b)
    nums = is_num(a) and is_num(b)
    if op in CMP and (nums or (isinstance(a, str) and isinstance(b, str))):
        return CMP[op](a, b)
    if op == "+" and type_name(a) == type_name(b) in ("str", "list"):
        return a + b
    if nums and op in ("/", "%") and b == 0:
        raise LumoError(f"line {line}: division by zero")
    if nums and op == "/":
        return a // b if is_int(a) and is_int(b) and a % b == 0 else a / b
    if nums and op in MATH:
        return MATH[op](a, b)
    raise LumoError(f"line {line}: can't do {type_name(a)} {op} {type_name(b)}")

def call(f, args, line):
    if isinstance(f, Fn):
        if len(args) != len(f.params):
            raise LumoError(f"line {line}: {f.name} takes {len(f.params)} args, got {len(args)}")
        env = Env(f.env)
        env.vars.update(zip(f.params, args))
        try:
            return evaluate(f.body, env)
        except Return as r:
            return r.value
        except (Break, Continue):
            raise LumoError(f"line {line}: break/continue outside a loop in {f.name}")
    if callable(f):
        try:
            return f(*args)
        except (TypeError, ValueError) as e:
            raise LumoError(f"line {line}: {f.__name__[2:]}: {e}")
    raise LumoError(f"line {line}: {type_name(f)} is not a function")

def evaluate(n, env):
    k = n[0]
    if k == "lit": return n[1]
    if k == "var": return env.find(n[1], n[2]).vars[n[1]]
    if k == "list": return [evaluate(x, env) for x in n[1]]
    if k == "fn": return Fn(n[1], n[2], n[3], env)
    if k == "seq":
        result = None
        for st in n[1]: result = evaluate(st, env)
        return result
    if k == "block": return evaluate(("seq", n[1]), Env(env))
    if k == "let":
        _, name, val, line = n
        if name in env.vars:
            raise LumoError(f"line {line}: '{name}' is already declared here")
        env.vars[name] = evaluate(val, env)
        return None
    if k == "assign":
        _, target, val, line = n
        v = evaluate(val, env)
        if target[0] == "var":
            env.find(target[1], line).vars[target[1]] = v
        else:
            obj, idx = evaluate(target[1], env), evaluate(target[2], env)
            if not isinstance(obj, list):
                raise LumoError(f"line {line}: can only assign into a list, got {type_name(obj)}")
            check_index(obj, idx, line)
            obj[idx] = v
        return None
    if k == "index":
        _, o, i, line = n
        obj, idx = evaluate(o, env), evaluate(i, env)
        if not isinstance(obj, (list, str)):
            raise LumoError(f"line {line}: can't index {type_name(obj)}")
        check_index(obj, idx, line)
        return obj[idx]
    if k == "if":
        _, cond, then, other, line = n
        if want_bool(evaluate(cond, env), line, "if"):
            return evaluate(then, env)
        return evaluate(other, env) if other else None
    if k == "while":
        _, cond, body, line = n
        while want_bool(evaluate(cond, env), line, "while"):
            try: evaluate(body, env)
            except Break: break
            except Continue: pass
        return None
    if k == "for":
        _, name, it, body, line = n
        seq = evaluate(it, env)
        if not isinstance(seq, (list, str)):
            raise LumoError(f"line {line}: can't loop over {type_name(seq)}")
        for item in list(seq):
            inner = Env(env)
            inner.vars[name] = item
            try: evaluate(body, inner)
            except Break: break
            except Continue: pass
        return None
    if k == "return": raise Return(evaluate(n[1], env))
    if k == "break": raise Break()
    if k == "continue": raise Continue()
    if k == "call":
        _, f, args, line = n
        return call(evaluate(f, env), [evaluate(a, env) for a in args], line)
    if k == "un":
        _, op, e, line = n
        v = evaluate(e, env)
        if op == "not": return not want_bool(v, line, "not")
        if is_num(v): return -v
        raise LumoError(f"line {line}: can't negate {type_name(v)}")
    if k == "bin":
        _, op, l, r, line = n
        if op in ("and", "or"):
            a = want_bool(evaluate(l, env), line, op)
            if (op == "and") != a: return a  # short-circuit
            return want_bool(evaluate(r, env), line, op)
        return binop(op, evaluate(l, env), evaluate(r, env), line)
    raise AssertionError(k)

# ---------- builtins + prelude ----------

def b_print(*args):
    print(" ".join(show(a) for a in args))

def b_len(x):
    if not isinstance(x, (list, str)): raise ValueError(f"expected list or str, got {type_name(x)}")
    return len(x)

def b_push(xs, x):
    if not isinstance(xs, list): raise ValueError(f"expected list, got {type_name(xs)}")
    xs.append(x)
    return xs

def b_str(x): return show(x)

def b_num(x):
    if is_num(x): return x
    if not isinstance(x, str): raise ValueError(f"expected str, got {type_name(x)}")
    try: return int(x)
    except ValueError: return float(x)

def b_range(a, b=None):
    if not is_int(a) or (b is not None and not is_int(b)): raise ValueError("expected whole numbers")
    return list(range(a) if b is None else range(a, b))

def b_type(x): return type_name(x)
def b_input(prompt=""): return input(prompt)

BUILTINS = {f.__name__[2:]: f for f in (b_print, b_len, b_push, b_str, b_num, b_range, b_type, b_input)}

# the standard library is written in Lumo itself
PRELUDE = """
fn map(xs, f) {
    let out = []
    for x in xs { push(out, f(x)) }
    out
}
fn filter(xs, f) {
    let out = []
    for x in xs { if f(x) { push(out, x) } }
    out
}
fn reduce(xs, start, f) {
    let acc = start
    for x in xs { acc = f(acc, x) }
    acc
}
"""

def run(src, env):
    try:
        return evaluate(Parser(lex(src)).program(), env)
    except Return:
        raise LumoError("return outside a function")
    except (Break, Continue):
        raise LumoError("break/continue outside a loop")
    except RecursionError:
        raise LumoError("too much recursion")

def new_env():
    root = Env()
    root.vars.update(BUILTINS)
    run(PRELUDE, root)
    return Env(root)  # user code may shadow builtins

def main():
    sys.setrecursionlimit(10000)
    env = new_env()
    if len(sys.argv) > 1:
        try:
            with open(sys.argv[1], encoding="utf-8") as f:
                run(f.read(), env)
        except LumoError as e:
            sys.exit(f"error: {e}")
        return
    print("lumo — ctrl-d to quit")
    buf = ""
    while True:
        try:
            buf += input("... " if buf else "> ") + "\n"
        except EOFError:
            print()
            return
        # ponytail: naive bracket count (ignores brackets inside strings)
        if any(buf.count(o) > buf.count(c) for o, c in ("{}", "[]", "()")):
            continue
        try:
            v = run(buf, env)
            if v is not None: print(show(v, nested=True))
        except LumoError as e:
            print(f"error: {e}")
        buf = ""

if __name__ == "__main__":
    main()
