#include <algorithm>
#include <utility>

#include "../util/util_bit.h"
#include "../util/util_math.h"

#include <sm3/sm3_io_map.h>

#include "d3d9_fixed_function.h"
#include "d3d9_shader_analysis.h"
#include "d3d9_util.h"

#include "../util/log/log.h"

using namespace dxbc_spv::sm3;

namespace dxvk {

  using namespace dxvk::caps;

  D3D9ShaderAnalysis::D3D9ShaderAnalysis(dxbc_spv::util::ByteReader code, bool isSWVP)
    : m_isSWVP(isSWVP) {
    if (!code) {
      Logger::err("No code chunk found in shader.");
      return;
    }

    if (Parser parser = Parser(code)) {
      m_length = 0u;
      RunAnalysis(parser);
    } else {
      Logger::err("Failed to parse code chunk.");
    }
  }

  bool D3D9ShaderAnalysis::RunAnalysis(Parser& parser) {
    if (!(m_shaderInfo = parser.getShaderInfo()))
      return false;

    if (m_shaderInfo.getVersion().first == 1u && m_shaderInfo.getType() == dxbc_spv::sm3::ShaderType::ePixel)
      m_usedRTs = 0b1u;

    D3D9ImmediateConstantsData shaderDefs;

    ConstantMask constMaskF;
    ConstantMask constMaskI;

    D3D9IoDclList ioDcls;

    while (parser) {
      dxbc_spv::sm3::Instruction instruction = parser.parseInstruction();

      if (!instruction || !HandleInstruction(instruction, constMaskF, constMaskI, shaderDefs, ioDcls))
        return false;
    }

    m_length = parser.getByteOffset();

    for (size_t i = 0u; i < shaderDefs.floats.size(); i++)
      clrBit(constMaskF, shaderDefs.floats[i].index);

    for (size_t i = 0u; i < shaderDefs.ints.size(); i++)
      clrBit(constMaskI, shaderDefs.ints[i]);

    // HWVP VS: defs above the API range are inlined by the converter; any other
    // read up there (or a0-relative access that could reach a def there) is not
    if (!m_isSWVP && m_shaderInfo.getType() == ShaderType::eVertex) {
      for (uint32_t i = MaxFloatConstantsVS / 32u; i < constMaskF.size(); i++)
        m_readsApiFloatBeyondHw |= constMaskF[i] != 0u;

      if (m_constants.floatsAccessedDynamically) {
        for (const auto& d : shaderDefs.floats)
          m_readsApiFloatBeyondHw |= d.index >= MaxFloatConstantsVS;
      }

      m_constants.floatCount = std::min(m_constants.floatCount, MaxFloatConstantsVS);
    }

    D3D9ConstantBufferLayout constLayoutF = m_constants.floatsAccessedDynamically
      ? D3D9ConstantBufferLayout(m_constants.floatCount, constMaskF.size(), constMaskF.data(),
          shaderDefs.floats.size(), shaderDefs.floats.data())
      : D3D9ConstantBufferLayout(constMaskF.size(), constMaskF.data());

    D3D9ConstantBufferLayout constLayoutI(constMaskI.size(), constMaskI.data());
    D3D9ConstantBufferLayout constLayoutB;

    if (m_isSWVP && m_shaderInfo.getType() == ShaderType::eVertex) {
      uint32_t dwordCount = align(m_constants.boolCount, 32u) / 32u;
      constLayoutB = D3D9ConstantBufferLayout(align(dwordCount, 4u));
    }

    m_constLayout = D3D9ConstantBufferCopy::getOrCreate(
      std::move(constLayoutF),
      std::move(constLayoutI),
      std::move(constLayoutB));

    if (m_shaderInfo.getType() == ShaderType::eVertex)
      m_usedSamplers <<= FirstVSSamplerSlot;

    return true;
  }

