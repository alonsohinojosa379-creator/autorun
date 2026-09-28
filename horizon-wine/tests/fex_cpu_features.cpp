#include <Windows/Common/CPUFeatures.h>
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <vector>

using namespace FEXCore::X86State;

class Syscalls final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame*) override { std::abort(); }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0x100000, 0x110000, true};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

static uint32_t reference_crc(uint32_t crc, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; i++) {
    crc ^= static_cast<uint8_t>(value);
    for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
    value >>= 8;
  }
  return crc;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const bool wide = std::atoi(argv[1]) == 64;
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, wide ? "1" : "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, "0");
  LogMan::Throw::InstallHandler([](const char* message) { fprintf(stderr, "%s\n", message); std::abort(); });
  auto features = FEX::Windows::CPUFeatures::FetchHostFeatures(true, wide ? FEXCore::HostFeatures::HostTypeEnum::Arm64ec :
                                                                          FEXCore::HostFeatures::HostTypeEnum::Wow64);
  assert(features.SupportsCRC);
  assert(!features.SupportsAES && !features.SupportsSHA && !features.SupportsPMULL_128Bit);
  assert(!features.SupportsAtomics && !features.SupportsSVE128 && !features.SupportsSVE256);
  {
    auto baseline = features;
    baseline.SupportsCRC = false;
    auto context = FEXCore::Context::Context::CreateNewContext(baseline);
    FEX::Windows::CPUFeatures windows(*context);
    assert(!(context->RunCPUIDFunction(1, 0).ecx & (1u << 20)));
    assert(!windows.IsFeaturePresent(PF_SSE4_2_INSTRUCTIONS_AVAILABLE));
  }
  Syscalls syscalls;
  FEXCore::SignalDelegator signals;
  auto context = FEXCore::Context::Context::CreateNewContext(features);
  context->SetSignalDelegator(&signals);
  context->SetSyscallHandler(&syscalls);
  context->EnableExitOnHLT();
  assert(context->InitCore());
  assert(context->RunCPUIDFunction(1, 0).ecx & (1u << 20));
  FEX::Windows::CPUFeatures windows(*context);
  assert(windows.IsFeaturePresent(PF_SSE4_2_INSTRUCTIONS_AVAILABLE));
  SYSTEM_CPU_INFORMATION info {};
  windows.UpdateInformation(&info);
  assert(info.ProcessorFeatureBits & CPU_FEATURE_SSE42);

  auto memory = mmap(reinterpret_cast<void*>(0x100000), 0x10000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(memory != MAP_FAILED);
  unsigned tests = 0;
  for (unsigned width : {1, 2, 4, 8}) {
    if (!wide && width == 8) continue;
    for (bool indirect : {false, true}) {
      const uintptr_t entry = 0x100000 + tests++ * 64, operand = 0x108ffd;
      std::vector<uint8_t> code;
      if (width == 2) code.push_back(0x66);
      code.push_back(0xf2);
      if (width == 8) code.push_back(0x48);
      code.insert(code.end(), {0x0f, 0x38, static_cast<uint8_t>(width == 1 ? 0xf0 : 0xf1),
                              static_cast<uint8_t>(indirect ? 0x01 : 0xc1), 0xf4});
      memcpy(reinterpret_cast<void*>(entry), code.data(), code.size());
      auto thread = context->CreateThread();
      auto& state = thread->CurrentFrame->State;
      FEXCore::Core::CPUState::gdt_segment segments[32] {};
      segments[0].D = !wide;
      segments[0].L = wide;
      FEXCore::Core::CPUState::SetGDTLimit(&segments[0], 0xfffff);
      state.segment_arrays[0] = state.segment_arrays[1] = segments;
      state.callret_sp = 0x10e000;
      uint64_t value = 0x0123456789abcdefULL;
      for (unsigned i = 0; i < 128; i++) {
        const uint32_t crc = static_cast<uint32_t>(value >> 32);
        const uint64_t source = indirect ? operand : wide ? value : static_cast<uint32_t>(value);
        memcpy(reinterpret_cast<void*>(operand), &value, sizeof(value));
        state.rip = entry;
        state.gregs[REG_RSP] = 0x10f000;
        state.gregs[REG_RAX] = crc | (wide ? 0xabcdef1200000000ULL : 0);
        state.gregs[REG_RCX] = source;
        context->SetFlagsFromCompactedEFLAGS(thread, 0x246);
        context->ExecuteThread(thread);
        assert(state.gregs[REG_RAX] == reference_crc(crc, value, width));
        assert(context->ReconstructCompactedEFLAGS(thread, false, nullptr, 0) == 0x246);
        assert(state.gregs[REG_RCX] == source);
        value = value * 6364136223846793005ULL + 1442695040888963407ULL;
      }
      context->DestroyThread(thread);
    }
  }
  context.reset();
  assert(!munmap(memory, 0x10000));
  printf("FEX %u-bit: Horizon CRC capability, SSE4.2 CPUID/Windows queries and %u CRC32 cases passed\n",
         wide ? 64 : 32, tests * 128);
}
