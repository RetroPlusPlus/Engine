; ship_destruct — the dreadnought scuttles itself. The longest cue in the set: a deep noise
; rumble on CH4, low rough clock, the slowest envelope — it decays over the whole destruct
; sequence rather than snapping. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $00
ldh [rNR41], a    ; CH4 length (unused — envelope ends the sound)
ld a, $C7
ldh [rNR42], a    ; envelope: initial volume 12, decrease, pace 7 (the long fade)
ld a, $77
ldh [rNR43], a    ; noise clock: low + rough — a deep rumble
ld a, $80
ldh [rNR44], a    ; trigger
.idle:
jr .idle