  bool D3D9ShaderAnalysis::HandleInstruction(
    const dxbc_spv::sm3::Instruction&   op,
          ConstantMask&                 constMaskF,
          ConstantMask&                 constMaskI,
          D3D9ImmediateConstantsData&   shaderDefs,
          D3D9IoDclList&                ioDcls) {
    ScanConverterHazards(op, ioDcls);

    auto matrixSize = getMatrixSize(op.getOpCode());

    for (uint32_t i = 0u; i < op.getSrcCount(); i++) {
      const auto& src = op.getSrc(i);
      auto registerType = src.getRegisterType();

      uint32_t index = src.getIndex();
      uint32_t count = 1u;

      if (i && matrixSize)
        count = matrixSize->second;

      switch (registerType) {
        case RegisterType::eConstInt:
          m_constants.intCount = std::max(m_constants.intCount, index + count);

          for (uint32_t j = 0u; j < count; j++)
            setBit(constMaskI, index + j);
          break;

        case RegisterType::eConstBool: {
          m_constants.boolCount = std::max(m_constants.boolCount, index + count);

          if (!m_isSWVP || m_shaderInfo.getType() != ShaderType::eVertex)
            m_constants.boolMask |= 1u << index;
        } break;

        case RegisterType::eConst:
        case RegisterType::eConst2:
        case RegisterType::eConst3:
        case RegisterType::eConst4: {
          m_constants.floatCount = std::max(m_constants.floatCount, index + count);

          if (src.hasRelativeAddressing()) {
            // the SM3 PS float-constant ceiling is MaxSM3FloatConstantsPS (224), not MaxFloatConstantsPS
            uint32_t hwvpFloatConstantsCount = GetShaderInfo().getType() == ShaderType::ePixel ? MaxSM3FloatConstantsPS : MaxFloatConstantsVS;

            m_constants.floatCount = std::max(m_constants.floatCount,
              m_isSWVP ? MaxFloatConstantsSoftware : hwvpFloatConstantsCount);

            m_constants.floatsAccessedDynamically = true;
          } else {
            for (uint32_t j = 0u; j < count; j++)
              setBit(constMaskF, index + j);
          }
        } break;

        default: break;
      }
    }

    if (GetShaderInfo().getType() == ShaderType::ePixel
      && op.hasDst()
      && op.getDst().getRegisterType() == RegisterType::eColorOut) {
      m_usedRTs |= 1u << op.getDst().getIndex();
    }

    switch (op.getOpCode()) {
      case OpCode::eDef:
      case OpCode::eDefI:
      case OpCode::eDefB:
        if (!HandleDef(op, shaderDefs))
          return false;
        break;

      case OpCode::eTexLd:
      case OpCode::eTexBem:
      case OpCode::eTexBemL:
      case OpCode::eTexReg2Ar:
      case OpCode::eTexReg2Gb:
      case OpCode::eTexM3x2Tex:
      case OpCode::eTexM3x3Tex:
      case OpCode::eTexM3x3Spec:
      case OpCode::eTexM3x3VSpec:
      case OpCode::eTexReg2Rgb:
      case OpCode::eTexDp3Tex:
      case OpCode::eTexM3x2Depth:
      case OpCode::eTexDp3:
      case OpCode::eTexM3x3:
      case OpCode::eTexLdd:
      case OpCode::eTexLdl:
        if (!HandleTextureSample(op))
          return false;
        break;

      case OpCode::eDcl:
        if (!HandleDcl(op))
          return false;
        break;

      default: break;
    }

    return true;
  }

