# Security

Lumo is a young hobby language. Its runtime is written in C, so bugs can be memory-safety bugs.

**Lumo is not a sandbox.** Lumo programs can read and write files and run other programs, so treat a `.lu` file like a shell script: only run ones you trust.

## Reporting a problem

Please don't open a public issue for a security bug. Report it privately through GitHub's
**Security → Report a vulnerability** form on this repository. Include a small `.lu` file or input that shows the problem if you can.

Most useful to hear about:

- crashes or memory errors in the interpreter, the JSON parser or `lumo build` executables, especially from untrusted input such as a JSON file
- anything in `lumo build --fast` or `run([...])` that lets input reach a shell
