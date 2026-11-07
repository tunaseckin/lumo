# Lumo for VS Code

Syntax highlighting for `.lu` files and a ▶ **Run Lumo File** button in the editor title bar.

Install from this repository (no build step):

```bash
ln -s "$(pwd)/editor/vscode" ~/.vscode/extensions/lumo-lang
```

Then restart VS Code. The Run button calls `lumo`, so put it on your PATH
(`make install`), or set **Settings → Lumo: Path** to the full path of the executable.