  bool D3D9ShaderAnalysis::HandleDef(
    const Instruction&                  op,
          D3D9ImmediateConstantsData&   shaderDefs) {
    dxbc_spv_assert(op.hasDst());
    uint32_t index = op.getDst().getIndex();

    if (op.getOpCode() == OpCode::eDef) {
      m_immediateConstants.floatCount = std::max(m_immediateConstants.floatCount, index + 1u);

      dxbc_spv_assert(op.hasImm());
      auto imm = op.getImm();

      Vector4 value = {
        imm.getImmediate<float>(0u), imm.getImmediate<float>(1u),
        imm.getImmediate<float>(2u), imm.getImmediate<float>(3u)
      };

      shaderDefs.floats.push_back({ index, value });
    } else if (op.getOpCode() == OpCode::eDefI) {
      m_immediateConstants.intCount = std::max(m_immediateConstants.intCount, index + 1u);
      shaderDefs.ints.push_back(index);
    } else if (op.getOpCode() == OpCode::eDefB) {
      m_immediateConstants.boolCount = std::max(m_immediateConstants.boolCount, index + 1u);
    } else {
      return false;
    }

    return true;
  }


  bool D3D9ShaderAnalysis::HandleTextureSample(const Instruction& op) {
    uint32_t samplerIndex;
    auto dst = op.getDst();
    Operand src1;
    if (op.getSrcCount() >= 2u)
      src1 = op.getSrc(1u);

    switch (op.getOpCode()) {
      case OpCode::eTexLd:
        if (GetShaderInfo().getVersion().first <= 1u)
          samplerIndex = dst.getIndex();
        else
          samplerIndex = src1.getIndex();
        break;

      case OpCode::eTexLdl:
      case OpCode::eTexLdd:
        samplerIndex = src1.getIndex();
        break;

      case OpCode::eTexReg2Ar:
      case OpCode::eTexReg2Gb:
      case OpCode::eTexReg2Rgb:
      case OpCode::eTexM3x2Tex:
      case OpCode::eTexM3x3Tex:
      case OpCode::eTexM3x3:
      case OpCode::eTexM3x2Depth:
      case OpCode::eTexM3x3Spec:
      case OpCode::eTexM3x3VSpec:
      case OpCode::eTexDp3Tex:
      case OpCode::eTexDp3:
      case OpCode::eTexBem:
      case OpCode::eTexBemL:
        samplerIndex = dst.getIndex();
        break;

      default: return false;
    }

    m_usedSamplers |= 1u << samplerIndex;

    return true;
  }


  bool D3D9ShaderAnalysis::HandleDcl(const Instruction& op) {
    dxbc_spv_assert(op.hasDcl());
    const auto& dcl = op.getDcl();
    dxbc_spv_assert(op.hasDst());
    const auto& dst = op.getDst();

    RegisterType registerType = dst.getRegisterType();
    uint32_t index = dst.getIndex();

    if (registerType == RegisterType::eSampler) {
      switch (dcl.getTextureType()) {
        case TextureType::eTexture3D:
          m_imageViewTypes[index] = MtlTextureViewType::e3D;
          break;
        case TextureType::eTextureCube:
          m_imageViewTypes[index] = MtlTextureViewType::eCube;
          break;
        case TextureType::eTexture2D:
        default:
          m_imageViewTypes[index] = MtlTextureViewType::e2D;
          break;
      }
      m_usedSamplers |= 1u << index;
      return true;
    }

    if (registerType == RegisterType::eInput && GetShaderInfo().getType() == ShaderType::eVertex) {
      // One attribute per semantic, in declaration order, as the converter
      // assigns input locations: a semantic declared again (on another
      // register or mask) shares the first declaration's attribute.
      Semantic semantic = { dcl.getSemanticUsage(), dcl.getSemanticIndex() };

      if (m_inputSignature.find(semantic) >= m_inputSignature.size())
        m_inputSignature.add(semantic);
      return true;
    }

    if (GetShaderInfo().getType() == ShaderType::ePixel
      && dcl.getSemanticUsage() == SemanticUsage::eColor
      && dcl.getSemanticIndex() < 2u) {
      Semantic semantic = { dcl.getSemanticUsage(), dcl.getSemanticIndex() };

      auto location = FindLocationInFixedFunctionIO(semantic);

      if (!location.has_value()) {
        dxbc_spv_unreachable();
        return false;
      }

      m_flatShadingMask |= 1u << *location;
    }

    return true;
  }


