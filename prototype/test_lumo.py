from lumo import LumoError, new_env, run

def r(src): return run(src, new_env())

assert r("1 + 2 * 3") == 7
assert r("7 / 2") == 3.5 and r("6 / 2") == 3
assert r('"a" + "b"') == "ab"
assert r("not 1 == 2") is True
assert r("fn fib(n) { if n < 2 { n } else { fib(n-1) + fib(n-2) } }\nfib(15)") == 610
assert r("[1, 2, 3] |> map(fn(x) { x * 2 }) |> reduce(0, fn(a, b) { a + b })") == 12
assert r("let x = 0\nfor i in range(5) { x = x + i }\nx") == 10
assert r("let c = fn() {\nlet n = 0\nfn() { n = n + 1\nn }\n}()\nc()\nc()") == 2
assert r("let x = if false { 1 }\nelse if true { 2 }\nelse { 3 }\nx") == 2
assert r("[1, [2]] == [1, [2]]") is True and r("1 == true") is False

for bad, msg in [('1 + "a"', "can't do"), ("y = 1", "not defined"),
                 ("if 1 { 2 }", "needs a bool"), ("let a = 1\nlet a = 2", "already declared"),
                 ("[1][5]", "out of range"), ("1 / 0", "division by zero"), ("x = 1 y", "expected")]:
    try:
        r(bad)
        raise AssertionError(f"should fail: {bad}")
    except LumoError as e:
        assert msg in str(e), (bad, e)
print("ok")
