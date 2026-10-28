// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/frontend/wqm.h"

namespace Shader::Gcn {
namespace {
constexpr u32 Exec = u32(OperandField::ExecLo);
constexpr u32 Vcc = u32(OperandField::VccLo);

bool Operand(const InstOperand& operand, u32 code) {
    const auto field = code <= ScalarGPRMax ? OperandField::ScalarGPR : OperandField(code);
    return operand.code == code && operand.field == field;
}

bool Move(const GcnInst& inst, u32 dst, u32 src) {
    return inst.opcode == Opcode::S_MOV_B64 && Operand(inst.dst[0], dst) &&
           Operand(inst.src[0], src);
}

bool Wqm(const GcnInst& inst, u32 src) {
    return inst.opcode == Opcode::S_WQM_B64 && Operand(inst.dst[0], Exec) &&
           Operand(inst.src[0], src);
}

bool Wait(const GcnInst& inst) {
    return inst.opcode == Opcode::S_WAITCNT || inst.opcode == Opcode::S_NOP;
}

bool WritesMask(const GcnInst& inst, u32 saved) {
    for (const auto& dst : inst.dst) {
        const u32 count = inst.category == InstCategory::ScalarMemory ? inst.control.smrd.count : 2;
        if (dst.field == OperandField::ExecLo || dst.field == OperandField::ExecHi ||
            (dst.field == OperandField::ScalarGPR &&
             // Conservatively treat scalar ALU writes as pairs; memory loads
             // can cover up to sixteen registers starting well below saved.
             dst.code + count > saved && dst.code <= saved + 1)) {
            return true;
        }
    }
    return inst.IsCmpx() || (inst.opcode >= Opcode::S_AND_SAVEEXEC_B64 &&
                            inst.opcode <= Opcode::S_XNOR_SAVEEXEC_B64);
}

bool LaneLocal(const GcnInst& inst) {
    // Do not speculate cross-lane operations, DPP/SDWA, stores or atomics.
    if (inst.opcode == Opcode::V_READFIRSTLANE_B32 || inst.opcode == Opcode::V_READLANE_B32 ||
        inst.opcode == Opcode::V_WRITELANE_B32 || inst.IsCmpx()) {
        return false;
    }
    for (const auto& src : inst.src) {
        if (src.field == OperandField::Dpp || src.field == OperandField::Sdwa || src.dpp ||
            src.sdwa_sel != SdwaSelector::Invalid) return false;
    }
    for (const auto& dst : inst.dst) {
        if (dst.sdwa_dst != SdwaDstUnused::Invalid) return false;
    }
    return inst.category == InstCategory::VectorALU ||
           inst.category == InstCategory::VectorInterpolation || Wait(inst) ||
           inst.opcode == Opcode::IMAGE_SAMPLE || inst.opcode == Opcode::IMAGE_SAMPLE_C;
}

bool Matches(std::span<const GcnInst> ins, size_t branch) {
    if (branch < 2 || ins[branch].opcode != Opcode::S_CBRANCH_SCC0) return false;
    const auto& mask = ins[branch - 1];
    if (mask.opcode != Opcode::S_ANDN2_B64 ||
        mask.dst[0].field != OperandField::ScalarGPR ||
        mask.src[0].field != OperandField::ScalarGPR ||
        mask.src[0].code != mask.dst[0].code || !Operand(mask.src[1], Vcc)) return false;
    const u32 saved = mask.dst[0].code;
    if (saved + 1 > ScalarGPRMax) return false;
    const auto& vcc = ins[branch - 2];
    if (vcc.opcode != Opcode::S_AND_B64 || !Operand(vcc.dst[0], Vcc) ||
        !((Operand(vcc.src[0], Vcc) && Operand(vcc.src[1], Exec)) ||
          (Operand(vcc.src[1], Vcc) && Operand(vcc.src[0], Exec)))) return false;

    // Require straight-line entry, saved original EXEC, and no intervening
    // change to it or its backup. In particular, this cannot run inside a
    // previously divergent scope or after an earlier export.
    bool backup = false;
    for (size_t i = 0; i < branch - 2; ++i) {
        const auto& inst = ins[i];
        if (inst.IsTerminateInstruction() || inst.category == InstCategory::Export) return false;
        if (Move(inst, saved, Exec)) {
            backup = true;
        } else if (!Wqm(inst, Exec) && WritesMask(inst, saved)) {
            return false;
        }
    }
    if (!backup) return false;

    size_t i = branch + 1;
    // The compiler can load normal-map descriptors before restoring WQM.
    while (i < ins.size() && (ins[i].category == InstCategory::ScalarMemory || Wait(ins[i]))) {
        if (WritesMask(ins[i], saved)) return false;
        ++i;
    }
    if (i == ins.size() || !Wqm(ins[i++], saved)) return false;
    bool implicit_sample = false;
    for (; i < ins.size() && !Move(ins[i], Exec, saved); ++i) {
        if (!LaneLocal(ins[i]) || WritesMask(ins[i], saved)) return false;
        implicit_sample |= ins[i].opcode == Opcode::IMAGE_SAMPLE ||
                           ins[i].opcode == Opcode::IMAGE_SAMPLE_C;
    }
    if (!implicit_sample || i == ins.size()) return false;
    ++i; // exact EXEC restoration; the translator's vm export rejects dead lanes
    bool export_seen = false;
    for (; i < ins.size() && ins[i].opcode == Opcode::EXP; ++i) {
        if (!ins[i].control.exp.vm) return false;
        export_seen = true;
    }
    if (!export_seen || i == ins.size() || ins[i].opcode != Opcode::S_ENDPGM) return false;
    ++i;

    // The taken shortcut must jump exactly to an output-free terminal block.
    // No other branch may enter either the shading region or its epilogue.
    u32 pc = 0, branch_pc = 0, empty_pc = 0;
    for (size_t n = 0; n < ins.size(); ++n) {
        if (n == branch) branch_pc = pc;
        if (n == i) empty_pc = pc;
        if (n != branch && (ins[n].IsConditionalBranch() || ins[n].IsUnconditionalBranch()))
            return false;
        pc += ins[n].length;
    }
    if (i == ins.size() || ins[branch].BranchTarget(branch_pc) != empty_pc) return false;
    if (Move(ins[i], Exec, 128) || Move(ins[i], Exec, saved)) ++i;
    bool empty_export = false;
    for (; i < ins.size() && ins[i].opcode == Opcode::EXP; ++i) {
        // A zero component mask writes no data for MRT/depth as well as NULL.
        // Observed form: MRT0, en=0, vm=1 after clearing EXEC in this block.
        if (ins[i].control.exp.en != 0 || ins[i].control.exp.target > 9) return false;
        empty_export = true;
    }
    return empty_export && i + 1 == ins.size() && ins[i].opcode == Opcode::S_ENDPGM;
}
} // namespace

u32 RemoveWqmEmptyWaveBranches(std::span<GcnInst> instructions) {
    for (size_t i = 0; i < instructions.size(); ++i) {
        if (!Matches(instructions, i)) continue;
        // Keep every lane through read-only shading, even for an empty wave.
        // Retain the saved live mask and masked export unchanged. The redundant
        // empty-wave shortcut is only an optimization in the GCN program.
        instructions[i].opcode = Opcode::S_NOP;
        instructions[i].control.sopp.simm = 0;
        return 1;
    }
    return 0;
}
} // namespace Shader::Gcn
