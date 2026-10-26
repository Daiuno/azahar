// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Thumb32 handlers: BL, BLX (32-bit Thumb instructions)

handle_ThumbBl: {
    state_.regs[14] = inst->branch.lr;   // Precomputed at decode: (PC+4)|1
    SET_PC(inst->branch.target);
    executed += inst->ticks;
    goto chain;
}

handle_ThumbBlxImm: {
    state_.regs[14] = inst->branch.lr;   // Precomputed at decode: (PC+4)|1
    state_.thumb_mode = false;  // Switch to ARM mode
    SET_PC(inst->branch.target);
    executed += inst->ticks;
    goto chain;
}

// Standalone ARMv6 BL halves (matches DynCom BL_1/BL_2/BLX_1_THUMB)
handle_ThumbBlPrefix: {
    state_.regs[14] = inst->imm32;       // Precomputed at decode: PC+4+(imm11<<12)
    executed += inst->ticks;
    SET_PC(GET_PC() + 2);
    DISPATCH();
}

handle_ThumbBlSuffix: {
    const u32 target = state_.regs[14] + inst->imm32;
    state_.regs[14] = (GET_PC() + 2) | 1;
    SET_PC(target & ~1u);
    executed += inst->ticks;
    goto chain;
}

handle_ThumbBlxSuffix: {
    const u32 target = (state_.regs[14] + inst->imm32) & ~3u;
    state_.regs[14] = (GET_PC() + 2) | 1;
    state_.thumb_mode = false;
    SET_PC(target);
    executed += inst->ticks;
    goto chain;
}
