; paddle_bounce — ball off the paddle (also voices the serve). A short, bright mid blip on pulse CH2
; (~700 Hz), 50% duty, a fast volume-envelope decay so the note silences itself (no note-off needed).
; Self-contained: enables the APU and sets panning/volume each play, so any SFX can be the first one
; cued. Moderate volume (accessibility).
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $80
ldh [rNR21], a    ; CH2 duty 50%
ld a, $A2
ldh [rNR22], a    ; envelope: initial volume 10, decrease, pace 2 (snappy decay)
ld a, $45
ldh [rNR23], a    ; frequency low  — period $745 → ~700 Hz
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
