# Lumo

A small, readable programming language with no surprises. One C file, no dependencies, about as fast as Python, and much faster with `--fast`.

<p align="center">
  <img src="docs/bricks.gif" width="560" alt="A brick-breaking game written in Lumo: a ball bounces off a paddle and breaks rows of colored bricks">
  <br><sub><a href="examples/bricks.lu">examples/bricks.lu</a>: a complete game in about 200 lines of Lumo</sub>
</p>

```
struct Todo { text: str, done: bool }

fn summary(todos: list) -> str {
    let open = todos |> filter(fn(t) { not t.done }) |> len()
    "${open} of ${len(todos)} left"
}

let todos = [Todo("write docs", false), Todo("ship v1", true)]
print(summary(todos))                 # 1 of 2 left
print(json_encode(todos))             # [{"text":"write docs","done":false},...]
```

- **No surprises:** you declare variables with `let`, conditions must be real bools, and there are no hidden conversions (`1 + "a"` is an error).
- **Optional types**, checked before the program runs: `fn area(r: num) -> num`. Code without types keeps working.
- **Everything is an expression:** `if`, `try` and blocks all return values.
- **Batteries:** structs, maps, closures, modules, `try`/`catch`, `${}` string interpolation, JSON, files, time and shell commands.
- **Graphics built in:** open a window and make a game in a few lines with `gfx`, on macOS, Windows and Linux.
- **Friendly errors** that show the line and suggest fixes: `'nmae' is not defined (did you mean 'name'?)`.
- **Share what you make:** `lumo build game.lu` gives you one file that runs without Lumo installed.
- **Fast when you need it:** `lumo --fast` compiles typed numeric functions to machine code: 1–3× Rust's time on the benchmarks below, 8–30× faster than Python.
- **Small:** the whole implementation is one readable C file (scanner, compiler, VM, GC).

## Get started

**Install** (macOS on Apple Silicon, Linux x64; elsewhere it builds from source):

```bash
curl -fsSL https://raw.githubusercontent.com/tunaseckin/lumo/main/install.sh | sh
```

**Windows** (in PowerShell):

```powershell
irm https://raw.githubusercontent.com/tunaseckin/lumo/main/install.ps1 | iex
```

