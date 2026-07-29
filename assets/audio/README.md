# Cast audio cues (parity-managed)

Every WAV here is a byte-identical copy of what its source game has
committed — read from that repository's git history (`git show HEAD:`),
never from its working tree. Do not edit these files by hand; they are
maintained by `tools/sync_source_parity.py` (`make parity-sync` to refresh,
`make parity-check` to verify).

Each cast directory carries the three desktop UI cues:

| Directory | Source game | Cues |
| --- | --- | --- |
| legend/ | Legend of Kilix | ui-move.wav, ui-confirm.wav, dialogue.wav |
| chumrunner/ | Chumrunner | ui-move.wav, ui-confirm.wav, dialogue.wav |
| fantasy/ | Kilix Fantasy | ui-move.wav, ui-confirm.wav, dialogue.wav |
| pleb-bound/ | Pleb Bound | ui-move.wav, ui-confirm.wav, dialogue.wav |

kilix-land substitutes a committed cue for the uncommitted `enemy_attack`
cues in Kilix Fantasy and Pleb Bound; the desktop does not take those cues,
so every file here is a direct committed copy with no substitution.
