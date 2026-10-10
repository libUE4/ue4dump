#include "AutoFix.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

#include "../../AutoFix/Arm64Decode.hpp"
#include "../UEMemory.hpp"
#include "../UEWrappers.hpp"

using namespace UEMemory;

AutoFixProfile::AutoFixProfile() = default;

AutoFixProfile::AutoFixProfile(const std::string &targetPackage)
    : _packageHint(targetPackage)
{
}

bool AutoFixProfile::ArchSupprted() const
{
    auto e_machine = GetUnrealELF().header().e_machine;
    return e_machine == EM_AARCH64;
}

std::string AutoFixProfile::GetAppName() const
{
    return "Auto (UE4/UE5 Generic)";
}

std::vector<std::string> AutoFixProfile::GetAppIDs() const
{
    if (!_packageHint.empty())
        return {_packageHint};
    return {"<auto>"};
}

AutoFixProfile::UEFamily AutoFixProfile::ScanVersionString() const
{
    if (_packageHint == "com.tencent.ig"){
        _versionStr = "4.18";
         return UEFamily::UE4_18_19;
    }
       

    static const char *kNeedles[] = {
        "++UE4+Release-",
        "+Release-",
    };

    auto resolveFamily = [this](const char *needle, int major, int minor) -> UEFamily {
        _versionStr = std::string(needle) + std::to_string(major) + "." + std::to_string(minor);
        if (major == 4)
        {
            if (minor <= 17) return UEFamily::UE4_00_17;
            if (minor <= 19) return UEFamily::UE4_18_19;
            if (minor == 20) return UEFamily::UE4_20;
            if (minor == 21) return UEFamily::UE4_21;
            if (minor == 22) return UEFamily::UE4_22;
            if (minor <= 24) return UEFamily::UE4_23_24;
            return UEFamily::UE4_25_27;
        }
        return UEFamily::Unknown;
    };

    auto scanRange = [&](uintptr_t startAddr, size_t length) -> UEFamily
    {
        if (length < 32) return UEFamily::Unknown;

        constexpr size_t kChunk = 0x100000;
        constexpr size_t kTail = 64;
        std::vector<uint8_t> buf(kChunk + kTail);

        for (size_t off = 0; off < length; off += kChunk)
        {
            size_t toRead = std::min<size_t>(kChunk + kTail, length - off);
            if (!vm_rpm_ptr((void *)(startAddr + off), buf.data(), toRead))
                continue;

            for (size_t ni = 0; ni < sizeof(kNeedles) / sizeof(kNeedles[0]); ++ni)
            {
                const char *needle = kNeedles[ni];
                const size_t nlen = strlen(needle);
                if (toRead < nlen + 4) continue;

                for (size_t i = 0; i + nlen + 4 < toRead; ++i)
                {
                    if (memcmp(buf.data() + i, needle, nlen) != 0) continue;

                    int major = 0, minor = 0;
                    if (sscanf((const char *)(buf.data() + i + nlen), "%d.%d", &major, &minor) != 2)
                        continue;
                    if (major != 4) continue;
                    return resolveFamily(needle, major, minor);
                }
            }
        }
        return UEFamily::Unknown;
    };

    auto ue_elf = GetUnrealELF();
    if (ue_elf.isValid())
    {
        for (const auto &seg : ue_elf.segments())
        {
            if (!seg.readable) continue;
            UEFamily fam = scanRange(seg.startAddress, seg.length);
            if (fam != UEFamily::Unknown) return fam;
        }
    }

    const auto maps = KittyMemoryEx::getAllMaps(kMgr.processID());
    for (const auto &m : maps)
    {
        if (!m.readable || !m.writeable || m.executable) continue;
        UEFamily fam = scanRange(m.startAddress, m.length);
        if (fam != UEFamily::Unknown) return fam;
    }

    return UEFamily::Unknown;
}