  static std::string HazardRegName(RegisterType type, uint32_t index) {
    switch (type) {
      case RegisterType::eTemp:       return str::format("r", index);
      case RegisterType::eInput:      return str::format("v", index);
      case RegisterType::eOutput:     return str::format("o", index);
      case RegisterType::eAttributeOut: return str::format("oD", index);
      case RegisterType::eColorOut:   return str::format("oC", index);
      case RegisterType::eDepthOut:   return "oDepth";
      case RegisterType::ePredicate:  return str::format("p", index);
      case RegisterType::eTexture:
      case RegisterType::ePixelTexCoord: return str::format("t", index);
      case RegisterType::eMiscType:
        return index == uint32_t(MiscTypeIndex::eMiscTypeFace) ? "vFace" : "vPos";
      default:                        return str::format("reg", uint32_t(type), "_", index);
    }
  }


  static std::string HazardMask(uint8_t mask, bool omitFull = true) {
    if (mask == 0xfu && omitFull)
      return std::string();

    std::string s = ".";
    for (uint32_t i = 0u; i < 4u; i++) {
      if (mask & (1u << i))
        s += "xyzw"[i];
    }
    return s;
  }


  static std::string HazardSemantic(Semantic semantic) {
    static const char* const s_names[] = {
      "position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent",
      "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample" };
    uint32_t usage = uint32_t(semantic.usage);

    return usage < sizeof(s_names) / sizeof(s_names[0])
      ? str::format(s_names[usage], semantic.index)
      : str::format("usage", usage, "_", semantic.index);
  }


  void D3D9ShaderAnalysis::ScanConverterHazards(
    const Instruction&                  op,
          D3D9IoDclList&                ioDcls) {
    const ShaderType stage = GetShaderInfo().getType();
    const uint32_t major = GetShaderInfo().getVersion().first;

    switch (op.getOpCode()) {
      case OpCode::eCrs:
      case OpCode::eSetP: {
        // Unpatched, Converter::handleCrs/handleSetP built one value per written
        // component but composite() indexed that list by component: only masks
        // that start at .x without gaps (and .xyzw for setp) came out right.
        const auto& dst = op.getDst();
        uint8_t mask = uint8_t(dst.getWriteMask(GetShaderInfo()));
        bool isCrs = op.getOpCode() == OpCode::eCrs;

        if (mask != 0x1u && mask != 0x3u && mask != 0x7u && (isCrs || mask != 0xfu)) {
          AddConverterHazard(str::format(isCrs ? "crs-partial-mask:" : "setp-partial-mask:",
            HazardRegName(dst.getRegisterType(), dst.getIndex()), HazardMask(mask, false)));
        }
      } break;

      case OpCode::eDcl: {
        // Mirrors IoMap::handleDclIoVar: which dcls declare I/O, and the
        // semantic it uses (SM1/2 varyings get one from the register).
        const auto& dst = op.getDst();
        RegisterType type = dst.getRegisterType();

        if (type != RegisterType::eAttributeOut && type != RegisterType::eOutput
         && type != RegisterType::eInput && type != RegisterType::eTexture
         && type != RegisterType::ePixelTexCoord && type != RegisterType::eMiscType
         && type != RegisterType::eColorOut)
          break;

        D3D9IoDclEntry entry = { };
        entry.registerType  = type;
        entry.registerIndex = dst.getIndex();
        entry.mask          = uint8_t(dst.getWriteMask(GetShaderInfo()));
        entry.isInput       = type == RegisterType::eInput
          || (type == RegisterType::eTexture && stage == ShaderType::ePixel)
          || type == RegisterType::eMiscType
          || type == RegisterType::ePixelTexCoord;

        const auto& dcl = op.getDcl();
        entry.semantic = Semantic { dcl.getSemanticUsage(), dcl.getSemanticIndex() };

        bool isVarying = entry.isInput == (stage == ShaderType::ePixel);

        if (isVarying && major < 3u) {
          if (type == RegisterType::eInput)
            entry.semantic = Semantic { SemanticUsage::eColor, entry.registerIndex };
          else if (type == RegisterType::eTexture || type == RegisterType::ePixelTexCoord)
            entry.semantic = Semantic { SemanticUsage::eTexCoord, entry.registerIndex };
          else if (type == RegisterType::eMiscType)
            entry.semantic = Semantic { SemanticUsage::ePosition, 0u };
        }

        AddIoDcl(ioDcls, entry, false);
      } break;

      default: {
        // IoMap::emitLoad/emitStore declare an undeclared v# (any stage) or
        // SM3 o# on first use, with the semantic determineSemanticForRegister
        // picks: color<n> for v#, texcoord<n> for o#.
        for (uint32_t i = 0u; i < op.getSrcCount(); i++) {
          const auto& src = op.getSrc(i);

          if (src.getRegisterType() == RegisterType::eInput && !src.hasRelativeAddressing()) {
            D3D9IoDclEntry entry = { RegisterType::eInput, src.getIndex(),
              Semantic { SemanticUsage::eColor, src.getIndex() }, 0xfu, true };
            AddIoDcl(ioDcls, entry, true);
          }
        }

        if (stage == ShaderType::eVertex && major >= 3u && op.hasDst()
         && op.getDst().getRegisterType() == RegisterType::eOutput
         && !op.getDst().hasRelativeAddressing()) {
          D3D9IoDclEntry entry = { RegisterType::eOutput, op.getDst().getIndex(),
            Semantic { SemanticUsage::eTexCoord, op.getDst().getIndex() }, 0xfu, false };
          AddIoDcl(ioDcls, entry, true);
        }
      } break;
    }
  }


