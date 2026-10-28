// SPDX-License-Identifier: GPL-2.0-or-later
// Reproduce an alpha-only AO export using the production GCN translator.
#include <cassert>
#include <cstdio>
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/profile.h"
#include "video_core/amdgpu/regs_color.h"

using namespace Shader;

static void CheckExport(AmdGpu::DataFormat format, AmdGpu::ColorBuffer::SwapMode swap,
                        u32 expected_source) {
    AmdGpu::ColorBuffer cb{};
    cb.info.format = u32(format);
    cb.info.comp_swap = swap;
    const auto mapping = cb.Swizzle();
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    auto& output = runtime.hw.fs.color_buffers[0];
    output.data_format = format;
    output.num_format = AmdGpu::NumberFormat::Float;
    output.export_format = AmdGpu::ShaderExportFormat::ABGR_32;
    output.swizzle = mapping;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    Profile profile{};
    Gcn::Translator translator{info, runtime, profile};
    Gcn::GcnInst instruction{};
    instruction.opcode = Gcn::Opcode::EXP;
    instruction.category = Gcn::InstCategory::Export;
    instruction.control.exp.en = 15;
    instruction.control.exp.target = u32(IR::Attribute::RenderTarget0);
    for (u32 i = 0; i < 4; ++i) instruction.src[i].code = i;
    translator.Translate(&block, 0, IR::Condition::True, std::span{&instruction, 1});
    bool found = false;
    for (const auto& inst : block.Instructions()) {
        if (inst.GetOpcode() != IR::Opcode::SetAttribute || inst.Arg(2).U32() != 0) continue;
        auto value = inst.Arg(1);
        while (value.Inst()->GetOpcode() == IR::Opcode::BitCastF32U32)
            value = value.Inst()->Arg(0);
        assert(value.Inst()->GetOpcode() == IR::Opcode::GetVectorRegister);
        assert(u32(value.Inst()->Arg(0).VectorReg()) == expected_source);
        found = true;
    }
    assert(found);
    // The color mask must enable the stored channel when its shader source is
    // enabled, and disable it when only other shader sources are enabled.
    for (u32 mask = 0; mask < 16; ++mask)
        assert((mapping.ApplyMask(mask) & 1) == ((mask >> expected_source) & 1));
}

int main() {
    using namespace AmdGpu;
    for (auto format : {DataFormat::Format8, DataFormat::Format16, DataFormat::Format32}) {
        CheckExport(format, ColorBuffer::SwapMode::Standard, 0);
        CheckExport(format, ColorBuffer::SwapMode::Alternate, 1);
        CheckExport(format, ColorBuffer::SwapMode::StandardReverse, 2);
        CheckExport(format, ColorBuffer::SwapMode::AlternateReverse, 3);
        // Verify the texture-read alpha alias remains intact.
        const CompMapping alpha{.r=CompSwizzle::Zero, .g=CompSwizzle::Zero,
                                .b=CompSwizzle::Zero, .a=CompSwizzle::Alpha};
        assert(RemapSwizzle(format, alpha).a == CompSwizzle::Red);
    }
    // Non-involutive packed-format mappings must use the same direction for
    // write masks as for the shader's destination-to-source mapping.
    const CompMapping cycle{.r=CompSwizzle::Alpha, .g=CompSwizzle::Red,
                            .b=CompSwizzle::Blue, .a=CompSwizzle::Green};
    assert(cycle.ApplyMask(1) == 2);
    assert(cycle.ApplyMask(2) == 8);
    assert(cycle.ApplyMask(8) == 1);
    std::puts("color-export-test: passed (GCN export channels and physical write masks)");
}
