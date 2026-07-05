; beam — an abductor's tractor beam lights: a funky, warbling saucer hum. CH2 pulse at a low duty
; (thin and electronic) held at a MODEST volume (5, not the old klaxon's blaring 10 — the beam is a
; tell, not an alarm), warbling between two nearby pitches ~7 times a second while a slow envelope
; fades it out over ~0.5 s. The pitch wobble is written to NR23 WITHOUT re-triggering, so the
; envelope keeps decaying underneath — the classic sci-fi "wonk-wonk-wonk" tractor pull. The two
; periods share their high 3 bits ($6xx), so only the low byte changes per warble step.
; Self-contained: enables the APU + panning/volume each play, so any SFX can be the first cued.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $40
ldh [rNR21], a    ; CH2 duty 25% (thin, electronic)
ld a, $56
ldh [rNR22], a    ; envelope: initial volume 5, decrease, pace 6 (quiet, fades in ~0.5 s)
ld a, $89
ldh [rNR23], a    ; period low  — $689 → ~350 Hz (high bits = 6)
ld a, $86
ldh [rNR24], a    ; trigger, period high = 6
ld d, $08         ; 8 warble half-steps
.warble:
ld bc, $2800      ; ~0.07 s per step (28 cycles/iter @ 4.19 MHz)
.gap:
dec bc
ld a, b
or c
jr nz, .gap
ld a, d
and $01
jr z, .lowpitch
ld a, $DC         ; odd step → period $6DC → ~450 Hz (the up-wonk)
jr .setpitch
.lowpitch:
ld a, $89         ; even step → period $689 → ~350 Hz (the down-wonk)
.setpitch:
ldh [rNR23], a    ; shift the pitch — NO re-trigger, so the envelope keeps fading
dec d
jr nz, .warble
.idle:
jr .idle
