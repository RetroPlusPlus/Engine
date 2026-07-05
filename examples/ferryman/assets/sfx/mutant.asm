; mutant — a lost soul returns as a HUNTER, and the beast ANNOUNCES ITSELF: a loud, gravelly
; monster ROAR. Two channels at once — CH1 pulse is the VOICE (duty 50%, volume 11, a hardware
; sweep bending the pitch DOWN from ~380 Hz: the "RAAAWR" bellow), CH4 noise is the THROAT
; (volume 9, rolling through deep 15-bit rumble and 7-bit crunch under the bellow: the gravel).
; A pure noise growl read as static; the pitched bellow is what says "a monster is here." Both
; ring ~0.8 s. Self-contained: enables the APU + panning/volume each play.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
; ── CH1 — the bellow (pitched voice, sweeping DOWN) ──────────────────────────────────────────
ld a, $3C
ldh [rNR10], a    ; sweep: pace 3, SUBTRACT (pitch falls), shift 4 — the descending roar
ld a, $80
ldh [rNR11], a    ; duty 50% (a fat, vocal body)
ld a, $B5
ldh [rNR12], a    ; envelope: volume 11, decrease, pace 5 (loud, ~0.86 s)
ld a, $A7
ldh [rNR13], a    ; period low — $6A7 → ~380 Hz start (high bits = 6)
ld a, $86
ldh [rNR14], a    ; trigger, period high = 6
; ── CH4 — the throat (noise gravel, rolled under the bellow) ─────────────────────────────────
ld a, $00
ldh [rNR41], a    ; length unused (the envelope ends it)
ld a, $95
ldh [rNR42], a    ; envelope: volume 9, decrease, pace 5
ld a, $67
ldh [rNR43], a    ; noise: clock shift 6, 15-bit LFSR, divisor 7 — the deep opening rumble
ld a, $80
ldh [rNR44], a    ; trigger
ld d, $0A         ; 10 roll steps of gravel under the bellow (~0.75 s)
.roll:
ld bc, $2C00      ; ~0.075 s per step (28 cycles/iter @ 4.19 MHz)
.gap:
dec bc
ld a, b
or c
jr nz, .gap
ld a, d
and $03           ; roll through 4 timbres — the grumbly, crunchy undulation
jr z, .deep
cp $01
jr z, .mid
cp $02
jr z, .crunch
ld a, $78         ; 3 → shift 7, 7-bit LFSR — a deep gnashing crunch
jr .setclock
.crunch:
ld a, $68         ; 2 → shift 6, 7-bit LFSR — a mid crunch
jr .setclock
.mid:
ld a, $66         ; 1 → shift 6, 15-bit, divisor 6 — a mid rumble
jr .setclock
.deep:
ld a, $67         ; 0 → shift 6, 15-bit, divisor 7 — the deep rumble
.setclock:
ldh [rNR43], a    ; roll the throat — NO re-trigger, so the envelope keeps fading
dec d
jr nz, .roll
.idle:
jr .idle
