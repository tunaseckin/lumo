#!/bin/sh
# Build, run tests/test.lu, then check that bad programs fail with the right message.
# Run from anywhere: ./tests/run.sh  (or: make test)
cd "$(dirname "$0")/.." || exit 1
make -s lumo || exit 1   # the real build (with graphics on macOS)
./lumo tests/test.lu || exit 1
# memory/UB check (skipped if the sanitizer build fails)
CC="${CC:-cc}"
if $CC -g -O1 -fsanitize=address,undefined -o "${TMPDIR:-/tmp}/lumo_asan" lumo.c -lm 2>/dev/null; then
  # memory errors only: types and the last objects are deliberately freed by process exit, not one by one
  ASAN_OPTIONS=detect_leaks=0 "${TMPDIR:-/tmp}/lumo_asan" tests/test.lu > /dev/null || { echo "FAIL: sanitizer"; exit 1; }
fi

# each snippet runs from a file in tests/ (so imports resolve the same everywhere, Windows too)
snippet=tests/_snippet.lu
trap 'rm -f "$snippet"' EXIT
check() {
  printf '%s\n' "$1" > "$snippet"
  out=$(./lumo "$snippet" 2>&1)
  case "$out" in
    *"$2"*) ;;
    *) echo "FAIL: $1"; echo "  wanted: $2"; echo "  got:    $out"; exit 1 ;;
  esac
}
check '1 + "a"' "line 1: can't do num + str"
check 'fn area(r: num) -> num { r * r }
area("x")' 'area: r should be num, got str'
check 'fn area(r: num) -> num { r * r }
area(1, 2)' 'area takes 1 args, got 2'
check 'fn f(x: num) -> str { x }' 'f should return str, got num'
check 'let x = 5
x = "a"' "'x' is num, can't assign str"
check 'let x: str = 5' 'x is declared str, got num'
check 'let xs: list[num] = [1, "a"]' 'xs is declared list[num], got list'
check 'let m: map[str, num] = {a: 1, b: "x"}' 'm is declared map[str, num], got map'
check 'let m = {a: 1}
m.b = "x"' 'map[str, num] holds num'
check 'let n = 5
n()' 'num is not a function'
check 'upper(5)' 'upper: argument 1 should be str, got num'
check 'let x: foo = 1' 'unknown type'
check 'fn f() { 1 }
print(f() + 1)
for c in 5 { }' "can't loop over num"
check 'print("never runs")
if 3 { 1 }' 'condition needs a bool, got num'
check 'fn f(g: fn(num) -> num) { g(1) }
f(fn(s: str) -> str { s })' 'f: g should be fn(num) -> num'