AutoFixProfile::UEFamily AutoFixProfile::GuessMajorFromSoName() const
{
    // Epic 在 UE5 起把动态库统一改名为 libUnreal.so，UE4 时代是 libUE4.so。
    // 直接据此映射到一个保守的 fallback 大版本即可；具体小版本由 ScanVersionString
    // 的精确路径优先决定。
    auto ue_elf = GetUnrealELF();
    if (!ue_elf.isValid()) return UEFamily::Unknown;

    const std::string elfPath = ue_elf.filePath();
    if (elfPath.find("libUnreal.so") != std::string::npos)
        return UEFamily::UE5_03;
    if (elfPath.find("libUE4.so") != std::string::npos)
        return UEFamily::UE4_25_27;
    return UEFamily::Unknown;
}

void AutoFixProfile::DetectVersion() const
{
    if (_versionDetected) return;
    _versionDetected = true;

    const UEFamily soHint = GuessMajorFromSoName();
    if (soHint == UEFamily::UE5_03)
    {
        _family = UEFamily::UE5_03;
        _versionStr = "libUnreal.so (UE5)";
        LOGI("[AutoFix] SO name suggests UE5, using UE5_03");
    }
    else
    {
        _family = ScanVersionString();
        if (_family == UEFamily::Unknown)
        {
            LOGW("[AutoFix] UE version string not found; fallback to UE4.25-4.27");
            _family = UEFamily::UE4_25_27;
        }
        else
        {
            LOGI("[AutoFix] UE version: %s", _versionStr.c_str());
        }
    }

    switch (_family)
    {
    case UEFamily::UE4_00_17:
    case UEFamily::UE4_18_19:
    case UEFamily::UE4_20:
        _useFNamePool = false; _outlineNumber = false; break;
    case UEFamily::UE4_21:
    case UEFamily::UE4_22:
    case UEFamily::UE4_23_24:
    case UEFamily::UE4_25_27:
    case UEFamily::UE5_00_02:
        _useFNamePool = true; _outlineNumber = false; break;
    case UEFamily::UE5_03:
        _useFNamePool = true; _outlineNumber = true; break;
    default: break;
    }
    _casePreserving = false;
}

void AutoFixProfile::EnsureOffsetsInited() const
{
    if (_offsetsInited) return;
    _offsetsInited = true;
    DetectVersion();

    switch (_family)
    {
    case UEFamily::UE4_00_17:
        _offsets = UE_DefaultOffsets::UE4_00_17(_casePreserving); break;
    case UEFamily::UE4_18_19:
        _offsets = UE_DefaultOffsets::UE4_18_19(_casePreserving); break;
    case UEFamily::UE4_20:
        _offsets = UE_DefaultOffsets::UE4_20(_casePreserving); break;
    case UEFamily::UE4_21:
        _offsets = UE_DefaultOffsets::UE4_21(_casePreserving); break;
    case UEFamily::UE4_22:
        _offsets = UE_DefaultOffsets::UE4_22(_casePreserving); break;
    case UEFamily::UE4_23_24:
        _offsets = UE_DefaultOffsets::UE4_23_24(_casePreserving); break;
    case UEFamily::UE4_25_27:
        _offsets = UE_DefaultOffsets::UE4_25_27(_casePreserving); break;
    case UEFamily::UE5_00_02:
        _offsets = UE_DefaultOffsets::UE5_00_02(_casePreserving, _outlineNumber); break;
    case UEFamily::UE5_03:
        _offsets = UE_DefaultOffsets::UE5_03(_casePreserving, _outlineNumber); break;
    default:
        _offsets = UE_DefaultOffsets::UE4_25_27(_casePreserving); break;
    }
}

UE_Offsets *AutoFixProfile::GetOffsets() const
{
    EnsureOffsetsInited();
    return &_offsets;
}

