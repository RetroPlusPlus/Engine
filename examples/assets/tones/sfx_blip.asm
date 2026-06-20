; sfx_blip — a synthetic ONE-SHOT SFX for the auto-close test. Triggers pulse CH2 with a fast DECREASING
; envelope (volume 15 → 0 at pace 1 ≈ 234 ms), then idles forever. It NEVER disables the DAC (no NR52=0) —
; exactly the finished-SFX shape the output-silence auto-close must catch: the channel decays to volume 0,
; the high-pass settles the DAC-on tail to exact (0,0), and the AudioSystem stops stepping the VM. Mirrors
; the real Bongusoid SFX drivers (e.g. serve.asm), just with a shorter envelope so the test runs quickly.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $80
ldh [rNR21], a    ; CH2 duty 50%
ld a, $F1
ldh [rNR22], a    ; envelope: initial volume 15, decrease, pace 1 (fast decay ~234 ms to silence)
ld a, $00
ldh [rNR23], a    ; frequency low
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
