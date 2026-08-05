# Laptop item cell prompts

Generated with the gemini image pipeline (model `gemini-3-pro-image`),
chroma-keyed and fitted into `desktop-items.png` column 7 by
`tools/laptop_item_art.py` (pixel styles reduce with nearest, painted
styles with premultiplied Lanczos). The refreshed atlas hash lives in
`manifest.json`; the cell records live in `PROVENANCE.json`. Prompts
are verbatim.

## legend

```text
Use case: stylized-concept.
Asset type: tiny game item sprite.
Primary request: one open portable laptop computer drawn as a 16-bit-era pixel-art game item with a chunky readable silhouette, a screen glowing warm amber, and a warm brown-grey shell with cream keys.
Style/medium: crisp 16-bit pixel art with large visible pixels and a limited warm palette of browns, amber, and cream; no smooth gradients.
Scene/backdrop: perfectly flat solid #00ff00 chroma-key background; do not use #00ff00 anywhere in the subject.
Composition: a single laptop centered, three-quarter view facing slightly left, filling about 70 percent of the canvas with even padding.
Constraints: one perfectly uniform #00ff00 background with no shadows or texture; crisp hard edges; no cast shadow; exactly one object; no text; no logo; no watermark; no border.
Avoid: photorealism, anti-aliasing, scenery, extra objects.
```

## chumrunner

```text
Use case: stylized-concept.
Asset type: game item sprite.
Primary request: one open portable laptop computer as a hand-painted 2D game item, dark navy shell with cool teal and cyan accent lighting, the screen glowing soft cyan, sleek but friendly rounded shapes.
Style/medium: polished hand-painted 2D game art, soft cel shading, crisp silhouette, cool palette of navy, teal, and cyan with small warm highlights.
Scene/backdrop: perfectly flat solid #00ff00 chroma-key background; do not use #00ff00 anywhere in the subject.
Composition: a single laptop centered, three-quarter view facing slightly left, filling about 70 percent of the canvas with even padding.
Constraints: one perfectly uniform #00ff00 background with no shadows or texture; crisp hard edges; no cast shadow; exactly one object; no text; no logo; no watermark; no border.
Avoid: photorealism, scenery, extra objects.
```

## fantasy

```text
Use case: stylized-concept.
Asset type: tiny game item sprite.
Primary request: one open portable laptop computer drawn as a retro 16-bit console-era pixel-art item with a mossy-green glowing screen and a weathered bronze-and-wood shell, as if a fantasy artificer built a small folding terminal.
Style/medium: tiny-sprite-readable retro pixel art, large visible pixels, limited palette of moss green, bronze, and dark wood; no smooth gradients.
Scene/backdrop: perfectly flat solid #ff00ff chroma-key background; do not use #ff00ff anywhere in the subject.
Composition: a single laptop centered, three-quarter view facing slightly left, filling about 70 percent of the canvas with even padding.
Constraints: one perfectly uniform #ff00ff background with no shadows or texture; crisp hard edges; no cast shadow; exactly one object; no text; no logo; no watermark; no border.
Avoid: photorealism, anti-aliasing, scenery, extra objects.
```

## pleb-bound

```text
Use case: stylized-concept.
Asset type: game item sprite.
Primary request: one open portable laptop computer as a cozy hand-painted storybook game item, warm cream shell with honey-gold accents, the screen glowing gentle warm gold, soft rounded friendly shapes.
Style/medium: warm hand-painted storybook 2D game art, soft painterly shading, crisp silhouette, palette of cream, honey gold, and warm brown.
Scene/backdrop: perfectly flat solid #00ff00 chroma-key background; do not use #00ff00 anywhere in the subject.
Composition: a single laptop centered, three-quarter view facing slightly left, filling about 70 percent of the canvas with even padding.
Constraints: one perfectly uniform #00ff00 background with no shadows or texture; crisp hard edges; no cast shadow; exactly one object; no text; no logo; no watermark; no border.
Avoid: photorealism, scenery, extra objects.
```

# Closed-lid cell prompts

The stateful laptop's CLOSED frame: one cell per style, fitted into the
optional `laptop-lid.png` sheet (32x32 per style row, same row order)
by `tools/laptop_lid_art.py`, which also records `PROVENANCE-LID.json`
beside it. The sheet ships outside `manifest.json` because it is
optional: absent art degrades to the open sprite. Same generator
(model `gemini-3-pro-image`), same chroma-key preparation. Prompts are
verbatim.

