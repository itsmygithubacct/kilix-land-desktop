# Cast atlases (parity-managed)

Every PNG in this directory is a byte-identical copy of what its source game
has committed — read from that repository's git history (`git show HEAD:`),
never from its working tree. Do not edit these files by hand; they are
maintained by `tools/sync_source_parity.py` (`make parity-sync` to refresh,
`make parity-check` to verify). Hashes and grids live in
`assets/graphics/manifest.json`.

Room plates under `assets/graphics/rooms/` are original desktop art, not
parity-managed, and not in the manifest.

## Atlas grids (from the manifest)

| Atlas | Source game | Grid (cols x rows) | Cell | Sheet |
| --- | --- | --- | --- | --- |
| legend-player.png | Legend of Kilix | 16 x 8 | 64 x 64 | 1024 x 512 |
| legend-npcs.png | Legend of Kilix | 4 x 4 | 64 x 64 | 256 x 256 |
| legend-portraits.png | Legend of Kilix | 4 x 3 | 96 x 96 | 384 x 288 |
| chumrunner-characters.png | Chumrunner | 8 x 4 | 222 x 222 | 1776 x 888 |
| chumrunner-portraits.png | Chumrunner | 4 x 2 | 444 x 444 | 1776 x 888 |
| fantasy-characters.png | Kilix Fantasy | 8 x 4 | 222 x 222 | 1776 x 888 |
| fantasy-portraits.png | Kilix Fantasy | 4 x 2 | 444 x 444 | 1776 x 888 |
| pleb-bound-characters.png | Pleb Bound | 8 x 4 | 222 x 222 | 1776 x 888 |
| pleb-bound-portraits.png | Pleb Bound | 4 x 2 | 444 x 444 | 1776 x 888 |
