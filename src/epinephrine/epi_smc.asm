; SPDX-License-Identifier: MIT
; Original SMCCC trampoline; x19 retains the register-block pointer across the monitor call.
#include "kxarm64.h"
        NESTED_ENTRY EpiSmcInvoke
        PROLOG_SAVE_REG_PAIR x19, x20, #-32!
        PROLOG_SAVE_REG_PAIR fp, lr, #16
        mov     x19, x0
        ldp     x0, x1, [x19, #0]
        ldp     x2, x3, [x19, #16]
        ldp     x4, x5, [x19, #32]
        ldp     x6, x7, [x19, #48]
        dsb     sy
        smc     #0
        dsb     sy
        isb
        stp     x0, x1, [x19, #0]
        stp     x2, x3, [x19, #16]
        str     x6, [x19, #48]
        EPILOG_RESTORE_REG_PAIR fp, lr, #16
        EPILOG_RESTORE_REG_PAIR x19, x20, #32!
        EPILOG_END ret
        NESTED_END EpiSmcInvoke
        END
