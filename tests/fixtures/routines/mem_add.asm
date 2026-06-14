; read an HRAM cell, add $10, write to a different cell, return it in A
ldh a, [$FF90]
add $10
ldh [$FF91], a
ret
