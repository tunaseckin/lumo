# Getting started with Lumo

This guide is for someone who has never written code. By the end you'll have made a game where you catch falling stars.
Type each step yourself and run it; typing teaches more than copy-and-paste.

Time: about 20–30 minutes. (Türkçe: [baslangic.md](baslangic.md))

---

## 0. Setup

Lumo installs with one command.

**Windows:** open **PowerShell** from the Start menu, paste this and press Enter:

```powershell
irm https://raw.githubusercontent.com/tunaseckin/lumo/main/install.ps1 | iex
```

**Mac and Linux:** open **Terminal**, paste this and press Enter:

```bash
curl -fsSL https://raw.githubusercontent.com/tunaseckin/lumo/main/install.sh | sh
```

When it's done, close and reopen the terminal and type:

```bash
lumo --version
```

It should print `lumo 0.2.0` (or newer). The example programs and this guide are installed too: in `%LOCALAPPDATA%\Lumo` on Windows, `~/.lumo` on Mac and Linux.

**Editor:** use VS Code to write code. With the Lumo extension, your code is colored and you get a ▶ button.
In VS Code click the **Extensions** icon on the left, open the `···` menu at the top, choose **Install from VSIX...**
and pick `lumo-lang.vsix`, downloaded from the [Releases](https://github.com/tunaseckin/lumo/releases) page.

Then use `File → Open Folder` to open a folder of your own (for example `lumo-lessons` on your Desktop).
Create a new file in it whose name ends in `.lu`, such as `hello.lu`.
After writing code, press the **▶** button at the top right, or type `lumo hello.lu` in a terminal in that folder.

---

## 1. Your first program

```
print("Hello, world!")
```

Run it. It should print `Hello, world!`. Congratulations, that's your first program!

`print(...)` shows whatever is inside the parentheses. Things in double quotes are **text** (strings).

---

## 2. Variables: giving names to values

To keep a value for later, give it a name:

```
let name = "Ada"
let age = 15
print(name)
print(age + 1)
```

- `let` creates a new variable.
- To change it, use `=` without `let`: `age = age + 1`
- To put a value inside text, write `${...}`:

```
let name = "Ada"
let age = 15
print("${name} will be ${age + 1} next year")
```

---

## 3. Making decisions: `if`

```
let score = 72

if score >= 50 {
    print("You passed!")
} else {
    print("Not this time.")
}
```

Curly braces `{ }` make a **block**: its contents run when the condition is true.
Comparisons: `==` (equal?), `!=` (different?), `<`, `>`, `<=`, `>=`. Combine them with `and`, `or`, `not`.

> Careful: `=` assigns, `==` compares. If you write `if score = 50`, Lumo will tell you.
> Some words belong to Lumo itself and can't be variable names: `let`, `if`, `else`, `for`, `while`, `fn`, `not`, `and`, `or`, `true`, `false`, `nil` and a few more.

---

## 4. Repeating: `for` and `while`

```
for number in 1..5 {
    print(number)
}
```

This runs the block for every number from 1 to 5. Each time around, `number` holds the next value.

It works with a list too:

```
let fruits = ["apple", "pear", "banana"]
for fruit in fruits {
    print("I like ${fruit}")
}
```

`while` repeats as long as a condition is true:

```
let count = 3
while count > 0 {
    print(count)
    count = count - 1
}
print("Liftoff!")
```

---

## 5. Functions: making your own commands

```
fn greet(who) {
    print("Hi ${who}!")
}

greet("Sam")
greet("Mia")
```

A function can also give back a result. The last line of its block is the result:

```
fn square(x) {
    x * x
}
print(square(9))   # 81
```

Everything after `#` is a **comment**. The computer ignores it; it's a note for you.

---

## 6. Open a window

Now for something visual. Open a new file and write:

```
gfx.window(800, 600, "My first window")

while gfx.frame() {
    gfx.clear("navy")
    gfx.circle(400, 300, 50, "yellow")
}
```

You should see a yellow circle in a navy window.

- `gfx.window(width, height, title)` opens the window.
- `while gfx.frame() { ... }` runs 60 times a second. This is the **game loop**: each time around, we draw the screen from scratch.
- Coordinates start at the top-left corner: `x` grows to the right, `y` grows downwards.

Closing the window ends the program.

---

## 7. Make it move

Keep the circle's position in a variable and change it with the arrow keys:

```
gfx.window(800, 600, "Moving")
let x = 400

while gfx.frame() {
    if gfx.key("left") { x = x - 5 }
    if gfx.key("right") { x = x + 5 }

    gfx.clear("navy")
    gfx.circle(x, 300, 50, "yellow")
}
```

Press ← and →: the circle moves! `gfx.key("left")` is `true` while the left arrow is held down.

---

## 8. The game: Catch the Star

Now we put everything together: a star falls from the top and you catch it with a basket at the bottom.
After each step you get the **whole** program; new lines are marked `# new`. Type it, run it, then move on.

**Step 1: a falling star.** Keep the star's position in two variables and move it down a little every frame.
When it falls off the bottom, start it again at the top, somewhere random:

```
gfx.window(800, 600, "Catch the Star")
let star_x = 400
let star_y = 0

while gfx.frame() {
    star_y = star_y + 4

    if star_y > 600 {
        star_y = 0
        star_x = random() * 760 + 20
    }

    gfx.clear("navy")
    gfx.circle(star_x, star_y, 15, "yellow")
}
```

`random()` gives a random number between 0 and 1, so `random() * 760 + 20` is somewhere between 20 and 780.

> We create the variables (`let`) **outside** the loop, at the top. A variable made with `let` inside the loop starts over every frame, so the star would never move. (If a variable with the same name exists outside, Lumo notices and warns you.)

**Step 2: the basket.** Add a basket that moves with ← and →, like in step 7.
`gfx.rect(x, y, width, height, color, corner)` draws a rectangle; `corner` is how round the corners are (0 = sharp):

```
gfx.window(800, 600, "Catch the Star")
let star_x = 400
let star_y = 0
let basket_x = 400                                      # new

while gfx.frame() {
    if gfx.key("left") { basket_x = basket_x - 8 }      # new
    if gfx.key("right") { basket_x = basket_x + 8 }     # new

    star_y = star_y + 4
    if star_y > 600 {
        star_y = 0
        star_x = random() * 760 + 20
    }

    gfx.clear("navy")
    gfx.circle(star_x, star_y, 15, "yellow")
    gfx.rect(basket_x - 60, 550, 120, 20, "orange", 8)  # new
}
```

> Drawing order matters: first clear the screen with `gfx.clear`, then draw on top. Anything drawn before `gfx.clear` gets wiped.

**Step 3: catching and scoring.** When the star reaches the basket's height (y between 530 and 560) and it's over the basket, it's caught.
`abs(...)` gives a number without its sign; here, the sideways distance between star and basket:

```
gfx.window(800, 600, "Catch the Star")
let star_x = 400
let star_y = 0
let basket_x = 400
let score = 0                                           # new

while gfx.frame() {
    if gfx.key("left") { basket_x = basket_x - 8 }
    if gfx.key("right") { basket_x = basket_x + 8 }

    star_y = star_y + 4

    if star_y > 530 and star_y < 560 and abs(star_x - basket_x) < 60 {   # new
        score = score + 1                               # new
        star_y = 0                                      # new
        star_x = random() * 760 + 20                    # new
    }                                                   # new

    if star_y > 600 {
        star_y = 0
        star_x = random() * 760 + 20
    }

    gfx.clear("navy")
    gfx.circle(star_x, star_y, 15, "yellow")
    gfx.rect(basket_x - 60, 550, 120, 20, "orange", 8)
    gfx.text("Score: ${score}", 20, 20, 24, "white")    # new
}
```

**Step 4: lives and game over.** Missing a star costs a life. With no lives left, the star stops and "GAME OVER" appears.
We put the falling-and-catching code inside `if lives > 0 { ... }`: once lives run out that part stops running, so the game stops but the window stays open.
Pressing space (`gfx.pressed("space")`) starts over:

```
gfx.window(800, 600, "Catch the Star")
let star_x = 400
let star_y = 0
let basket_x = 400
let score = 0
let lives = 3                                           # new

while gfx.frame() {
    if gfx.key("left") { basket_x = basket_x - 8 }
    if gfx.key("right") { basket_x = basket_x + 8 }

    if lives > 0 {                                      # new
        star_y = star_y + 4

        if star_y > 530 and star_y < 560 and abs(star_x - basket_x) < 60 {
            score = score + 1
            star_y = 0
            star_x = random() * 760 + 20
        }

        if star_y > 600 {
            lives = lives - 1                           # new
            star_y = 0
            star_x = random() * 760 + 20
        }
    } else if gfx.pressed("space") {                    # new: play again
        score = 0
        lives = 3
    }

    gfx.clear("navy")
    gfx.circle(star_x, star_y, 15, "yellow")
    gfx.rect(basket_x - 60, 550, 120, 20, "orange", 8)
    gfx.text("Score: ${score}   Lives: ${lives}", 20, 20, 24, "white")
    if lives == 0 {                                     # new
        gfx.text("GAME OVER", 285, 240, 48, "red")
        gfx.text("press space to play again", 270, 310, 20, "white")
    }
}
```

- `gfx.key(...)` is `true` **while** a key is held; `gfx.pressed(...)` is `true` only at the moment it's pressed, once.
- `else if` tries a second condition when the first one wasn't true.

Your game is done! The same program is in [catch-the-star.lu](catch-the-star.lu).

**Polish it:** add `gfx.sound("Pop")` when you catch a star and `gfx.sound("Basso")` when you miss, and make the star a bit faster with every catch.
Hint: use a `speed` variable instead of `4`, and increase it when you catch.

---

## 9. Send it to a friend

Turn your game into a single file (run this in a terminal in the folder with your game):

```bash
lumo build mygame.lu
```

You can send the resulting file (`mygame.exe` on Windows, `mygame` on a Mac) to someone on the same system; it runs even without Lumo installed.
It isn't code-signed, so there may be a warning the first time: on Windows choose **More info → Run anyway**, on a Mac right-click the file and choose **Open**.

---

## If you get an error

Lumo tries to explain errors and shows the line:

```
error: line 3: 'scroe' is not defined (did you mean 'score'?)
     3 | print(scroe)
```

Here `score` was typed as `scroe`, and Lumo guesses what you meant.

**If you know another language** (Python, JavaScript...), here's what's different:

| There | In Lumo |
|---|---|
| `x = 5` (first time) | `let x = 5` |
| `x++` | `x += 1` |
| `if x:` + indentation | `if x { ... }` |
| `elif` | `else if` |
| `True`, `None`, `null` | `true`, `nil` |
| `f"{x}"`, `` `${x}` `` | `"${x}"` |
| `xs.append(x)`, `s.upper()`, `len(x)` | `push(xs, x)`, `upper(s)`, `len(x)` |
| `xs[1:3]` | `slice(xs, 1, 3)` |
| `a ** b`, `a // b` | `pow(a, b)`, `floor(a / b)` |
| `def f(x):`, `lambda x: ...` | `fn f(x) { ... }`, `fn(x) { ... }` |
| `range(1, 6)` | `1..5` (both ends included) |

If you write those anyway, Lumo's error message tells you the Lumo version. All built-in functions are listed in the [README](../README.md#builtins).

---

## What's next?

Improve your game:

1. **Play with the mouse:** `gfx.mouse().x` is the mouse's horizontal position (don't forget the parentheses). Move the basket there: `basket_x = gfx.mouse().x`
2. **Several stars:** keep each star's x and y in two lists: `let xs = [100, 400, 700]` and `let ys = [0, -200, -400]`
   (the x and y at the same position belong to the same star). To handle them all, write `for i in 0..len(ys) - 1 { ... }`;
   inside, move with `ys[i] = ys[i] + 4`, draw with `gfx.circle(xs[i], ys[i], 15, "yellow")`,
   and do the catch and miss checks with `xs[i]` instead of `star_x` and `ys[i]` instead of `star_y`.
   (The last item is at `len - 1` because counting starts at 0.)
3. **High score:** files hold text. Save with `write_file("best.txt", str(score))`
   and read with `num(read_file("best.txt"))`. Check whether the file exists with `exists("best.txt")`.
   When the game ends (`lives == 0`) and the score beats the record, save it, and show the record on screen.
4. **Bonus star:** sometimes (when `random() < 0.2`) drop a red star worth 5 points.

More: the [README](../README.md) and the [examples](../examples), such as the [bricks.lu](../examples/bricks.lu) brick-breaking game.
