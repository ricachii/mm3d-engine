// Implementacion de las funciones externas Thumb del generador AOT (docs/decisiones.md D-005/D-006).
//
// Se invoca con una entrada con el bit 0 activo (BLX <imm> desde codigo ARM, o un destino indirecto Thumb que el despachador
// generado entrega al ExternalCall por defecto). Interpreta hasta que el control pasa a ARM y devuelve el flujo que el
// despachador sabe continuar: Returned/Branch con el PC ARM, o MemoryFault/BlockLimit/Unsupported.
#pragma once

#include "mm3d_thumb_interpreter.h"
#include "oot3d_native_whole_aot_runtime.h"

namespace Oot3dNativeGame {

inline Oot3dWholeAotFlow ExecuteThumbExternalCall(
    uint32_t entry, Oot3dWholeAotFrame& frame, NativeA32Memory& memory) {
    // Presupuesto por llamada; al agotarse se devuelve BlockLimit con el PC Thumb (bit 0) y se reanuda desde fuera.
    constexpr uint32_t kThumbInstructionBudget = 1U << 22;
    const mm3d::thumb::Result result =
        mm3d::thumb::Run(frame.Guest, memory, entry, kThumbInstructionBudget);
    switch (result.kind) {
    case mm3d::thumb::Exit::Return:
        return Oot3dAotReturned(result.pc);
    case mm3d::thumb::Exit::Branch:
        return Oot3dAotBranch(result.pc);
    case mm3d::thumb::Exit::MemoryFault:
        return {Oot3dWholeAotFlowKind::MemoryFault, result.pc, result.detail};
    case mm3d::thumb::Exit::BlockLimit:
        return {Oot3dWholeAotFlowKind::BlockLimit, result.pc, 0U};
    default:
        return {Oot3dWholeAotFlowKind::Unsupported, result.pc, result.detail};
    }
}

}  // namespace Oot3dNativeGame
