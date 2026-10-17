#include "d3d9_shader.h"
#include "d3d9_shader_compiler.h"
#include "d3d9_device.h"

#include "../util/util_env.h"

namespace dxvk {

  static void ApplyCbvSlotMap(MtlShaderFunction* fn, const D3D9CbvSlotMap& map) {
    for (size_t i = 0u; i < fn->cbvSlotMap.size(); i++)
      fn->cbvSlotMap[i] = (i < map.slot.size()) ? map.slot[i] : int8_t(-1);

    fn->abTableIndex = map.abTableIndex;
    fn->abWords.clear();
    if (map.abUsed) {
      fn->abWords.reserve(map.abWords.size());
      for (const auto& w : map.abWords)
        fn->abWords.push_back({ MtlShaderFunction::AbWordKind(w.kind), w.reg });
    }
  }


  static void CopyFunction(MtlShaderFunction* dst, const MtlShaderFunction* src) {
    dst->library      = src->library;
    dst->entryName    = src->entryName;
    dst->cbvSlotMap   = src->cbvSlotMap;
    dst->abTableIndex = src->abTableIndex;
    dst->abWords      = src->abWords;
  }


  /**
   * \brief Compiles one function future of a program
   *
   * Holds its program, the paired program and the function until it has
   * run, then drops them (the function keeps the task, not the reverse).
   */
  class D3D9ShaderFunctionTask : public MtlCompileTask {

  public:

    D3D9ShaderFunctionTask(
            D3D9ShaderProgram*    pProgram,
      const D3D9ShaderProgram*    pPrev,
            MtlShaderFunction*    pFn)
    : m_program(pProgram)
    , m_prev(const_cast<D3D9ShaderProgram*>(pPrev))
    , m_fn(pFn) { }

  protected:

    void run() override {
      Compile();

      m_fn      = nullptr;
      m_prev    = nullptr;
      m_program = nullptr;
    }

  private:

    Rc<D3D9ShaderProgram> m_program;
    Rc<D3D9ShaderProgram> m_prev;
    Rc<MtlShaderFunction> m_fn;

    void Compile() {
      // Phase A of both stages; runs them here if no worker has yet.
      m_program->wait();

      if (m_prev != nullptr)
        m_prev->wait();

      const auto& backend = m_program->m_backend;

      if (backend == nullptr)
        return;

      ID3D9MetalShaderBackend::PairedStageIo paired = { };

      if (m_prev != nullptr && m_prev->m_backend != nullptr)
        paired.prevStageIr = &m_prev->m_backend->serializedIr();

      std::vector<uint8_t> ir = backend->getFinalIr(paired);

      if (ir.empty()) {
        Fallback(nullptr);
        return;
      }

      Sha1Hash hash = Sha1Hash::compute(ir.data(), ir.size());
      Rc<MtlShaderFunction> owner = m_program->ClaimIr(hash, m_fn);

      if (owner != m_fn) {
        // Same IR as another function of this shader (typically: the PS
        // reads nothing the VS doesn't write, so linking changed nothing).
        // The owner never waits on us, so this cannot deadlock.
        owner->task->wait();
        CopyFunction(m_fn.ptr(), owner.ptr());

        if (compile_stats::g_enabled)
          compile_stats::g_shared.add(0u);

        if (!m_fn->library)
          Fallback(&hash);
        return;
      }

      const uint64_t t0 = compile_stats::g_enabled ? compile_stats::nowNs() : 0u;

      D3D9CbvSlotMap cbvMap;
      std::vector<uint8_t> metallib = backend->compileMetallib(ir, &cbvMap);

      Rc<MtlShaderFunction> lib;

      if (!metallib.empty()) {
        lib = m_program->m_device->createShaderFunction(
          m_program->m_stage, metallib.data(), metallib.size());
      }

      if (compile_stats::g_enabled)
        compile_stats::g_shaders.add(compile_stats::nowNs() - t0);

      if (lib == nullptr) {
        Logger::err(str::format("D3D9 shader: ", metallib.empty() ? "AIR compile" : "newLibrary",
          " failed for ", m_program->m_name, m_prev != nullptr ? " (paired)" : ""));
        Fallback(&hash);
        return;
      }

      m_fn->library = lib->library;
      ApplyCbvSlotMap(m_fn.ptr(), cbvMap);
    }

    // A failed linked PS falls back to the standalone PS, unless that is
    // the very IR that just failed (whose owner may be waiting on us).
    void Fallback(const Sha1Hash* pFailedHash) {
      if (m_prev == nullptr)
        return;

      if (pFailedHash != nullptr) {
        std::vector<uint8_t> ir = m_program->m_backend->getFinalIr({ });

        if (ir.empty() || Sha1Hash::compute(ir.data(), ir.size()) == *pFailedHash)
          return;
      }

      Rc<MtlShaderFunction> standalone = m_program->GetFunction(nullptr);
      standalone->task->wait();
      CopyFunction(m_fn.ptr(), standalone.ptr());
    }

  };


