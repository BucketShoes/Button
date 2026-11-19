# Capture button — next-revision list (from first articles, b43 / case v43)

Overall, quite happy. If doing another revision, there are some tweaks to improve: mostly a stronger return spring,
a lid that's easier to fit and remove, and tighter tolerances on the sliding parts. For the battery holder, LCSC's
shipping is pricey and the chosen part wasn't available locally, so plan for a local replacement; pouch cells work fine meanwhile.

Parts arrived 2026-11-18. The boards power up and run the firmware; the WS2812s and battery holder aren't fitted yet.

## Cost reference (5 sets)
Rounded to the nearest A$5.

- Total about A$195, plus A$35 shipping: lid, shell, chimney extension (sleeve), PCB and PCBA. Still to add: WS2812s, battery holder, cells.
- PCB about A$10 (a special offer).
- Economic PCBA about A$125: setup and stencil A$15, components (23 lines) A$70, extended-part fees A$25, assembly and hand-soldering A$15.
- So the prints are about A$60 for 5 sets (about A$12 a set). All-in, about A$45 a unit before LEDs and cell.
- Fixed-per-order items (setup, stencil, extended fees, shipping) are why 2 units cost nearly as much as 5.

## What worked
- Overall wall thickness: doesn't feel like it bends. Outer catches strong.
- Materials: shell (and sleeve) in JLC3DP 9600 white resin, lid in 8001 clear resin.
- The 9600 shell is much more rigid than feared, and translucent enough that the LEDs glow out through the sides. The PCB twist-lock works and latches.
- Endstops feel fairly solid.
- The untreated 8001 lid is a little diffuse (no smoothing or polishing), but the large displays read easily through it.
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

## CAD housekeeping (for whoever edits the case model)
- Fix print-file growth from the shelf polygon rebuild (v41+); v39 shell was ~180 faces.
- Export A−B / B−A volume diffs vs previous version.

## Open questions
- Battery holder: a replacement must fit the existing footprint, including its locating-peg hole. LCSC shipping to Australia is about A$45; look for the same part number elsewhere, or keep using a pouch cell taped in and wired to the battery pads.