Or download a build from [Releases](https://github.com/tunaseckin/lumo/releases) (it also has `lumo-lang.vsix` for VS Code), or build it yourself (needs a C compiler; on Windows, MSYS2 with `mingw-w64-ucrt-x86_64-gcc`):

```bash
make                         # build ./lumo (needs a C compiler)
./lumo examples/tour.lu      # run a file
./lumo                       # interactive REPL (ctrl-d to quit)
make test                    # run all tests
make install PREFIX=~/.local # put `lumo` on your PATH
lumo build examples/life.lu  # make ./life, a standalone program you can send to anyone
```

Scripts can start with `#!/usr/bin/env lumo` and be run directly.

**New to programming?** Start with the step-by-step guide: [docs/getting-started.md](docs/getting-started.md) (Türkçe: [docs/baslangic.md](docs/baslangic.md)). It ends with a game you made yourself.

**Editor:** [editor/vscode](editor/vscode) adds syntax highlighting and a ▶ Run button to VS Code and Cursor.

**Examples:** [bricks (a graphical game)](examples/bricks.lu), [tour](examples/tour.lu), [fizzbuzz](examples/fizzbuzz.lu), [todo app with JSON storage](examples/todo.lu), [word count](examples/wordcount.lu), [game of life](examples/life.lu), [number guessing](examples/guess.lu).

## Graphics

```
gfx.window(800, 600, "My game")
let x = 400
while gfx.frame() {                      # 60 times a second, until the window closes
    if gfx.key("left") { x = x - 5 }
    if gfx.key("right") { x = x + 5 }
    gfx.clear("navy")
    gfx.circle(x, 300, 30, "yellow")
    gfx.text("use the arrow keys", 20, 20, 24, "white")
}
```

| Function | |
|---|---|
| `gfx.window(w, h, title?)` | open the window |
| `gfx.frame()` | show what you drew, wait for the next frame, read input; `false` when closed |
| `gfx.clear(color)` | fill the window |
| `gfx.rect(x, y, w, h, color, radius?)` | rectangle, optionally with rounded corners |
| `gfx.circle(x, y, r, color)` · `gfx.line(x1, y1, x2, y2, color, width?)` | shapes |
| `gfx.text(value, x, y, size?, color?)` · `gfx.text_width(value, size?)` | text |
| `gfx.key(name)` · `gfx.pressed(name)` | key held down / pressed this frame: `"left"`, `"space"`, `"a"`, ... |
| `gfx.mouse()` | `{x, y, down, clicked}` |
| `gfx.delta()` | seconds since the last frame, for smooth movement |
| `gfx.sound(name)` | play a system sound: `"Pop"`, `"Tink"`, `"Glass"`, `"Hero"`, `"Basso"`, ... |
| `gfx.pixels(buf, w, h, palette?)` | draw a w×h image over the whole window; `buf` holds `0xRRGGBB` colors or indexes into a 256-color palette |
| `gfx.screenshot(path)` | save the frame as PNG |

To record a game, run it with `LUMO_RECORD=some/folder`: every second frame is saved as a PNG (turn them into a GIF with ffmpeg).

Colors are names (`"red"`, `"orange"`, `"navy"`, ...), `"#rrggbb"`/`"#rrggbbaa"`, or `[r, g, b]`/`[r, g, b, a]` with parts from 0 to 255. Coordinates are in points with (0, 0) at the top left. Games packaged with `lumo build` run on any computer with the same system.

Graphics use only the system's own libraries: Cocoa on macOS, Win32 on Windows, X11 on Linux (loaded when a window opens, so `lumo` still runs on servers). On Windows and Linux Lumo draws text with its built-in font (Poppins Bold, [OFL](docs/fonts/OFL.txt)); it covers English and Turkish letters and common Western European ones. Sounds are the macOS system sound names; Windows and Linux play their own alert sounds for them.

## Errors

Mistakes are caught before the program runs whenever possible, and every error shows its line:

```
error: line 2: 'nmae' is not defined (did you mean 'name'?)
     2 | print(nmae)
```

Habits from other languages get a direct hint, for example `null` → use `nil`, `elif` → use `else if`, `def`/`function` → `fn`, `&&` → `and`, `//` → `#`, `'text'` → `"text"`, `if x = 1` → use `==`, `x: int` → use `num`.

## Speed: `lumo --fast`

Normally Lumo is interpreted, about as fast as Python. Add types to a function and `--fast` turns it into machine code:

```
fn fib(n: num) -> num { if n < 2 { n } else { fib(n - 1) + fib(n - 2) } }
```

```bash
lumo --fast examples/bench.lu             # compile typed functions, then run
lumo build --fast examples/bench.lu       # same, as a standalone executable
```

`lumo build --fast` lists which functions became native and why the others didn't. A function is compiled when its parameters are `num`, `bool` or `list[num]`, it returns `num` or `bool`, and its body sticks to numbers, bools, lists of numbers, `if`/`while`/`for`, math builtins and other compiled functions. Everything else keeps running interpreted, so a program never has to change to use `--fast`. Compiled code keeps Lumo's rules: division by zero, index out of range and wrong argument types are still errors with the same messages.

[examples/bench.lu](examples/bench.lu), one run each on an Apple-silicon MacBook Air (milliseconds, lower is better; C with `cc -O2`, Rust with `rustc -O`):

| | Python 3.14 | Lumo | **Lumo --fast** | Rust | C |
|---|---|---|---|---|---|
| fib(32) | 120 | 190 | **14** | 7 | 4 |
| primes below 300 000 | 255 | 444 | **16** | 15 | 9 |
| sieve of 5 000 000 | 473 | 309 | **36** | 13 | 10 |

These are small numeric benchmarks, which is exactly the kind of code `--fast` handles; code with strings, maps or structs still runs interpreted at the normal speed. `--fast` needs a C compiler (`cc`) and a `lumo` built with `make`. Set `LUMO_KEEP_C=out.c` to see the generated C.

## Sharing programs

`lumo build app.lu` writes `app`: a copy of the Lumo runtime with your program and every file it imports packed inside. It runs on machines without Lumo, gets all command-line arguments in `args`, and refuses to build if the program has errors. On Windows the file is `app.exe`. (macOS and Windows may warn about an unsigned download, as they do for any app from the internet.)

## Rules

- Declare with `let`, then assign with `=`. A typo is an error, never a new variable.
- Everything is an expression. A block's value is its last line.
- `if`, `while`, `and`, `or`, `not` need real bools. `if 1 {}` is an error.
- No implicit conversions. `1 + "a"` is an error; use `str(1) + "a"`.
- `x |> f(y)` means `f(x, y)`.
- Types are optional. Write them where you want them checked; everything else is `any`.
- Newlines end statements. Lines starting with `|>`, `else`, `catch`, `and` or `or` continue the previous one.

## Syntax

```
let x = 5                       # num (64-bit float), str, bool, nil, list, map, fn
x = x + 1                       # or x += 1 (also -=, *=, /=, %=)
let xs = [1, 2, 3]
let m = {name: "lumo", "k": 1}  # m.name == m["name"]

fn add(a, b) { a + b }          # last expression is returned
let double = fn(x) { x * 2 }
return value                    # early return

if a > b { "a" } else if a == b { "eq" } else { "b" }
while cond { ... }              # break / continue work
for x in xs { ... }             # lists, strings (by character), maps (by key)
for i in 1..5 { ... }           # 1, 2, 3, 4, 5  (a..b includes both ends; empty if b < a)

import "lib/geo.lu"             # binds `geo`: geo.area(2), geo.pi
import "lib/geo.lu" as g        # pick the name yourself

"hi ${name}, 1 + 1 = ${1 + 1}"  # interpolation; write \${ for a literal ${

let r = try { risky() } catch e { "failed: ${e}" }   # try is an expression too
error("anything")               # throws any value, e.g. error({code: 404})
```

Each file has its own globals. Importing a file runs it once and gives you its top-level names as a map. Paths are relative to the importing file, and importing the same file again returns the same module.

Runtime errors are caught as message strings (`"can't do num + str"`). An uncaught error prints its line and a traceback.

Operators: `+ - * / %` (floored `%`), `== != < <= > >=` (deep `==` for lists and maps), `and or not`, and bitwise `& | ^ ~ << >> >>>` on 32-bit integers. Numbers can be written `42`, `3.14`, `1e9` or `0xFF`.

## Types

```
fn area(r: num) -> num { 3.14 * r * r }
let names: list[str] = ["a", "b"]
let scores: map[str, num] = {ada: 3}
fn apply(f: fn(num) -> num, x: num) -> num { f(x) }
```

Types: `num` `str` `bool` `nil` `any` `list[T]` `map[K, V]` `fn(A, B) -> R`, plus your structs. Bare `list` and `map` mean `list[any]` and `map[any, any]`.

- **Checked before the program runs:** argument types and counts, return types, assignments, operators (`1 + "a"`), conditions (`if 5`), indexing, and calling something that isn't a function.
- **Inference:** `let x = 5` makes `x` a `num`, so `x = "a"` is an error. `let x = nil` leaves the type open. Literals with mixed items (`[1, "a"]`) are `list[any]`, but they don't fit `list[num]`.
- **`any` fits everywhere.** Annotated parameters, annotated `let`s and return values are also checked at runtime, so a wrong value that slipped in through `any` stops right at the boundary: `r should be num, got str`.
- **Not checked:** item types inside lists and maps at runtime (only the outer kind), module members (`geo.area` is `any`), and functions used before they are defined.

## Structs

```
struct Point { x: num, y: num }
let p = Point(1, 2)        # fields in order
p.x = 10
p.z                        # compile error: Point has no field 'z'
fn norm2(q: Point) -> num { q.x * q.x + q.y * q.y }

struct Node {              # fields may be untyped, one per line
    value
    next
}
```

A struct is a map with fixed fields, so `keys`, `has`, `for k in p`, `==` and `json_encode` work on it. You can't add or remove fields, and it prints as `Point(x: 10, y: 2)`. `type(p)` is `"Point"`. A struct goes anywhere a `map` is expected, but a plain map isn't a `Point`.

## Builtins

| Group | Functions |
|---|---|
| I/O | `print(...)` `input(prompt?)` `args` |
| Files | `read_file(path)` `write_file(path, s)` `append_file(path, s)` `exists(path)` `list_dir(path)` `read_bytes(path)` → list of 0-255 `bytes_str(bytes, start, len)` |
| System | `run([prog, args...])` or `run("shell command")` → `{output, code}` `env(name)` `exit(code?)` |
| Time | `now()` (seconds) `sleep(s)` `format_time(t, fmt?)` `clock()` (CPU seconds) |
| JSON | `json_encode(v)` `json_decode(s)` |
| Lists | `len` `push` `pop` `insert(xs, i, x)` `slice(x, a, b?)` `sort` `reverse` `contains` `index_of` `join(xs, sep)` |
| Strings | `len` `slice` `split(s, sep)` `replace(s, a, b)` `upper` `lower` `trim` `contains` `starts_with` `ends_with` `find` `chars` `repeat(s, n)` `reverse` `ord` `chr` |
| Maps | `keys` `has(m, k)` `get(m, k, default)` `remove(m, k)` |
| Convert | `str(x)` `num(s)` `type(x)` |
| Math | `floor` `ceil` `round` `trunc` `sqrt` `abs` `min` `max` `pow` `log` `sin` `cos` `pi` `random()` `range(a, b?, step?)` |
| Functional (written in Lumo) | `map(xs, f)` `filter(xs, f)` `reduce(xs, start, f)` `sum(xs)` `any(xs, f)` `all(xs, f)` |
| Errors | `assert(cond, msg?)` `error(value)` |

Strings are UTF-8: `len`, indexing, `slice`, `find` and `for` count characters, not bytes.

## Safety

Lumo is not a sandbox. A Lumo program can read and write your files and run other programs, like a Python or shell script can, so only run programs you trust.

`run("...")` passes its text to the shell. Never build that text from input you don't control; use the list form, `run(["convert", file, "out.png"])`, which starts the program directly without a shell. Executables made by `lumo build` contain the program's source in readable form and are not code-signed.

To report a security problem, see [SECURITY.md](SECURITY.md).

## How it was made

Lumo was designed and written with heavy help from an AI assistant (Claude). The tests in `tests/` (language behaviour, error messages, memory checks with AddressSanitizer, and `--fast` giving the same output as the interpreter) are what keep it honest.

## Project layout

```
lumo.c            the whole language
gfx_macos.m       the graphics backend for macOS
gfx.c             the graphics backend for Windows and Linux (gfx_font.h: its font, made by tools/bake_font.py)
tests/            test.lu (language tests), fast.lu (--fast equivalence) and run.sh (runs everything)
docs/             beginner guides (English, Turkish) and how the implementation works
examples/         example programs
editor/vscode/    VS Code / Cursor extension
doom/             Doom on Lumo (work in progress, see doom/README.md)
prototype/        the first Python version, kept for reference
```

## Not in v1

- generics (`map`, `filter`, `push` return `any`)
- methods on structs (use plain functions and `|>`: `p |> norm2()`)
- naming another module's struct as a type (`geo.Point` is `any`)
- private module names (everything at a module's top level is visible to importers)
- images/sprites, 3D
- HTTP, regex, concurrency
- Unicode `upper`/`lower` (ASCII only)

## Contributing

Found something confusing? That's exactly what we want to hear: open an issue. To work on the code, see [CONTRIBUTING.md](CONTRIBUTING.md); [docs/nasil-calisir.md](docs/nasil-calisir.md) explains how the implementation fits together.

## License

MIT, see [LICENSE](LICENSE).
