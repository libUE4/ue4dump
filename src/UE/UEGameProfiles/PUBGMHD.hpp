#pragma once

#include <cstring>

#include "../UEGameProfile.hpp"
using namespace UEMemory;

class PUBGMHDProfile : public IGameProfile
{
public:
    PUBGMHDProfile() = default;

    bool ArchSupprted() const override
    {
        auto e_machine = GetUnrealELF().header().e_machine;
        return e_machine == EM_AARCH64;
    }

    std::string GetAppName() const override
    {
        return "和平精英";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.tencent.tmgp.pubgmhd"};
    }

    bool isUsingCasePreservingName() const override
    {
        return false;
    }

    bool IsUsingFNamePool() const override
    {
        return false;
    }

    bool isUsingOutlineNumberName() const override
    {
        return false;
    }

        uintptr_t GetGUObjectArrayPtr() const override
    {
        return IGameProfile::GetGUObjectArrayPtr();
    }
    void print_func() const
    {
        auto base = GetUEVars()->GetBaseAddress();
        //printf("0x%lx\n", base);
        std::string CalShootTargetLocationBP = "60 1C A3 4E 81 1C A4 4E A2 1C A5 4E C0 03 5F D6 ? ? ? F9";
        std::string DecArray = "0F 11 00 B9 1B 21 00 B9 0E 31 00 B9 0F 41 00 B9 1B 51 00 B9 18 61 00 B9 19 71 00 B9 1A 81 00 B9 17 91 00 B9 0B A1 00 B9 1F B1 00 B9 1F C1 00 B9 0D D1 00 B9 0A E1 00 B9 08 C1 03 91";
        std::string GetAccessoriesDeviationFactor = "? ? ? BD 81 09 21 1E 00 28 21 1E 40 09 20 1E";
        std::string GetAccessoriesAllRecoilFactorModifier = "40 08 20 1E ? ? ? 71";
        std::string AnimationKick = "0B 08 2B 1E 00 01 3F D6";
        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
        printf("CalShootTargetLocationBPOffset:0x%lX\n", findIdaPattern(map_type, CalShootTargetLocationBP, 0) - base);
        printf("DecArrayOffset:0x%lX\n", findIdaPattern(map_type, DecArray, 0) - base);
        printf("GetAccessoriesDeviationFactorOffset:0x%lX\n", findIdaPattern(map_type, GetAccessoriesDeviationFactor, 0) - base);
        printf("GetAccessoriesAllRecoilFactorModifierOffset:0x%lX\n", findIdaPattern(map_type, GetAccessoriesAllRecoilFactorModifier, 0) - base);
        printf("AnimationKickOffset:0x%lX\n", findIdaPattern(map_type, AnimationKick, 0) - base);
    }
    uintptr_t GetNamesPtr() const override
    {
        print_func();
        return IGameProfile::GetNamesPtr();
    }
    uintptr_t GetFrameCount() const override
    {
        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;


        std::string ida_pattern = "? ? ? ? ? ? ? F9 ? ? ? F9 C0 03 5F D6 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 F3 03 01 2A";
        const int step = 0;
        auto FrameOff = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step));
        if (FrameOff != 0)
            return vm_rpm_ptr<uintptr_t>((void*)FrameOff);
        return 0;
    }

    uintptr_t GetMatrix() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"? ? ? B4 ? ? ? 32 68 25 08 9B ? ? ? F9 ? ? ? 14 ? ? ? 97", -0x34},
            {"68 25 08 9B ? ? ? F9 ? ? ? 14 ? ? ? 97", -0x3c},
            {"? ? ? 54 ? ? ? 14 ? ? ? B4 ? ? ? 32 68 25 08 9B ? ? ? F9 ? ? ? 14 ? ? ? 97", -0x2c},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto& it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step));
            if (adrl != 0) return adrl;
        }
        return 0;
    }

    uintptr_t GetPhysx() const override
    {
        struct TArrayView
        {
            uintptr_t data = 0;
            int32_t count = 0;
            int32_t max = 0;
        };

        constexpr uintptr_t kSceneArrayOffset = 0x8;
        constexpr uintptr_t kSceneRigidActorsOffset = 0x2568;
        constexpr uintptr_t kActorTypeOffset = 0x8;
        constexpr int kMinPhysxScore = 18;
        constexpr int kStrongPhysxScore = 80;

        auto isPossibleUserPtr = [](uintptr_t ptr) -> bool {
            return ptr >= 0x10000 && ptr < 0x8000000000ULL && (ptr % sizeof(uintptr_t)) == 0;
        };

        auto isReadablePtr = [&](uintptr_t ptr, size_t len = sizeof(uintptr_t)) -> bool {
            return isPossibleUserPtr(ptr) && kPtrValidator.isPtrReadable(ptr, len);
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

            int32_t checkCount = arr.count < 4 ? arr.count : 4;
            return kPtrValidator.isPtrReadable(arr.data, static_cast<size_t>(checkCount) * sizeof(uintptr_t));
        };

        auto scoreActorArray = [&](const TArrayView &actors) -> int {
            if (!isValidArray(actors, 0x20000))
                return 0;

            int sampleCount = actors.count < 64 ? actors.count : 64;
            std::vector<uintptr_t> actorPtrs(static_cast<size_t>(sampleCount));
            if (!vm_rpm_ptr((void *)actors.data, actorPtrs.data(), actorPtrs.size() * sizeof(uintptr_t)))
                return 0;

            int readableActors = 0;
            int typeHits = 0;
            int dynamicHits = 0;
            int staticHits = 0;

            for (uintptr_t actor : actorPtrs)
            {
                if (!isReadablePtr(actor + kActorTypeOffset, sizeof(uint16_t)))
                    continue;

                readableActors++;
                uint16_t actorType = vm_rpm_ptr<uint16_t>((void *)(actor + kActorTypeOffset));
                if (actorType == 6 || actorType == 7)
                {
                    typeHits++;
                    if (actorType == 6) dynamicHits++;
                    if (actorType == 7) staticHits++;
                }
            }

            if (readableActors < 2 || typeHits < 2)
                return 0;

            int score = 8 + typeHits * 2;
            if (dynamicHits > 0) score += 3;
            if (staticHits > 0) score += 3;
            if (typeHits * 2 >= readableActors) score += 4;
            return score;
        };

        auto scoreScene = [&](uintptr_t scene) -> int {
            if (!isReadablePtr(scene + kSceneRigidActorsOffset, sizeof(TArrayView)))
                return 0;

            TArrayView actors{};
            if (!readArray(scene + kSceneRigidActorsOffset, actors))
                return 0;

            int actorScore = scoreActorArray(actors);
            if (actorScore == 0)
                return 0;

            return 6 + actorScore;
        };

        auto scorePhysxInstance = [&](uintptr_t physx) -> int {
            if (!isReadablePtr(physx + kSceneArrayOffset, sizeof(TArrayView)))
                return 0;

            TArrayView scenes{};
            if (!readArray(physx + kSceneArrayOffset, scenes) || !isValidArray(scenes, 64))
                return 0;

            int checkedScenes = scenes.count < 4 ? scenes.count : 4;
            uintptr_t scenePtrs[4] = {};
            if (!vm_rpm_ptr((void *)scenes.data, scenePtrs, static_cast<size_t>(checkedScenes) * sizeof(uintptr_t)))
                return 0;

            int bestSceneScore = 0;
            int sceneHits = 0;
            for (int i = 0; i < checkedScenes; i++)
            {
                int sceneScore = scoreScene(scenePtrs[i]);
                if (sceneScore > 0)
                {
                    sceneHits++;
                    if (sceneScore > bestSceneScore)
                        bestSceneScore = sceneScore;
                }
            }

            if (sceneHits == 0)
                return 0;

            return 8 + bestSceneScore + sceneHits * 4;
        };

        auto scorePhysxSlot = [&](uintptr_t slot) -> int {
            if (!kPtrValidator.isPtrReadable(slot, sizeof(uintptr_t)))
                return 0;

            uintptr_t physx = vm_rpm_ptr<uintptr_t>((void *)slot);
            if (!isReadablePtr(physx + kSceneArrayOffset, sizeof(TArrayView)))
                return 0;

            return scorePhysxInstance(physx);
        };

        auto reportPhysx = [&](const char *source, uintptr_t slot, int score) -> uintptr_t {
            uintptr_t base = GetUEVars()->GetBaseAddress();
            uintptr_t physx = vm_rpm_ptr<uintptr_t>((void *)slot);
            LOGI("[PUBGMHD] Physx %s found: slot=0x%lx rva=0x%lx instance=0x%lx score=%d",
                 source,
                 static_cast<unsigned long>(slot),
                 static_cast<unsigned long>(slot - base),
                 static_cast<unsigned long>(physx),
                 score);
            return slot;
        };

        auto findPhysxByStructure = [&]() -> uintptr_t {
            ElfScanner ueElf = GetUnrealELF();
            std::vector<KittyMemoryEx::ProcMap> scanSegments;

            auto addSegment = [&](const KittyMemoryEx::ProcMap &seg) {
                if (!seg.readable || !seg.writeable || !seg.is_private)
                    return;

                if (seg.endAddress <= seg.startAddress || seg.length < sizeof(uintptr_t))
                    return;

                bool exists = false;
                for (const auto &known : scanSegments)
                {
                    if (known.startAddress == seg.startAddress && known.endAddress == seg.endAddress)
                    {
                        exists = true;
                        break;
                    }
                }
                if (!exists)
                    scanSegments.push_back(seg);
            };

            for (const auto &seg : ueElf.bssSegments())
            {
                addSegment(seg);
            }

            for (const auto &seg : ueElf.segments())
            {
                addSegment(seg);
            }

            uintptr_t bestSlot = 0;
            int bestScore = 0;
            constexpr size_t kChunkSize = 0x100000;
            std::vector<uint8_t> buffer(kChunkSize);

            LOGI("[PUBGMHD] Physx structure scan start: segments=%zu", scanSegments.size());

            for (const auto &seg : scanSegments)
            {
                for (uintptr_t cursor = seg.startAddress; cursor < seg.endAddress; cursor += kChunkSize)
                {
                    size_t toRead = seg.endAddress - cursor;
                    if (toRead > kChunkSize) toRead = kChunkSize;
                    if (toRead < sizeof(uintptr_t)) continue;
                    if (!vm_rpm_ptr((void *)cursor, buffer.data(), toRead))
                        continue;

                    for (size_t off = 0; off + sizeof(uintptr_t) <= toRead; off += sizeof(uintptr_t))
                    {
                        uintptr_t physx = 0;
                        std::memcpy(&physx, buffer.data() + off, sizeof(uintptr_t));
                        if (!isPossibleUserPtr(physx))
                            continue;

                        uintptr_t slot = cursor + off;
                        int score = scorePhysxSlot(slot);
                        if (score > bestScore)
                        {
                            bestScore = score;
                            bestSlot = slot;
                            LOGI("[PUBGMHD] Physx candidate: slot=0x%lx rva=0x%lx instance=0x%lx score=%d",
                                 static_cast<unsigned long>(slot),
                                 static_cast<unsigned long>(slot - GetUEVars()->GetBaseAddress()),
                                 static_cast<unsigned long>(physx),
                                 score);
                            if (score >= kStrongPhysxScore)
                                return reportPhysx("structure", bestSlot, bestScore);
                        }
                    }
                }
            }

            return bestScore >= kMinPhysxScore ? reportPhysx("structure", bestSlot, bestScore) : 0;
        };

        uintptr_t structPhysx = findPhysxByStructure();
        if (structPhysx) return structPhysx;

        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"? ? ? B0 00 03 3F D6", 0x0},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto& it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step),0x20);
            int score = scorePhysxSlot(adrl);
            if (score >= kMinPhysxScore)
                return reportPhysx("pattern", adrl, score);
        }

        return 0;
    }
    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_18_19(isUsingCasePreservingName());

        static bool once = false;
        if (!once)
        {
            once = true;
            offsets.FNameEntry.Index = sizeof(void *);
            offsets.FNameEntry.Name = sizeof(void *) + sizeof(int32_t);
            offsets.TUObjectArray.NumElements = 0x38;
        }

        return &offsets;
    }
};
