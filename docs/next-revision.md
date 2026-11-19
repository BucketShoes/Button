# Capture button — next-revision list (from first articles, b43 / case v43)

Parts arrived 2026-11-18. Not yet powered; WS2812s and battery not fitted.

## Cost reference (5 sets)
- Total A$193 + A$34 shipping: lid, shell, chimney extension (sleeve), PCB, PCBA. Still to add: WS2812s, battery holder, cells.
- PCB A$11.63 (special offer).
- Economic PCBA A$123.48: setup 11.97, stencil 2.25, components (23 lines) 69.71, extended components fee 26.94, SMT assembly 3.16, hand-soldering labour 5.25, manual assembly 2.88, nitrogen reflow 1.32.
- So prints ≈ A$58 for 5 sets (≈ A$11.60/set). All-in ≈ A$45/unit before LEDs/cell.
- Fixed-per-order items (setup, stencil, extended fees, shipping) are why 2 units cost nearly as much as 5.

## What worked
- Overall wall thickness: doesn't feel like it bends. Outer catches strong.
- White shell resin is much more rigid than feared; PCB twist-lock works and latches.
- Endstops feel fairly solid.
- Displays legible through the lid even though the lid is more diffuse than wanted.
- Speaker chimney holes printed (just), see below.

## Lid / shell fit
- Lid sliding fit tighter: smaller ring tolerance, and a longer sliding section at the start of rest.
- Outer-wall notches longer still (lid nearly falls off; may improve with the tighter fit).
- Keying: 3 unevenly spaced notches is hard to orient by feel. Make one catch/slot distinctly different (e.g. 2 catches with a pair of slots + 1 single, or one wider), so not every catch tries every slot.
- Rounded corners on the outer top rim (square rim may also be why it looks big).

## Feel / springs
- Return spring and pushers much weaker than expected; easy to drive to the endstop. Shift most of the press force onto the return spring.
- Return spring feels flimsy in every direction; resin feels viscoelastic (slow recovery within ~1 s).
- The two long thin arms bend more than the thick single arm and let the turnaround rotate; that rotation seems to be most of the movement (design aimed roughly 50/50).
- Field-trial idea on existing units: glue the turnaround to the lid (with the lid assembled, so the existing preload is kept and only the rate rises), or glue the two arms mid-way.
- Switch click is audible but barely tactile.

## Rattle / sound
- Clack and rattle are at initial engagement and when moving the lid around, not at the endstops: lid side play and rotation hitting the slots/notches/top rest. Sounds like an empty thin-walled box.
- Helps: tighter/smaller lid fit; something around the rim to damp and contain internal sounds; PCB lock friction (try Blu-tack in the rotation notch slot on existing units).

## PCB retention
- PCB notches not 180° apart, so it only installs one way.
- Twist-lock has no friction: tight sliding fit at least on a thin section (taper in, tightest at the end of the lock notch).
- Anti-rotation pin: longer, so it's impossible to clip the lid closed unless the pin is through the notch (enough tolerance for it to go through). Self-aligning/camming pin is not viable: the locking notch is in the way of a misaligned PCB. Rely on twist-lock friction to keep the PCB in place.
- Silkscreen arrows saying which way to twist.
- Reapply the v42 pin notes that were rolled back for fab (thickness, PCB clearance at corners, directional taper, finish taper before the shelf).

## Holes / small features
- Screw holes too big for the endstop pillar tips (pad almost drops in): flare the pillar tip and/or smaller holes.
- Bigger USB plug opening.
- Speaker chimney holes are below print tolerance (barely printed, debris around them): enlarge.
- Bigger diffuser bumps over the LEDs.

## Lid optics
- TEAM1/TEAM2 and URL silkscreen not legible through the untreated lid; counters fine. Trying buffing one lid.

## Size
- Would like it smaller; unsure what to take the size from.

## Carried over from earlier notes
- Fix print-file growth from the shelf polygon rebuild (v41+); v39 shell was ~180 faces.
- Export A−B / B−A volume diffs vs previous version.

## Open questions
- Two holes near the NE switch: which is the pillar landing (H1), what is the other? Needs the b43 board file.
- Battery holder: a replacement must fit the existing footprint, including any locating-peg hole. LCSC ~A$45 shipping to AU; look for the same MPN elsewhere, or use a pouch cell taped in and wired.