namespace
{
    static std::string ReadEntryString(uint8_t *entry, const UE_Offsets *off, bool useFNamePool)
    {
        if (!entry) return "";

        if (!useFNamePool)
        {
            uint8_t *pStr = entry + off->FNameEntry.Name;
            return vm_rpm_str(pStr, kMAX_UENAME_BUFFER);
        }
        uint16_t header = 0;
        if (!vm_rpm_ptr(entry + off->FNamePoolEntry.Header, &header, sizeof(uint16_t)))
            return "";
        if (!off->FNamePoolEntry.GetLength) return "";
        size_t strLen = std::min<size_t>(off->FNamePoolEntry.GetLength(header), kMAX_UENAME_BUFFER);
        if (strLen == 0 || strLen >= kMAX_UENAME_BUFFER) return "";
        uint8_t *pStr = entry + off->FNamePoolEntry.Header + sizeof(int16_t);
        return vm_rpm_str(pStr, strLen);
    }

    static uint8_t *ResolveEntryAt(uintptr_t namesPtr, int32_t id, const UE_Offsets *off, bool useFNamePool)
    {
        if (id < 0 || namesPtr == 0) return nullptr;

        if (!useFNamePool)
        {
            uintptr_t gNames = vm_rpm_ptr<uintptr_t>((void *)namesPtr);
            if (!kPtrValidator.isPtrReadable(gNames)) return nullptr;
            const int32_t kPerChunk = 16384;
            const int32_t chunkIdx = id / kPerChunk;
            const int32_t within = id % kPerChunk;
            uint8_t *arr = vm_rpm_ptr<uint8_t *>((void *)(gNames + chunkIdx * sizeof(uintptr_t)));
            if (!kPtrValidator.isPtrReadable(arr)) return nullptr;
            return vm_rpm_ptr<uint8_t *>(arr + within * sizeof(uintptr_t));
        }
        uintptr_t blockBit = off->FNamePool.BlocksBit;
        uintptr_t blocks = off->FNamePool.BlocksOff;
        uintptr_t mask = (1u << blockBit) - 1;
        uintptr_t stride = off->FNamePool.Stride;
        if (blockBit == 0 || stride == 0) return nullptr;
        uintptr_t blockOff = ((uintptr_t)id >> blockBit) * sizeof(void *);
        uintptr_t inside = ((uintptr_t)id & mask) * stride;
        uint8_t *chunk = vm_rpm_ptr<uint8_t *>((void *)(namesPtr + blocks + blockOff));
        if (!kPtrValidator.isPtrReadable(chunk)) return nullptr;
        return chunk + inside;
    }

    static bool VerifyNamesAtCandidate(uintptr_t candidate, const UE_Offsets *off, bool useFNamePool)
    {
        struct Probe { int32_t id; const char *expect; };
        static const Probe kProbes[] = {
            {0, "None"}, {2, "ByteProperty"}, {3, "IntProperty"},
            {4, "BoolProperty"}, {5, "FloatProperty"},
        };
        int hits = 0;
        for (const auto &p : kProbes)
        {
            uint8_t *e = ResolveEntryAt(candidate, p.id, off, useFNamePool);
            if (!e) continue;
            std::string s = ReadEntryString(e, off, useFNamePool);
            if (s == p.expect) ++hits;
            if (hits >= 1) return true;
        }
        return false;
    }
}

uintptr_t AutoFixProfile::GetGUObjectArrayPtr() const
{
    return IGameProfile::GetGUObjectArrayPtr();
}

uintptr_t AutoFixProfile::GetFrameCount() const
{
    auto ue_elf = GetUnrealELF();
    if (_packageHint == "com.tencent.tmgp.nz" && ue_elf.isValid())
    {
        // 逆战：未来当前 UE4.24: GFrameCounter 在 UE .bss 内。
        // 指令特征：ldr w9, [base+0xcfc]; add w9,#1; str w9,[base+0xcfc]
        // 地址 = libUE4 base + 0x1e1e1cfc，运行时会随帧数递增。
        const uintptr_t candidate = ue_elf.base() + 0x1e1e1cfcULL;
        const uint32_t v0 = vm_rpm_ptr<uint32_t>((void *)candidate);
        if (v0 > 0)
            return candidate;
    }

    struct Pattern
    {
        const char *ida;
        int step;
    };

    static const Pattern kPatterns[] = {
        {"C0 03 5F D6 ? ? ? ? 08 ? ? ? 00 01 40 F9 C0 03 5F D6 FD 7B BF A9", 4},
        {"? ? ? F0 ? ? ? F9 ? ? ? F9 C0 03 5F D6 ? ? ? A9 FD 03 00 91 ? ? ? D0", 0},
        {"? ? ? F0 ? ? ? F9 ? ? ? F9 C0 03 5F D6 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 F3 03 01 2A", 0},
    };

    PATTERN_MAP_TYPE mapType = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
    for (const auto &pattern : kPatterns)
    {
        uintptr_t frameOff = Arm64::Decode_ADRP_LDR(findIdaPattern(mapType, pattern.ida, pattern.step));
        if (frameOff != 0) return vm_rpm_ptr<uintptr_t>((void *)frameOff);
    }

    return 0;
}