  void D3D9ShaderAnalysis::AddIoDcl(
          D3D9IoDclList&                ioDcls,
          D3D9IoDclEntry                entry,
          bool                          isImplicit) {
    // The conflict test unpatched IoMap::dclIoVar asserted on.
    bool isRegularRegister = entry.registerType == RegisterType::eInput
                          || entry.registerType == RegisterType::eOutput;

    if (isImplicit) {
      for (const auto& e : ioDcls) {
        if (e.isInput == entry.isInput && e.registerType == entry.registerType
         && e.registerIndex == entry.registerIndex)
          return; // declared already, nothing to declare
      }
    }

    for (const auto& e : ioDcls) {
      if (e.isInput != entry.isInput)
        continue;

      bool sameRegister = e.registerType == entry.registerType && e.registerIndex == entry.registerIndex;

      if (sameRegister || (isRegularRegister && e.semantic == entry.semantic)) {
        AddConverterHazard(str::format("packed-io:",
          HazardRegName(e.registerType, e.registerIndex), HazardMask(e.mask), "=", HazardSemantic(e.semantic), "+",
          HazardRegName(entry.registerType, entry.registerIndex), HazardMask(entry.mask),
          isImplicit ? "(undeclared)=" : "=", HazardSemantic(entry.semantic)));
        break;
      }
    }

    ioDcls.push_back(entry);
  }


  void D3D9ShaderAnalysis::AddConverterHazard(const std::string& item) {
    if (!m_converterHazards.empty())
      m_converterHazards += ' ';

    m_converterHazards += item;
  }


  std::optional<uint32_t> D3D9ShaderAnalysis::FindLocationInFixedFunctionIO(dxbc_spv::sm3::Semantic semantic) const {
    return IoMap::findFixedFunctionLocation(semantic);
  }


  void D3D9ShaderAnalysis::setBit(ConstantMask& mask, uint32_t bit) {
    uint32_t index = bit / 32u;
    uint32_t shift = bit % 32u;

    if (index >= mask.size())
      mask.resize(index + 1u);

    mask[index] |= 1u << shift;
  }


  void D3D9ShaderAnalysis::clrBit(ConstantMask& mask, uint32_t bit) {
    uint32_t index = bit / 32u;
    uint32_t shift = bit % 32u;

    if (index < mask.size())
      mask[index] &= ~(1u << shift);
  }

}
