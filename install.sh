#!/bin/sh
# Installs the latest Lumo release on macOS (Apple Silicon) or Linux (x64):
#   curl -fsSL https://raw.githubusercontent.com/tunaseckin/lumo/main/install.sh | sh
# Lumo goes to ~/.lumo, the `lumo` command to ~/.local/bin (change with LUMO_HOME / LUMO_BIN).
# Where there's no ready-made build (or it doesn't run), it builds from source if a C compiler is around.
set -e
repo=tunaseckin/lumo
home="${LUMO_HOME:-$HOME/.lumo}"
bin="${LUMO_BIN:-$HOME/.local/bin}"

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) target=macos-arm64 ;;
  Linux-x86_64) target=linux-x64 ;;
  *) target= ;;
esac

tag=$(curl -fsSL "https://api.github.com/repos/$repo/releases/latest" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p')
[ -n "$tag" ] || { echo "error: couldn't find the latest Lumo release (no internet?)"; exit 1; }
if [ "$("$home/lumo" --version 2>/dev/null)" = "lumo ${tag#v}" ]; then
  echo "Lumo ${tag#v} is already installed and up to date ($home)"
  exit 0
fi
if [ -e "$bin/lumo" ] && [ "$(readlink "$bin/lumo")" != "$home/lumo" ]; then
  echo "Note: replacing $bin/lumo (it pointed to $(readlink "$bin/lumo" || echo "another file"))"
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

ok=
if [ -n "$target" ]; then
  echo "Downloading Lumo $tag for $target..."
  curl -fsSL "https://github.com/$repo/releases/download/$tag/lumo-$tag-$target.tar.gz" | tar xz -C "$tmp"
  src="$tmp/lumo-$tag-$target"
  "$src/lumo" --version > /dev/null 2>&1 && ok=1 || echo "The ready-made build doesn't run here; building from source instead."
fi
if [ -z "$ok" ]; then
  command -v cc > /dev/null && command -v make > /dev/null || {
    echo "error: no ready-made Lumo for this computer, and building it needs a C compiler and make"
    [ "$(uname -s)" = Darwin ] && echo "  install them with: xcode-select --install   (then run this again)"
    exit 1
  }
  echo "Building Lumo $tag from source..."
  curl -fsSL "https://github.com/$repo/archive/refs/tags/$tag.tar.gz" | tar xz -C "$tmp"
  src=$(echo "$tmp"/lumo-*)
  command -v xxd > /dev/null || { echo "error: building needs xxd (on Debian/Ubuntu: sudo apt install xxd)"; exit 1; }
  make -s -C "$src" lumo
fi

rm -rf "$home"
mkdir -p "$home" "$bin"
cp -R "$src"/. "$home"/
ln -sf "$home/lumo" "$bin/lumo"
echo "Installed $("$bin/lumo" --version) (examples and guides are in $home)"

case ":$PATH:" in
  *":$bin:"*) echo "Try it: lumo $home/examples/bricks.lu" ;;
  *)
    case "$SHELL" in */zsh) rc="$HOME/.zshrc" ;; *) rc="$HOME/.bashrc" ;; esac
    echo "export PATH=\"$bin:\$PATH\"" >> "$rc"
    echo "Added $bin to your PATH in $rc. Open a new terminal, then try: lumo $home/examples/bricks.lu"
    ;;
esac