uintptr_t AutoFixProfile::GetMatrix() const
{
    struct Pattern
    {
        const char *ida;
        int step;
    };

    static const Pattern kPatterns[] = {
        {"08 3D 40 F9 00 01 3F D6 E8 03 13 AA ? ? ? F9", 0x18},
        {"FD ? ? A9 28 ? ? F9 F3 ? ? F8 C0 03 5F D6", -0x1c},
        {"00 01 3F D6 E8 03 13 AA 60 ? 00 F9 ? ? ? A9", 14},
        {"? ? ? D0 ? ? ? F9 ? ? ? 32 6B 7D 40 93 6C 25 0A 9B ? ? ? F9 BF 01 08 EB ? ? ? 54 6B 25 0A 9B ? ? ? B9 ? ? ? 31 ? ? ? 54 ? ? ? 14 ? ? ? B4 ? ? ? 32 68 25 08 9B ? ? ? F9 ? ? ? 14", 0},
    };

    PATTERN_MAP_TYPE mapType = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
    for (const auto &pattern : kPatterns)
    {
        uintptr_t addr = Arm64::Decode_ADRP_LDR(findIdaPattern(mapType, pattern.ida, pattern.step));
        if (addr != 0) return addr;
    }

    return 0;
}

uintptr_t AutoFixProfile::GetPhysx() const
{
    if (_packageHint == "com.tencent.tmgp.nz")
    {
        struct TArrayView
        {
            uintptr_t data = 0;
            int32_t count = 0;
            int32_t max = 0;
        };

        auto isPossiblePtr = [](uintptr_t ptr) -> bool {
            return ptr >= 0x10000 && ptr < 0x8000000000ULL && (ptr % sizeof(uintptr_t)) == 0;
        };

        auto isReadablePtr = [&](uintptr_t ptr, size_t len = sizeof(uintptr_t)) -> bool {
            return isPossiblePtr(ptr) && kPtrValidator.isPtrReadable(ptr, len);
        };

        auto readArray = [&](uintptr_t address, TArrayView &out) -> bool {
            return kPtrValidator.isPtrReadable(address, sizeof(TArrayView)) &&
                   vm_rpm_ptr((void *)address, &out, sizeof(out));
        };

        auto isValidArray = [&](const TArrayView &arr, int32_t maxLimit) -> bool {
            if (!isReadablePtr(arr.data))
                return false;
            if (arr.count <= 0 || arr.max <= 0 || arr.count > arr.max || arr.max > maxLimit)
                return false;

            const int32_t checkCount = arr.count < 4 ? arr.count : 4;
            return kPtrValidator.isPtrReadable(arr.data, static_cast<size_t>(checkCount) * sizeof(uintptr_t));
        };

        auto scoreActorArray = [&](const TArrayView &actors) -> int {
            if (!isValidArray(actors, 0x500000))
                return 0;

            const int sampleCount = actors.count < 128 ? actors.count : 128;
            int readableActors = 0;
            int typeHits = 0;
            int dynamicHits = 0;
            int staticHits = 0;

            for (int i = 0; i < sampleCount; ++i)
            {
                const uintptr_t actor = vm_rpm_ptr<uintptr_t>((void *)(actors.data + (i * sizeof(uintptr_t))));
                if (!isReadablePtr(actor + 0x18, sizeof(uint16_t)))
                    continue;

                ++readableActors;
                const uint16_t actorType = vm_rpm_ptr<uint16_t>((void *)(actor + 0x18));
                if (actorType == 6 || actorType == 7)
                {
                    ++typeHits;
                    if (actorType == 6) ++dynamicHits;
                    if (actorType == 7) ++staticHits;
                }
            }

            if (readableActors < 2 || typeHits < 2)
                return 0;

            int score = 8 + typeHits * 2;
            if (dynamicHits > 0) score += 3;
            if (staticHits > 0) score += 3;
            if (typeHits * 2 >= readableActors) score += 4;
            if (actors.count > 50) score += 4;
            if (actors.count > 500) score += 4;
            return score;
        };

        const ElfScanner ueElf = GetUnrealELF();
        if (ueElf.isValid())
        {
            // 逆战：未来当前局内 PhysX 全局槽位。
            // 局内结构扫描命中:
            //   slot          = base + 0x1E5D7F20
            //   instance      = *slot
            //   scene array   = instance + 0xF0
            //   rigid actors  = scene + 0x35E8
            //   actor type    = actor + 0x18
            const uintptr_t slot = ueElf.base() + 0x1e5d7f20ULL;
            const uintptr_t instance = kPtrValidator.isPtrReadable(slot, sizeof(uintptr_t))
                                           ? vm_rpm_ptr<uintptr_t>((void *)slot)
                                           : 0;

            TArrayView scenes{};
            if (isReadablePtr(instance + 0xF0, sizeof(TArrayView)) &&
                readArray(instance + 0xF0, scenes) &&
                isValidArray(scenes, 128))
            {
                const int sceneCount = scenes.count < 8 ? scenes.count : 8;
                int bestScore = 0;
                int bestActorCount = 0;
                uintptr_t bestScene = 0;

                for (int i = 0; i < sceneCount; ++i)
                {
                    const uintptr_t scene = vm_rpm_ptr<uintptr_t>((void *)(scenes.data + (i * sizeof(uintptr_t))));
                    if (!isReadablePtr(scene + 0x35E8, sizeof(TArrayView)))
                        continue;

                    TArrayView actors{};
                    if (!readArray(scene + 0x35E8, actors))
                        continue;

                    const int score = scoreActorArray(actors);
                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestActorCount = actors.count;
                        bestScene = scene;
                    }
                }

                if (bestScore >= 24)
                {
                    LOGI("[AutoFix][NZ] Physx found: slot=0x%lx rva=0x%lx instance=0x%lx scenes=%d bestScene=0x%lx actors=%d score=%d",
                         static_cast<unsigned long>(slot),
                         static_cast<unsigned long>(slot - ueElf.base()),
                         static_cast<unsigned long>(instance),
                         scenes.count,
                         static_cast<unsigned long>(bestScene),
                         bestActorCount,
                         bestScore);
                    return slot;
                }
            }
        }
    }

    struct Pattern
    {
        const char *ida;
        int step;
        int decodeStep;
        bool dereference;
    };

    static const Pattern kPatterns[] = {
        {"E1 ? ? ? 40 00 40 BD F4 03 02 AA", 0x28, 0x8, true},
        {"48 ? ? ? F3 03 04 AA F5 03 03 2A", -0xc, 0x8, true},
        {"? ? ? B0 00 03 3F D6", 0x0, 0x20, false},
    };

    PATTERN_MAP_TYPE mapType = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
    for (const auto &pattern : kPatterns)
    {
        uintptr_t addr = Arm64::Decode_ADRP_LDR(findIdaPattern(mapType, pattern.ida, pattern.step), pattern.decodeStep);
        if (addr == 0) continue;
        return pattern.dereference ? vm_rpm_ptr<uintptr_t>((void *)addr) : addr;
    }

    return 0;
}

uintptr_t AutoFixProfile::GetNamesPtr() const
{
    return IGameProfile::GetNamesPtr();
}