## legend (closed)

```text
Use case: stylized-concept. Asset type: tiny game item sprite. Primary request: one CLOSED portable laptop computer drawn as a 16-bit-era pixel-art game item with a chunky readable silhouette, the lid shut flat so only the warm brown-grey shell top, the back hinge, and a thin front seam show, a slim slab shape, warm brown shell with a small cream latch detail. Style/medium: crisp 16-bit pixel art with large visible pixels and a limited warm palette of browns, amber, and cream; no smooth gradients. Scene/backdrop: perfectly flat solid #00ff00 chroma-key background; do not use #00ff00 anywhere in the subject. Composition: a single closed laptop centered, three-quarter view facing slightly left seen from slightly above, filling about 70 percent of the canvas with even padding. Constraints: one perfectly uniform #00ff00 background with no shadows or texture; crisp hard edges; no cast shadow; exactly one object; no text; no logo; no watermark; no border. Avoid: photorealism, anti-aliasing, scenery, extra objects, an open lid, a visible screen.
```

## chumrunner (closed)

```text
Use case: stylized-concept. Asset type: game item sprite. Primary request: one CLOSED portable laptop computer as a hand-painted 2D game item, the lid shut flat so only the dark navy shell top, the back hinge, and a thin front seam show, a sleek slim slab with cool teal and cyan accent edge lighting and one small dim cyan indicator on the front edge, sleek but friendly rounded shapes. Style/medium: polished hand-painted 2D game art, soft cel shading, crisp silhouette, cool palette of navy, teal, and cyan with small warm highlights. Scene/backdrop: perfectly flat solid #00ff00 chroma-key background; do not use #00ff00 anywhere in the subject. Composition: a single closed laptop centered, three-quarter view facing slightly left seen from slightly above, filling about 70 percent of the canvas with even padding. Constraints: one perfectly uniform #00ff00 background with no shadows or texture; crisp hard edges; no cast shadow; exactly one object; no text; no logo; no watermark; no border. Avoid: photorealism, scenery, extra objects, an open lid, a visible screen or keyboard.
```

## fantasy (closed)

```text
Use case: stylized-concept. Asset type: tiny game item sprite. Primary request: one CLOSED portable laptop computer drawn as a retro 16-bit console-era pixel-art item, the lid shut flat so only the weathered bronze-and-wood shell top with riveted bronze corner caps, the back hinge, and a thin front clasp show, a slim slab as if a fantasy artificer folded shut a small terminal. Style/medium: tiny-sprite-readable retro pixel art, large visible pixels, limited palette of moss green, bronze, and dark wood; no smooth gradients. Scene/backdrop: perfectly flat solid #ff00ff chroma-key background; do not use #ff00ff anywhere in the subject. Composition: a single closed laptop centered, three-quarter view facing slightly left seen from slightly above, filling about 70 percent of the canvas with even padding. Constraints: one perfectly uniform #ff00ff background with no shadows or texture; crisp hard edges; no cast shadow; exactly one object; no text; no logo; no watermark; no border. Avoid: photorealism, anti-aliasing, scenery, extra objects, an open lid, a visible glowing screen.
```

## pleb-bound (closed)

```text
Use case: stylized-concept. Asset type: game item sprite. Primary request: one CLOSED portable laptop computer as a cozy hand-painted storybook game item, the lid shut flat so only the warm cream shell top with a honey-gold trim line and a small round honey-gold latch, the back hinge, and a thin front seam show, a soft rounded slim slab, friendly shapes. Style/medium: warm hand-painted storybook 2D game art, soft painterly shading, crisp silhouette, palette of cream, honey gold, and warm brown. Scene/backdrop: perfectly flat solid #00ff00 chroma-key background; do not use #00ff00 anywhere in the subject. Composition: a single closed laptop centered, three-quarter view facing slightly left seen from slightly above, filling about 70 percent of the canvas with even padding. Constraints: one perfectly uniform #00ff00 background with no shadows or texture; crisp hard edges; no cast shadow; exactly one object; no text; no logo; no watermark; no border. Avoid: photorealism, scenery, extra objects, an open lid, a visible screen or keyboard.
```
