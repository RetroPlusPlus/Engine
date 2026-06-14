; dualSeedRng — a general-purpose hardware-entropy RNG: folds the free-running divider (rDIV) into a
; dual, carry-chained HRAM seed ($FFE1 / $FFE2) and returns a byte in A. No inputs; the seed persists
; across calls, so the stream mixes well even when rDIV is momentarily steady.
;
; NOTE: this is an implementation of the disassembled Pokemon Gen 1/2 `_Random` algorithm
; (hRandomAdd / hRandomSub). This is the engine's OWN assembly of the publicly-documented routine; no
; copyrighted game code is included. The name is mechanism-descriptive (dual carry-chained seed)
; rather than title-specific. The seed cells $FFE1 / $FFE2 are routine-local HRAM (not hardware-
; register names), so they are written as raw addresses.
ldh a, [$FFE1]   ; hRandomAdd seed
ld  b, a
ldh a, [rDIV]
adc b
ldh [$FFE1], a
ldh a, [$FFE2]   ; hRandomSub seed
ld  b, a
ldh a, [rDIV]
sbc b
ldh [$FFE2], a
ret
