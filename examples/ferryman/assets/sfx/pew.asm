; pew — a passenger fires the gold return bolt. THE most frequent cue in the game, so it is a
; QUIET pew: a fast downward pitch sweep on pulse CH1 (the classic zap shape, kept small — volume
; 6, snappy envelope) so a full deck's volley reads as crackle, not a siren. Clearly distinct
; from the abductor klaxon (two slow honks) and the bank sweep (long and rising). Self-contained:
; enables the APU and sets panning/volume each play, so any SFX can be the first one cued.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $1B
ldh [rNR10], a    ; CH1 sweep: pace 1, subtraction (pitch falls), shift 3 — the steep zap
ld a, $40
ldh [rNR11], a    ; CH1 duty 25% (thin, laser-ish)
ld a, $61
ldh [rNR12], a    ; envelope: initial volume 6, decrease, pace 1 (small and gone)
ld a, $9B
ldh [rNR13], a    ; frequency low  — period $79B → ~1300 Hz start
ld a, $87
ldh [rNR14], a    ; trigger + frequency high
.idle:
jr .idle