  D3D9ShaderProgram::D3D9ShaderProgram(
          MtlDevice*                pDevice,
    const std::string&              Name,
          MtlShaderStage            Stage,
    const D3D9ShaderOptions&        Options,
    const D3D9ShaderAnalysis&       Analysis,
    const void*                     pBytecode)
  : m_device(pDevice), m_name(Name), m_stage(Stage), m_options(Options), m_analysis(Analysis)
  , m_bytecode(reinterpret_cast<const uint8_t*>(pBytecode),
               reinterpret_cast<const uint8_t*>(pBytecode) + Analysis.GetLength()) {
    // D9MT_PS_PREFETCH=0: compile pixel shaders only once a draw needs them
    // (default: right after Phase A, like vertex shaders, since a PS linked
    // against a compatible VS reuses the standalone library).
    static const bool s_psPrefetch = env::getEnvVar("D9MT_PS_PREFETCH") != "0";

    if (Stage == MtlShaderStage::Vertex || s_psPrefetch) {
      Rc<MtlShaderFunction> fn = CreateFunction(nullptr);
      m_functions.emplace(nullptr, fn);
      m_prefetch = fn->task;
    }
  }


  Rc<MtlShaderFunction> D3D9ShaderProgram::GetFunction(const D3D9ShaderProgram* pPrev) {
    Rc<MtlShaderFunction> fn;

    { std::lock_guard lock(m_mutex);
      auto& slot = m_functions[pPrev];

      if (slot != nullptr)
        return slot;

      slot = fn = CreateFunction(pPrev);
    }

    m_device->submitCompile(fn->task, true);
    return fn;
  }


  Rc<MtlShaderFunction> D3D9ShaderProgram::CreateFunction(const D3D9ShaderProgram* pPrev) {
    Rc<MtlShaderFunction> fn = new MtlShaderFunction();
    fn->stage = m_stage;
    fn->task  = new D3D9ShaderFunctionTask(this, pPrev, fn.ptr());
    return fn;
  }


  Rc<MtlShaderFunction> D3D9ShaderProgram::ClaimIr(const Sha1Hash& Hash, const Rc<MtlShaderFunction>& Fn) {
    std::lock_guard lock(m_mutex);
    return m_byIr.emplace(Hash, Fn).first->second;
  }


  void D3D9ShaderProgram::run() {
    Logger::debug(str::format("Compiling shader ", m_name));

    m_backend = createD3D9MetalShader(m_name, m_stage, m_options, m_analysis, m_bytecode.data());

    if (m_backend == nullptr)
      Logger::err(str::format("D3D9 shader: Phase-A build failed for ", m_name));

    m_bytecode = std::vector<uint8_t>();

    // Queue (not run) the standalone compile: it waits on this task.
    if (m_prefetch != nullptr)
      m_device->submitCompile(std::exchange(m_prefetch, nullptr), false);
  }


  D3D9CommonShader::D3D9CommonShader(
          D3D9DeviceEx*             pDevice,
    const D3D9ShaderModuleKey&      ShaderKey,
          D3D9ShaderAnalysis&&      Analysis,
    const D3D9ShaderCreateInfo&     ModuleInfo,
    const void*                     pShaderBytecode)
  : m_analysis(std::move(Analysis)) {
    const MtlShaderStage stage = ShaderKey.stage();

    m_name = str::format(stage == MtlShaderStage::Vertex ? "vs_" : "ps_", ShaderKey.hash());

    // Once per distinct shader: patterns dxbc-spirv's sm3 converter aborted on
    // or miscompiled before patches/dxbc-spirv (issue #8), so logs show
    // whether a game uses them.
    if (unlikely(!m_analysis.GetConverterHazards().empty())) {
      Logger::warn(str::format("D3D9 SM3 hazard: ", m_name, " ",
        stage == MtlShaderStage::Vertex ? "VS" : "PS", " ", m_analysis.GetConverterHazards()));
    }

    // The game may free the bytecode once Create*Shader returns; the
    // program keeps its own copy until Phase A has run.
    m_program = new D3D9ShaderProgram(pDevice->GetDXVKDevice().ptr(),
      m_name, stage, ModuleInfo.shaderOptions, m_analysis, pShaderBytecode);

    pDevice->GetDXVKDevice()->submitCompile(m_program, false);
  }


  HRESULT D3D9ShaderModuleSet::GetShaderModule(
          D3D9DeviceEx*           pDevice,
    const D3D9ShaderModuleKey&    ShaderKey,
          D3D9ShaderAnalysis&&    ShaderAnalysis,
    const D3D9ShaderCreateInfo&   ModuleInfo,
    const void*                   pShaderBytecode,
          D3D9CommonShader*       pShader) {
    std::lock_guard lock(m_mutex);

    auto entry = m_modules.find(ShaderKey);
    if (entry != m_modules.end()) {
      if (pShader)
        *pShader = entry->second;
      return D3D_OK;
    }

    D3D9CommonShader module(pDevice, ShaderKey, std::move(ShaderAnalysis), ModuleInfo, pShaderBytecode);

    auto result = m_modules.emplace(ShaderKey, std::move(module));
    if (pShader)
      *pShader = result.first->second;

    return D3D_OK;
  }

}
