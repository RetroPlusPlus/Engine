; tone_c — trigger CH3 at C4 (~262 Hz). The wave channel is set up once by wave_init.asm; this routine
; only sets the frequency and triggers, so a (re-)play never rewrites wave RAM (no corruption) or
; toggles the DAC (no pop).
ld a, $06
ldh [rNR33], a    ; frequency low — C4
ld a, $87
ldh [rNR34], a    ; trigger (bit 7) + frequency high
.idle:
jr .idle
