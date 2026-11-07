# Doom on Lumo

Porting Doom to Lumo, step by step. Everything here is written in Lumo.

```bash
lumo doom/viewer.lu            # title screen; space: level map; ← →: other levels
```

## Status

- [x] Read WAD files: directory, palette, pictures, level geometry ([wad.lu](wad.lu))
- [x] Title screen and level maps ([viewer.lu](viewer.lu))
- [ ] Wall textures and sprites
- [ ] The 3D renderer (BSP walk, wall and floor drawing)
- [ ] Player movement, monsters, weapons

## The WAD file

Doom's maps, pictures and sounds live in a WAD file that is **not** part of this repository
(`*.wad` is git-ignored). The free shareware `doom1.wad` (v1.9, MD5 `f0cefca49926d00903cf57551d901abe`)
is in Debian's `doom-wad-shareware` package. Put it at `doom/doom1.wad`, or pass its path:
`lumo doom/viewer.lu path/to/doom1.wad`.

## License

The parts translated from id Software's Doom source code are covered by its license, the GNU GPL v2,
so this folder is GPL-2.0, unlike the rest of Lumo (MIT). The game data in WAD files belongs to
id Software and is not included.
