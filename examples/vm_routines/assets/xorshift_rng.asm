; xorshiftRng — this example's OWN random-number routine, hosted on the engine's VM.
;
; The engine ships no RNG algorithm: an algorithm has design choices in it, so it belongs to the game
; that authors it. What the engine supplies is the machine, the binding, and the free-running divider.
; This file is the game's half.
;
; The generator is Marsaglia's 8-bit xorshift (shift triple 7, 5, 3), written here in SM83:
;
;     x ^= x << 7;   x ^= x >> 5;   x ^= x << 3
;
; The seed lives in one high-RAM cell of the game's choosing and advances by pure xorshift, so it
; cycles through all 255 non-zero values and never sticks — provided it starts non-zero, which the
; program does by writing the cell before the first roll.
;
; The returned byte is the advanced seed XORed with the live divider, so the stream carries both the
; generator's spread and real elapsed-time entropy. The divider only moves if the host advances the
; machine's clock between calls (Vm::advanceTick), which is exactly what makes this a VM routine
; rather than something to write in C++.
;
; No inputs; returns the byte in A.

; x ^= x << 7  — bit 0 becomes bit 7, everything else clears
ldh a, [$FF90]
ld  b, a
rrca
and $80
xor b

; x ^= x >> 5  — rotating left 3 is rotating right 5, then keep the low three bits
ld  b, a
rlca
rlca
rlca
and $07
xor b

; x ^= x << 3
ld  b, a
rlca
rlca
rlca
and $F8
xor b

; Store the advanced seed, then return it mixed with the divider.
ldh [$FF90], a
ld  b, a
ldh a, [rDIV]
xor b
ret
