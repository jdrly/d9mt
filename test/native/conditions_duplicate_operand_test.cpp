/* conditions_duplicate_operand_test: dxbc-spirv's ConditionPass and a boolean op whose two
 * operands are the same condition, under the shipping pass pipeline (ir::legalizeIr with d9mt's
 * options: all defaults, as driver/d3d9/d3d9_shader_compiler.cpp runPhaseA).
 *
 * ConditionPass::handleBooleanOp rewrites each operand of a BAnd knowing the other one is true
 * (of a BOr: false). At 76236b8 it took the other operand from the original instruction for both,
 * so two equivalent operands were each folded on the strength of the other:
 *   a = (x == c), b = (x == c), a && b   became   true && true
 * for every x. Upstream f528e1e ("ir: Use rewritten operand in handleBooleanOp instead of the
 * original", patches/dxbc-spirv/0004) takes it from the instruction as rewritten so far. The two
 * operands are different definitions of one expression when the pass sees them (CsePass runs
 * after it), which is what nested selects on one comparison give: ArithmeticPass merges
 * select(a, select(b, p, q), q) into select(a && b, p, q).
 *
 * A pixel shader computes from one input x, per output:
 *   0  select((x == c) && (x == c), 1, 0)       two definitions
 *   1  select((x != c) && (x != c), 1, 0)       two definitions (upstream's example)
 *   2  select((x == c) || (x == c), 1, 0)       two definitions
 *   3  select((x != c) || (x != c), 1, 0)       two definitions
 *   4  select(a && a, 1, 0), a = (x == c)       one definition
 *   5  select(a || a, 1, 0), a = (x != c)       one definition
 *   6  select(x == c, select(x == c, 1, 0), 0)  nested selects, two definitions
 * After legalizeIr every output must still be select(<the one comparison of x and c>, 1, 0): a
 * select whose condition is that comparison of the loaded input, never a constant and never a
 * boolean op. Built by scripts/run-native-tests.sh against third_party/dxbc-spirv (host, no GPU).
 * Without 0004 outputs 0, 1 and 6 fail (constant 1). */
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

const uint32_t OutputCount = 7u;

}

int main() {
  ir::Builder b;
  auto fn = b.add(ir::Op::Function(ir::ScalarType::eVoid));
  auto entry = b.add(ir::Op::EntryPoint(fn, ir::ShaderStage::ePixel));
  auto in = b.add(ir::Op::DclInput(ir::ScalarType::eF32, entry, 0u, 0u));
  ir::SsaDef out[OutputCount];
  for (uint32_t i = 0u; i < OutputCount; i++)
    out[i] = b.add(ir::Op::DclOutput(ir::ScalarType::eF32, entry, i, 0u));

  /* Scoped IR, as the sm3 converter emits it: no label (ConvertControlFlowPass adds the blocks). */
  auto x = b.add(ir::Op::InputLoad(ir::ScalarType::eF32, in, ir::SsaDef()));
  auto c = b.makeConstant(0.5f);
  auto one = b.makeConstant(1.0f);
  auto zero = b.makeConstant(0.0f);

  auto eq = [&] { return b.add(ir::Op::FEq(ir::ScalarType::eBool, x, c)); };
  auto ne = [&] { return b.add(ir::Op::FNe(ir::ScalarType::eBool, x, c)); };
  auto store = [&] (uint32_t index, ir::SsaDef cond) {
    b.add(ir::Op::OutputStore(out[index], ir::SsaDef(), b.add(ir::Op::Select(ir::ScalarType::eF32, cond, one, zero))));
  };

  ir::OpCode expected[OutputCount] = {
    ir::OpCode::eFEq, ir::OpCode::eFNe, ir::OpCode::eFEq, ir::OpCode::eFNe,
    ir::OpCode::eFEq, ir::OpCode::eFNe, ir::OpCode::eFEq };
  const char* names[OutputCount] = {
    "(x == c) && (x == c), two definitions",
    "(x != c) && (x != c), two definitions",
    "(x == c) || (x == c), two definitions",
    "(x != c) || (x != c), two definitions",
    "a && a, a = (x == c)",
    "a || a, a = (x != c)",
    "nested selects on x == c, two definitions" };

  { auto p = eq(); auto q = eq(); store(0u, b.add(ir::Op::BAnd(ir::ScalarType::eBool, p, q))); }
  { auto p = ne(); auto q = ne(); store(1u, b.add(ir::Op::BAnd(ir::ScalarType::eBool, p, q))); }
  { auto p = eq(); auto q = eq(); store(2u, b.add(ir::Op::BOr(ir::ScalarType::eBool, p, q))); }
  { auto p = ne(); auto q = ne(); store(3u, b.add(ir::Op::BOr(ir::ScalarType::eBool, p, q))); }
  { auto p = eq(); store(4u, b.add(ir::Op::BAnd(ir::ScalarType::eBool, p, p))); }
  { auto p = ne(); store(5u, b.add(ir::Op::BOr(ir::ScalarType::eBool, p, p))); }
  { auto p = eq(); auto q = eq();
    auto inner = b.add(ir::Op::Select(ir::ScalarType::eF32, q, one, zero));
    b.add(ir::Op::OutputStore(out[6u], ir::SsaDef(), b.add(ir::Op::Select(ir::ScalarType::eF32, p, inner, zero)))); }

  b.add(ir::Op::Return());
  b.add(ir::Op::FunctionEnd());

  ir::CompileOptions options = {};
  ir::legalizeIr(b, options);

  uint32_t found = 0u;
  for (const auto& op : b) {
    if (op.getOpCode() != ir::OpCode::eOutputStore)
      continue;
    const ir::Op& decl = b.getOpForOperand(op, 0u);
    uint32_t loc = uint32_t(decl.getOperand(1u));
    if (loc >= OutputCount)
      continue;
    found |= 1u << loc;

    const ir::Op& value = b.getOpForOperand(op, 2u);
    bool ok = value.getOpCode() == ir::OpCode::eSelect;
    const char* why = "the stored value is not a select";
    if (value.getOpCode() == ir::OpCode::eConstant)
      why = "the stored value is a constant";

    if (ok) {
      const ir::Op& cond = b.getOpForOperand(value, 0u);
      const ir::Op& onTrue = b.getOpForOperand(value, 1u);
      const ir::Op& onFalse = b.getOpForOperand(value, 2u);
      if (cond.getOpCode() != expected[loc]) {
        ok = false; why = "the select's condition is not the one comparison";
      } else if (b.getOpForOperand(cond, 0u).getOpCode() != ir::OpCode::eInputLoad
              || b.getOpForOperand(cond, 1u).getDef() != c) {
        ok = false; why = "the comparison is not of the loaded input and c";
      } else if (onTrue.getDef() != one || onFalse.getDef() != zero) {
        ok = false; why = "the select's values are not 1 and 0";
      }
    }
    std::printf("output %u: final op #%u\n", loc, uint32_t(value.getOpCode()));
    check(ok, std::string(names[loc]) + (ok ? ": still select(the comparison, 1, 0)" : std::string(": ") + why));
  }
  check(found == (1u << OutputCount) - 1u, "all outputs are still stored");
  return failures ? 1 : 0;
}
