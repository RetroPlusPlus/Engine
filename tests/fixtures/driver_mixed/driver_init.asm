; The PATH half of a mixed hosted-driver binding: the driver's one-time setup, declared as an Embed
; `DriverImagePath` so the build assembles it into the routine registry. Its byte-image sibling — the
; per-tick routine — is assembled in-process and handed over as a span, so one binding carries both
; sources at once.
;
; Enable the APU, load a 32-step wave into wave RAM, route and level both channels, and switch CH3's DAC
; on. It does NOT trigger a tone: the tick decides that, which is how a hosted-but-unplayed driver stays
; silent until the first play.
ld hl, $C010          ; zero the driver's state RAM ($C010..$C030) — post-reset WRAM is not zero, and a
ld c, $21             ; garbage mailbox would read as a spurious play on the very first tick
xor a
.clr:
ld [hl+], a
dec c
jr nz, .clr
ld a, $80
ldh [$FF26], a        ; NR52 — APU master enable
ld a, $00
ldh [$FF1A], a        ; NR30 — CH3 DAC off while wave RAM is written
ld a, $01
ldh [$FF30], a
ld a, $23
ldh [$FF31], a
ld a, $45
ldh [$FF32], a
ld a, $67
ldh [$FF33], a
ld a, $89
ldh [$FF34], a
ld a, $AB
ldh [$FF35], a
ld a, $CD
ldh [$FF36], a
ld a, $EF
ldh [$FF37], a
ld a, $FE
ldh [$FF38], a
ld a, $DC
ldh [$FF39], a
ld a, $BA
ldh [$FF3A], a
ld a, $98
ldh [$FF3B], a
ld a, $76
ldh [$FF3C], a
ld a, $54
ldh [$FF3D], a
ld a, $32
ldh [$FF3E], a
ld a, $10
ldh [$FF3F], a
ld a, $FF
ldh [$FF25], a        ; NR51 — route all channels L+R
ld a, $77
ldh [$FF24], a        ; NR50 — master volume L+R
ld a, $80
ldh [$FF1A], a        ; NR30 — CH3 DAC on
ld a, $20
ldh [$FF1C], a        ; NR32 — CH3 output level 100%
ret
