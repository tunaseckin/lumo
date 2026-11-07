# Contributing to Lumo

Thanks for trying Lumo! It's young, so every report helps.

## The most useful things you can do

- **Tell us where you got stuck.** Followed [the getting-started guide](docs/getting-started.md) and something was confusing? That's a bug in the guide or the language. Open a "Feedback" issue.
- **Report confusing error messages.** Lumo's errors are meant to tell beginners what to do. If one didn't, open a "Bug" issue with the code and the message.
- **Write an example.** A small game or tool in `examples/` shows what Lumo is for.

## Working on the code

The whole language is one C file, [lumo.c](lumo.c), plus [gfx_macos.m](gfx_macos.m) for graphics on macOS.
[docs/nasil-calisir.md](docs/nasil-calisir.md) explains how it fits together (in Turkish for now).

```bash
make          # build ./lumo
make test     # language tests, error-message checks, AddressSanitizer, --fast equivalence
```

Please keep `make test` passing and add a test for what you change:

- language behaviour: an `assert(...)` in [tests/test.lu](tests/test.lu)
- error messages: a `check 'code' 'expected message'` line in [tests/run.sh](tests/run.sh)
- `--fast`: a function in [tests/fast.lu](tests/fast.lu) (its output must match the interpreter's)

Match the style around you: short functions, a comment where the *why* isn't obvious, error messages that say what to do next.

## Good first issues

- **Graphics on Linux and Windows.** `gfx_macos.m` shows the 15 functions a backend needs; SDL or raylib would work.
- **Unicode `upper`/`lower`.** They only change ASCII letters today, so `upper("ğ")` stays `"ğ"`.
- **Error messages in other languages**, starting with Turkish.
- **`--fast` for strings and structs.** Today only numbers, bools and lists of numbers compile to C.
- **A Homebrew formula** and an install script.
- **More examples:** a snake game, a text adventure, a JSON tool.

## Ground rules

Be kind; many people here are learning to program. Assume good intent, explain instead of correcting, and remember that a confusing error message is Lumo's fault, not the user's.
