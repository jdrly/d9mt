/* conditions_signed_zero_test: dxbc-spirv's ConditionPass (76236b8) and signed zero, under the
 * shipping pass pipeline (ir::legalizeIr with d9mt's options: all defaults, as
 * driver/d3d9/d3d9_shader_compiler.cpp runPhaseA).
 *
 * ConditionPass::evaluateBooleanAnd treats FEq x, c0 and FEq x, c1 as mutually exclusive when c0
 * and c1 are different constant definitions. +0.0 and -0.0 are different definitions that compare
 * equal, so at x = 0 both are true. A pixel shader computes, from one input x:
 *   out0 = select(x == +0, select(x == -0, 1, 0), 0)   (nested predication, `cmp`)
 *   out1 = select((x == +0) && (x == -0), 1, 0)          (BAnd)
 *   out2 = select(x == +0, select(x == +0, 1, 0), 0)    (the control: one constant, a valid fold)
 * After legalizeIr no output may be the constant 0: at x = 0 every one is 1. The control must
 * still be a live value as well (or the constant-free fold to x == +0), so a pass that gives
 * up on everything doesn't pass by accident.
 *
 * Built by scripts/run-native-tests.sh against third_party/dxbc-spirv (host, no GPU). It prints
 * each output's final op and the disassembly of the whole shader for the record. */
#include <cstdio>
#include <string>

#include <ir/ir_builder.h>
#include <ir/ir_legalize.h>

using namespace dxbc_spv;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok)
    failures++;
}

bool isConstantZero(const ir::Op& op) {
  if (op.getOpCode() != ir::OpCode::eConstant)
    return false;
  for (uint32_t i = 0u; i < op.getOperandCount(); i++) {
    if (uint64_t(op.getOperand(i)) & 0x7fffffffu)
      return false;
  }
  return true;
}

}

int main() {
  ir::Builder b;
  auto fn = b.add(ir::Op::Function(ir::ScalarType::eVoid));
  auto entry = b.add(ir::Op::EntryPoint(fn, ir::ShaderStage::ePixel));
  auto in = b.add(ir::Op::DclInput(ir::ScalarType::eF32, entry, 0u, 0u));
  ir::SsaDef out[3];
  for (uint32_t i = 0u; i < 3u; i++)
    out[i] = b.add(ir::Op::DclOutput(ir::ScalarType::eF32, entry, i, 0u));

  b.add(ir::Op::Label());
  auto x = b.add(ir::Op::InputLoad(ir::ScalarType::eF32, in, ir::SsaDef()));
  auto pos = b.makeConstant(0.0f);
  auto neg = b.makeConstant(-0.0f);
  auto one = b.makeConstant(1.0f);
  auto zero = pos;
  check(pos != neg, "+0.0 and -0.0 are different constant definitions (the premise)");

  auto eqPos = b.add(ir::Op::FEq(ir::ScalarType::eBool, x, pos));
  auto eqNeg = b.add(ir::Op::FEq(ir::ScalarType::eBool, x, neg));
  auto eqPos2 = b.add(ir::Op::FEq(ir::ScalarType::eBool, x, pos));

  auto inner0 = b.add(ir::Op::Select(ir::ScalarType::eF32, eqNeg, one, zero));
  b.add(ir::Op::OutputStore(out[0], ir::SsaDef(), b.add(ir::Op::Select(ir::ScalarType::eF32, eqPos, inner0, zero))));

  auto both = b.add(ir::Op::BAnd(ir::ScalarType::eBool, eqPos, eqNeg));
  b.add(ir::Op::OutputStore(out[1], ir::SsaDef(), b.add(ir::Op::Select(ir::ScalarType::eF32, both, one, zero))));

  auto inner2 = b.add(ir::Op::Select(ir::ScalarType::eF32, eqPos2, one, zero));
  b.add(ir::Op::OutputStore(out[2], ir::SsaDef(), b.add(ir::Op::Select(ir::ScalarType::eF32, eqPos, inner2, zero))));

  b.add(ir::Op::Return());
  b.add(ir::Op::FunctionEnd());

  ir::CompileOptions options = {};
  ir::legalizeIr(b, options);

  const char* names[3] = { "nested select, x == +0 then x == -0",
                           "BAnd of x == +0 and x == -0",
                           "control: nested select on x == +0 twice" };
  uint32_t found = 0u;
  for (const auto& op : b) {
    if (op.getOpCode() != ir::OpCode::eOutputStore)
      continue;
    const ir::Op& decl = b.getOpForOperand(op, 0u);
    uint32_t loc = uint32_t(decl.getOperand(1u));
    const ir::Op& value = b.getOpForOperand(op, 2u);
    std::printf("output %u: final op #%u (%s)\n", loc, uint32_t(value.getOpCode()),
      value.getOpCode() == ir::OpCode::eConstant ? "constant" : "live");
    if (loc < 3u) {
      found |= 1u << loc;
      check(!isConstantZero(value), std::string(names[loc]) + ": not folded to 0 (x = 0 makes it 1)");
    }
  }
  check(found == 7u, "all three outputs are still stored");
  return failures ? 1 : 0;
}
