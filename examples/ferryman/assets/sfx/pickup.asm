; pickup — a colonist climbs aboard (also voices a deliberate Drop, its quieter emotional twin).
; A bright clip-on blip: CH1 with a gentle upward hardware sweep so it chirps hopefully, snappy
; envelope. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $23
ldh [rNR10], a    ; CH1 sweep: pace 2, addition (pitch rises), shift 3 — a hopeful chirp
ld a, $80
ldh [rNR11], a    ; CH1 duty 50%
ld a, $92
ldh [rNR12], a    ; envelope: initial volume 9, decrease, pace 2
ld a, $9B
ldh [rNR13], a    ; frequency low  — period $79B → ~1300 Hz
ld a, $87
ldh [rNR14], a    ; trigger + frequency high
.idle:
jr .idle