check 'y = 1' "'y' doesn't exist yet"
check 'if 1 { 2 }' 'needs a bool'
check 'true and 1' 'and needs a bool, got num'
check 'fn f(x) { x }
true and f(1)' 'and/or needs a bool, got num'
check 'let a = 1
let a = 2' 'already declared'
check 'fn f() { let b = 1
let b = 2 }' 'already declared'
check '[1][5]' 'out of range'
check '1 / 0' 'division by zero'
check 'let x = 1 y' 'expected end of statement'
check '{a: 1}.b' 'not found'
check 'fn f(a) { a }
f(1, 2)' 'takes 1 args, got 2'
check 'fn f() { f() }
f()' 'too much recursion'
check 'break' 'outside a loop'
check 'return 1' 'outside a function'
check '"x"[0] = "y"' "can't be changed"
check 'print = 1' 'is a builtin'
check 'let s = "abc' 'unterminated string'
check '!true' "use 'not'"
check 'fn f() { nope }
f()' "line 1: 'nope' is not defined"
check 'for n in range(3) { try { break } catch { nil } }
error("boom")' 'line 2: boom'
check 'fn f() { try { return 1 } catch { 2 } }
f()
error("boom")' 'line 3: boom'
check 'try { 1 }' "expected 'catch'"
check 'error({code: 1})' 'line 1: {"code": 1}'
check 'let s = "a${1 2}"' "expected '}'"
check 'let s = "a${1' 'unterminated string'
check 'import "nope.lu"' "can't read"
check 'import "my-mod.lu"' 'as name'
check 'import "testlib/geo.lu" as geo
geo.nope' 'not found'
check 'struct P { x: num }
P("a")' 'P: x should be num, got str'
check 'struct P { x: num }
let p = P(1)
p.y' "P has no field 'y'"
check 'struct P { x: num }
let p = P(1)
p.x = "s"' "P.x is num, can't assign str"
check 'struct P { x, x }' "duplicate field 'x'"
check 'struct P { x: num }
fn f(p: P) { p }
f({x: 1})' 'f: p should be P, got map'
check 'struct P { x: num }
P(1, 2)' 'P takes 1 args, got 2'
# friendly errors for beginners
check 'let name = "a"
print(nmae)' "'nmae' is not defined (did you mean 'name'?)"
check 'fn f(count) { cuont + 1 }' "'cuont' is not defined (did you mean 'count'?)"
check 'pritn("x")' "(did you mean 'print'?)"
check 'retrun 5' "(did you mean 'return'?)"
check 'let x = null' "'null' is not defined (use nil)"
check 'let x = 1
if x > 0 { 1 }
elif x < 0 { 2 }' "use 'else if'"
check 'def f() { 1 }' 'functions start with fn'
check 'var x = 1' 'declare variables with let'
check 'let not = 72' "'not' is a reserved word in Lumo"
check 'fn f(for) { 1 }' "'for' is a reserved word in Lumo"
check 'let x = 1
if x = 1 { 2 }' 'use == to compare'
check 'if true: 1' "blocks use { ... }, not ':'"
check 'let x = 1 // comment' 'comments start with #'
check 'true && false' "use 'and'"
check 'true || false' "use 'or'"
check '"a" & 1' "can't do str & num"
check 'let r = "a"..3' "can't do str..num"
check "let s = 'hi'" 'strings use double quotes'
check 'fn f(x: int) { x }' "unknown type 'int' (use num)"
check 'struct User { name: str }
let u = User("a")
u.nmae' "User has no field 'nmae' (did you mean 'name'?)"
check 'let m: any = {color: 1}
m.colr' "key \"colr\" not found (did you mean 'color'?)"
check 'let a = 1
let b = 2
print(a + "x")' '   3 | print(a + "x")'
# habits from other languages and common slips (found by simulated beginners)
check 'let yıldız_x = 1
print(yildiz_x)' "(did you mean 'yıldız_x'?)"
check 'let Skor = 1
print(skor)' "(did you mean 'Skor'?)"
check 'let x = 1
x += 1' 'Lumo has no +=: write x = x + 1'
check 'let x = 1
x++' 'Lumo has no ++'
check 'print(2 ** 3)' 'use pow(a, b)'
check 'print(7 // 2)' 'floor(a / b)'
check 'let n = 1
print(f"{n}")' 'Lumo has no f-strings'
check 'let s = """a"""' 'no triple-quoted strings'
check 'let s = "abc
print(1)' 'line 1: unterminated string (a string ends on the line it starts'
check 'let xs: list = [1]
xs.append(2)' 'list has no .append: Lumo uses functions, write push(xs, x)'
check 'let xs: any = [1]
xs.append(2)' 'list has no .append: Lumo uses functions, write push(xs, x)'
check 'let s: any = "a"
print(s.upper())' 'str has no .upper'
check 'let xs = [1, 2, 3]
print(xs[0:2])' 'use slice(xs, a, b)'
check 'let xs = [1]
print([x * 2 for x in xs])' 'no list comprehensions'
check 'fn f(a) { a }
f(a = 1)' 'no named arguments'
check 'let x = 1
if x > 0 { 1 } elif x < 0 { 2 }' "(use 'else if')"
check 'let x = nil
if x is nil { 1 }' 'use == to compare'
check 'stop()' "there's no function by that name"
check 'let s = "Skor: " + 3' 'write "...${x}..." or use str(x)'
check 'let a: any = gfx.frame
if a { 1 }' 'did you forget () to call it?'
check 'let m: any = gfx.mouse
print(m.x)' 'did you forget () to call it?'
check 'gfx.circel(1, 2, 3, "red")' "gfx has no function 'circel' (did you mean 'circle'?)"
check 'gfx.circle(1, 2, 3)' 'usage: gfx.circle(x, y, radius, color)'
check 'let xs = [1, 2, 3]
for i in 0..len(xs) { print(xs[i]) }' 'the last item is [2], and a..b includes b'
check 'let skor = 0
while true {
let skor = skor + 1
}' "'skor' already exists; inside a loop, let makes a new one"
check 'fn f() { print(x) }
f()
let x = 1' "'x' is used before its let runs"
check 'fn f() {
print(1)' 'line 1: this { is never closed'
check 'print(1)
}' "unexpected '}': there's one } more than {"
check 'print(max(1, 2)
print(3)' "')' is missing: the bracket opened on this line is never closed"
check 'yaş = 15' "'yaş' doesn't exist yet; to create it write: let yaş = ..."
check 'print(Merhaba)' 'or put it in quotes if you meant text'
check 'let x = 1
if x > 0 { print(1)
else { print(2) }' "close the if block with } before else"
check '```
print(1)' 'leave out the ``` lines'
check 'let ys = [1, 2]
for y in ys { y = y + 4 }' "'y' is the loop variable; changing it doesn't change the list"
check 'write_file("a.txt", 5)' 'str(x) turns a number into text'
check 'read_file("/nonexistent/x.txt")' 'exists(path) checks'
# a global declared further down is fine (not reported as undefined)
printf 'fn later() { early }\nlet early = 1\nprint(later())\n' > "$snippet"
out=$(./lumo "$snippet" 2>&1)
[ "$out" = "1" ] || { echo "FAIL: later-declared global: $out"; exit 1; }
# lumo build: a standalone executable that carries its imports
tmp="${TMPDIR:-/tmp}/lumo_build_test_$$"
mkdir -p "$tmp/lib"
printf 'import "lib/greet.lu"\nprint(greet.hello(args[0]))\n' > "$tmp/main.lu"
printf 'fn hello(n) { "hello ${n}" }\n' > "$tmp/lib/greet.lu"
./lumo build "$tmp/main.lu" -o "$tmp/app" > /dev/null || { echo "FAIL: build"; exit 1; }
rm -r "$tmp/lib" "$tmp/main.lu"
out=$(cd / && "$tmp/app" lumo)
[ "$out" = "hello lumo" ] || { echo "FAIL: built app printed: $out"; exit 1; }
printf 'print(nope)\n' > "$tmp/bad.lu"
./lumo build "$tmp/bad.lu" -o "$tmp/bad" 2> /dev/null && { echo "FAIL: build accepted a broken program"; exit 1; }
rm -rf "$tmp"
# graphics: clear errors without a window, and in builds without graphics
check 'gfx.rect(0, 0, 10, 10, "red")' 'open a window first'
$CC -O2 -o "${TMPDIR:-/tmp}/lumo_plain" lumo.c -lm || exit 1
printf 'gfx.window(10, 10)\n' > "$snippet"
out=$("${TMPDIR:-/tmp}/lumo_plain" "$snippet" 2>&1)
case "$out" in *"built without graphics"*) ;; *) echo "FAIL: plain build gfx: $out"; exit 1 ;; esac
# lumo --fast: typed functions compiled to C must behave exactly like the interpreter
tmp="${TMPDIR:-/tmp}/lumo_fast_test_$$"
mkdir -p "$tmp"
./lumo tests/fast.lu > "$tmp/slow.txt" 2>&1
./lumo build --fast tests/fast.lu -o "$tmp/fast" > "$tmp/build.txt" || { echo "FAIL: build --fast"; cat "$tmp/build.txt"; exit 1; }
grep -q "native:      fib" "$tmp/build.txt" || { echo "FAIL: fib wasn't compiled"; cat "$tmp/build.txt"; exit 1; }
grep -q "interpreted: calls_interp (it calls uses_str)" "$tmp/build.txt" || { echo "FAIL: fallback report"; cat "$tmp/build.txt"; exit 1; }
"$tmp/fast" > "$tmp/fast.txt" 2>&1
diff "$tmp/slow.txt" "$tmp/fast.txt" > /dev/null || { echo "FAIL: --fast output differs:"; diff "$tmp/slow.txt" "$tmp/fast.txt"; exit 1; }
out=$(./lumo --fast tests/fast.lu 2>&1)
[ "$out" = "$(cat "$tmp/slow.txt")" ] || { echo "FAIL: lumo --fast run"; exit 1; }
out=$(printf 'print(1)\n' > "$tmp/x.lu"; "${TMPDIR:-/tmp}/lumo_plain" build --fast "$tmp/x.lu" -o "$tmp/x" 2>&1)
case "$out" in *"needs lumo built with"*) ;; *) echo "FAIL: plain build --fast: $out"; exit 1 ;; esac
rm -rf "$tmp"
echo "errors ok"
