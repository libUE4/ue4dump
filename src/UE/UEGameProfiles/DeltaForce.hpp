#pragma once

#include "../UEGameProfile.hpp"
using namespace UEMemory;

class DeltaForceProfile : public IGameProfile
{
public:
    DeltaForceProfile() = default;

    bool ArchSupprted() const override
    {
        auto e_machine = GetUnrealELF().header().e_machine;
        return e_machine == EM_AARCH64;
    }

    std::string GetAppName() const override
    {
        return "Delta Force(CN)";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.tencent.tmgp.dfm"};
    }

    bool isUsingCasePreservingName() const override
    {
        return false;
    }

    bool IsUsingFNamePool() const override
    {
        return true;
    }

    bool isUsingOutlineNumberName() const override
    {
        return false;
    }
    uintptr_t GetGUObjectArrayPtr() const override
    {
        return IGameProfile::GetGUObjectArrayPtr();
    }
    uintptr_t GetFrameCount() const override
    {
        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        std::string ida_pattern = "? ? ? F0 ? ? ? F9 ? ? ? F9 C0 03 5F D6 ? ? ? A9 FD 03 00 91 ? ? ? D0";
        const int step = 0;
        auto FrameOff = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step));
        if (FrameOff !=0)
            return vm_rpm_ptr<uintptr_t>((void*)FrameOff);
        return  0;
    }
    uintptr_t GetMatrix() const override
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"08 3D 40 F9 00 01 3F D6 E8 03 13 AA ? ? ? F9", 0x18},
            {"FD ? ? A9 28 ? ? F9 F3 ? ? F8 C0 03 5F D6", -0x1c},
            {"00 01 3F D6 E8 03 13 AA 60 ? 00 F9 ? ? ? A9", 14},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto &it : idaPatterns)
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
        struct CollectionDesc
        {
            uintptr_t data = 0;
            int32_t count = 0;
            int32_t max = 0;
        };

        struct PhysxCandidate
        {
            uintptr_t slot = 0;
            uintptr_t holder = 0;
            uintptr_t scene = 0;
            uintptr_t collectionOffset = 0;
            CollectionDesc collection{};
            int readableEntries = 0;
            int typeHits = 0;
            int score = 0;
        };

        constexpr int32_t kMaxCollectionEntries = 50000;
        constexpr size_t kSceneScanSize = 0x4000;
        constexpr size_t kChunkSize = 0x10000;
        constexpr size_t kSmallSegmentLimit = 0x400000;
        constexpr int kMinPhysxScore = 48;
        constexpr int kStrongPhysxScore = 120;

        auto isPossiblePointer = [](uintptr_t value) -> bool
        {
            return value >= 0x10000 && value < 0x8000000000ULL &&
                   (value % sizeof(uintptr_t)) == 0;
        };

        auto readPointer = [&](uintptr_t address, uintptr_t &value) -> bool
        {
            value = 0;
            if (!address || !kPtrValidator.isPtrReadable(address, sizeof(value)) ||
                !vm_rpm_ptr(reinterpret_cast<void *>(address), &value, sizeof(value)))
            {
                return false;
            }
            return isPossiblePointer(value) && kPtrValidator.isPtrReadable(value);
        };

        auto scoreScene = [&](uintptr_t scene, PhysxCandidate &result) -> int
        {
            if (!isPossiblePointer(scene) ||
                !kPtrValidator.isPtrReadable(scene, kSceneScanSize))
            {
                return 0;
            }

            std::vector<uint8_t> sceneSnapshot(kSceneScanSize);
            if (!vm_rpm_ptr(reinterpret_cast<void *>(scene),
                            sceneSnapshot.data(), sceneSnapshot.size()))
            {
                return 0;
            }

            PhysxCandidate best{};
            best.scene = scene;

            for (size_t offset = 0;
                 offset + sizeof(CollectionDesc) <= sceneSnapshot.size();
                 offset += sizeof(uintptr_t))
            {
                CollectionDesc collection{};
                __builtin_memcpy(&collection, sceneSnapshot.data() + offset, sizeof(collection));
                if (!isPossiblePointer(collection.data) ||
                    collection.count < 4 || collection.max < collection.count ||
                    collection.max > kMaxCollectionEntries)
                {
                    continue;
                }

                const int sampleCount = collection.count < 32 ? collection.count : 32;
                const size_t sampleBytes = static_cast<size_t>(sampleCount) * sizeof(uintptr_t);
                if (!kPtrValidator.isPtrReadable(collection.data, sampleBytes))
                    continue;

                std::vector<uintptr_t> entries(static_cast<size_t>(sampleCount));
                if (!vm_rpm_ptr(reinterpret_cast<void *>(collection.data),
                                entries.data(), sampleBytes))
                {
                    continue;
                }

                int readableEntries = 0;
                int dynamicHits = 0;
                int staticHits = 0;
                for (uintptr_t entry : entries)
                {
                    if (!isPossiblePointer(entry) ||
                        !kPtrValidator.isPtrReadable(entry + sizeof(uintptr_t), sizeof(uint16_t)))
                    {
                        continue;
                    }

                    ++readableEntries;
                    const uint16_t type = vm_rpm_ptr<uint16_t>(
                        reinterpret_cast<void *>(entry + sizeof(uintptr_t)));
                    if (type == 6) ++dynamicHits;
                    if (type == 7) ++staticHits;
                }

                const int typeHits = dynamicHits + staticHits;
                if (readableEntries < 4 || typeHits < 4 ||
                    typeHits * 2 < readableEntries)
                {
                    continue;
                }

                int score = typeHits * 4;
                if (dynamicHits > 0) score += 10;
                if (staticHits > 0) score += 10;
                if (typeHits == readableEntries) score += 20;
                if (collection.count > 100) score += 8;
                if (collection.count > 1000) score += 8;

                if (score > best.score)
                {
                    best.scene = scene;
                    best.collectionOffset = offset;
                    best.collection = collection;
                    best.readableEntries = readableEntries;
                    best.typeHits = typeHits;
                    best.score = score;
                }
            }

            if (best.score > result.score)
                result = best;
            return best.score;
        };

        auto scoreSlot = [&](uintptr_t slot, uintptr_t holder,
                             PhysxCandidate &result) -> int
        {
            uintptr_t p1 = 0;
            uintptr_t p2 = 0;
            uintptr_t scene = 0;
            if (!readPointer(holder, p1) ||
                !readPointer(p1 + sizeof(uintptr_t), p2) ||
                !readPointer(p2, scene))
            {
                return 0;
            }

            PhysxCandidate candidate{};
            const int score = scoreScene(scene, candidate);
            if (score > 0)
            {
                candidate.slot = slot;
                candidate.holder = holder;
                result = candidate;
            }
            return score;
        };

        const ElfScanner ueElf = GetUnrealELF();
        const uintptr_t base = ueElf.base();
        std::vector<KittyMemoryEx::ProcMap> scanSegments;

        auto addSegment = [&](const KittyMemoryEx::ProcMap &segment)
        {
            if (!segment.isValid() || !segment.writeable || !segment.is_private ||
                segment.endAddress <= segment.startAddress ||
                segment.length < sizeof(uintptr_t))
            {
                return;
            }

            for (const auto &known : scanSegments)
            {
                if (known.startAddress == segment.startAddress &&
                    known.endAddress == segment.endAddress)
                {
                    return;
                }
            }
            scanSegments.push_back(segment);
        };

        for (const auto &segment : ueElf.bssSegments())
            addSegment(segment);
        for (const auto &segment : ueElf.segments())
            addSegment(segment);

        auto isUEHolder = [&](uintptr_t address) -> bool
        {
            for (const auto &segment : scanSegments)
            {
                if (address >= segment.startAddress &&
                    address + sizeof(uintptr_t) <= segment.endAddress)
                {
                    return true;
                }
            }
            return false;
        };

        auto reportCandidate = [&](const char *source,
                                   const PhysxCandidate &candidate) -> uintptr_t
        {
            LOGI("[DeltaForce] Physx %s found: slot=0x%lx rva=0x%lx holder=0x%lx scene=0x%lx collection=+0x%lx entries=%d/%d types=%d/%d score=%d",
                 source,
                 static_cast<unsigned long>(candidate.slot),
                 static_cast<unsigned long>(base ? candidate.slot - base : 0),
                 static_cast<unsigned long>(candidate.holder),
                 static_cast<unsigned long>(candidate.scene),
                 static_cast<unsigned long>(candidate.collectionOffset),
                 candidate.collection.count,
                 candidate.collection.max,
                 candidate.typeHits,
                 candidate.readableEntries,
                 candidate.score);
            return candidate.slot;
        };

        PhysxCandidate bestCandidate{};
        std::vector<uint8_t> buffer(kChunkSize);
        LOGI("[DeltaForce] Physx structure scan start: segments=%zu",
             scanSegments.size());

        auto scanPass = [&](bool smallSegments) -> uintptr_t
        {
            for (const auto &segment : scanSegments)
            {
                const bool isSmall = segment.length <= kSmallSegmentLimit;
                if (isSmall != smallSegments)
                    continue;

                for (uintptr_t cursor = segment.startAddress;
                     cursor < segment.endAddress;
                     cursor += kChunkSize)
                {
                    size_t toRead = static_cast<size_t>(segment.endAddress - cursor);
                    if (toRead > buffer.size()) toRead = buffer.size();
                    if (toRead < sizeof(uintptr_t) ||
                        !vm_rpm_ptr(reinterpret_cast<void *>(cursor), buffer.data(), toRead))
                    {
                        continue;
                    }

                    for (size_t offset = 0;
                         offset + sizeof(uintptr_t) <= toRead;
                         offset += sizeof(uintptr_t))
                    {
                        uintptr_t holder = 0;
                        __builtin_memcpy(&holder, buffer.data() + offset, sizeof(holder));
                        if (!isPossiblePointer(holder) || !isUEHolder(holder))
                            continue;

                        PhysxCandidate candidate{};
                        const uintptr_t slot = cursor + offset;
                        const int score = scoreSlot(slot, holder, candidate);
                        if (score <= bestCandidate.score)
                            continue;

                        bestCandidate = candidate;
                        LOGI("[DeltaForce] Physx candidate: slot=0x%lx rva=0x%lx scene=0x%lx collection=+0x%lx score=%d",
                             static_cast<unsigned long>(slot),
                             static_cast<unsigned long>(base ? slot - base : 0),
                             static_cast<unsigned long>(candidate.scene),
                             static_cast<unsigned long>(candidate.collectionOffset),
                             score);

                        if (score >= kStrongPhysxScore)
                            return reportCandidate("structure", candidate);
                    }
                }
            }
            return 0;
        };

        auto holderInModuleDataWindow = [&](uintptr_t address) -> bool
        {
            if (!base || address < base) return false;
            const uintptr_t rva = address - base;
            return rva >= 0x10000000ULL && rva <= 0x24000000ULL;
        };

        auto scanDataWindow = [&](uintptr_t beginRva, uintptr_t endRva,
                                  const char *name) -> uintptr_t
        {
            if (!base || endRva <= beginRva) return 0;
            const uintptr_t begin = base + beginRva;
            const uintptr_t end = base + endRva;
            LOGI("[DeltaForce] Physx data-window scan: %s rva=0x%lx..0x%lx",
                 name,
                 static_cast<unsigned long>(beginRva),
                 static_cast<unsigned long>(endRva));
            for (uintptr_t cursor = begin; cursor < end; cursor += kChunkSize)
            {
                size_t toRead = static_cast<size_t>(end - cursor);
                if (toRead > buffer.size()) toRead = buffer.size();
                if (toRead < sizeof(uintptr_t) ||
                    !vm_rpm_ptr(reinterpret_cast<void *>(cursor), buffer.data(), toRead))
                {
                    continue;
                }

                for (size_t offset = 0;
                     offset + sizeof(uintptr_t) <= toRead;
                     offset += sizeof(uintptr_t))
                {
                    uintptr_t holder = 0;
                    __builtin_memcpy(&holder, buffer.data() + offset, sizeof(holder));
                    if (!isPossiblePointer(holder) || !holderInModuleDataWindow(holder))
                        continue;

                    PhysxCandidate candidate{};
                    const uintptr_t slot = cursor + offset;
                    const int score = scoreSlot(slot, holder, candidate);
                    if (score <= bestCandidate.score)
                        continue;

                    bestCandidate = candidate;
                    LOGI("[DeltaForce] Physx data-window candidate: slot=0x%lx rva=0x%lx scene=0x%lx collection=+0x%lx score=%d",
                         static_cast<unsigned long>(slot),
                         static_cast<unsigned long>(base ? slot - base : 0),
                         static_cast<unsigned long>(candidate.scene),
                         static_cast<unsigned long>(candidate.collectionOffset),
                         score);
                    if (score >= kStrongPhysxScore)
                        return reportCandidate("data-window", candidate);
                }
            }
            return 0;
        };

        if (uintptr_t found = scanPass(true))
            return found;
        if (uintptr_t found = scanPass(false))
            return found;
        if (uintptr_t found = scanDataWindow(0x18000000ULL, 0x20000000ULL, "primary"))
            return found;
        if (uintptr_t found = scanDataWindow(0x10000000ULL, 0x24000000ULL, "wide"))
            return found;
        if (bestCandidate.score >= kMinPhysxScore)
            return reportCandidate("structure-best", bestCandidate);

        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"E1 ? ? ? 40 00 40 BD F4 03 02 AA",0x28},
            {"48 ? ? ? F3 03 04 AA F5 03 03 2A", -0xc},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto &it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::Decode_ADRP_LDR(findIdaPattern(map_type, ida_pattern, step),8);
            uintptr_t holder = 0;
            PhysxCandidate candidate{};
            if (adrl != 0 && readPointer(adrl, holder) &&
                scoreSlot(adrl, holder, candidate) >= kMinPhysxScore)
            {
                return reportCandidate("pattern", candidate);
            }
        }
        return 0;
    }
    uintptr_t GetNamesPtr() const override
    {
        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        std::string ida_pattern = "91 ? 10 81 52 ? ? 21 8b";
        const int step = -7;

        return Arm64::Decode_ADRP_ADD(findIdaPattern(map_type, ida_pattern, step));
    }

    uintptr_t GetProcessEvent() const override
    {
        if (uintptr_t processEvent = IGameProfile::GetProcessEvent())
            return processEvent;

        auto base = GetUEVars()->GetBaseAddress();
        constexpr uintptr_t kKnownProcessEvent = 0xE8B79F0;
        return base ? (base + kKnownProcessEvent) : 0;
    }

    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_25_27(isUsingCasePreservingName());

        static bool once = false;
        if (!once)
        {
            once = true;

            offsets.FNamePool.BlocksBit = 18;
            offsets.FNamePool.BlocksOff -= sizeof(void *);

            offsets.TUObjectArray.NumElements = sizeof(int32_t);
            offsets.TUObjectArray.Objects = offsets.TUObjectArray.NumElements + (sizeof(int32_t) * 3);

            offsets.UObject.ClassPrivate = sizeof(void *);
            offsets.UObject.OuterPrivate = offsets.UObject.ClassPrivate + sizeof(void *);
            offsets.UObject.ObjectFlags = offsets.UObject.OuterPrivate + sizeof(void *);
            offsets.UObject.NamePrivate = offsets.UObject.ObjectFlags + sizeof(int32_t);
            offsets.UObject.InternalIndex = offsets.UObject.NamePrivate + offsets.FName.Size;

            offsets.UStruct.PropertiesSize = offsets.UField.Next + (sizeof(void *) * 2) + sizeof(int32_t);
            offsets.UStruct.SuperStruct = offsets.UStruct.PropertiesSize + sizeof(int32_t);
            offsets.UStruct.Children = offsets.UStruct.SuperStruct + (sizeof(void *) * 2);
            offsets.UStruct.ChildProperties = offsets.UStruct.Children + (sizeof(void *) * 3);

            offsets.UFunction.NumParams = offsets.UStruct.ChildProperties + ((sizeof(void *) + sizeof(int32_t) * 2) * 2) + (sizeof(void *) * 5);
            offsets.UFunction.ParamSize = offsets.UFunction.NumParams + sizeof(int16_t);
            offsets.UFunction.EFunctionFlags = offsets.UFunction.ParamSize + sizeof(int16_t) + sizeof(int32_t);
            offsets.UFunction.Func = offsets.UFunction.EFunctionFlags + (sizeof(int32_t) * 2) + (sizeof(void *) * 3);

            offsets.FField.FlagsPrivate = sizeof(void *);
            offsets.FField.Next = offsets.FField.FlagsPrivate + (sizeof(void *) * 2);
            offsets.FField.ClassPrivate = offsets.FField.Next + sizeof(void *);
            offsets.FField.NamePrivate = offsets.FField.ClassPrivate + sizeof(void *);

            offsets.FProperty.ArrayDim = offsets.FField.NamePrivate + GetPtrAlignedOf(offsets.FName.Size) + sizeof(void *);
            offsets.FProperty.ElementSize = offsets.FProperty.ArrayDim + sizeof(int32_t);
            offsets.FProperty.PropertyFlags = offsets.FProperty.ElementSize + sizeof(int32_t);
            offsets.FProperty.Offset_Internal = offsets.FProperty.PropertyFlags + sizeof(int64_t) + sizeof(int32_t);
            offsets.FProperty.Size = offsets.FProperty.Offset_Internal + (sizeof(int32_t) * 3) + (sizeof(void *) * 4);
        }

        return &offsets;
    }


    std::string GetNameEntryString(uint8_t *entry) const override
    {
        std::string name = IGameProfile::GetNameEntryString(entry);

        auto dec_ansi = [](char *str, uint32_t len)
        {
            if (!str || !*str || len == 0) return;

            uint32_t key = 0;
            switch (len % 9)
            {
            case 0u:
                key = ((len & 0x1F) + len);
                break;
            case 1u:
                key = ((len ^ 0xDF) + len);
                break;
            case 2u:
                key = ((len | 0xCF) + len);
                break;
            case 3u:
                key = (33 * len);
                break;
            case 4u:
                key = (len + (len >> 2));
                break;
            case 5u:
                key = (3 * len + 5);
                break;
            case 6u:
                key = (((4 * len) | 5) + len);
                break;
            case 7u:
                key = (((len >> 4) | 7) + len);
                break;
            case 8u:
                key = ((len ^ 0xC) + len);
                break;
            default:
                key = ((len ^ 0x40) + len);
                break;
            }

            for (uint32_t i = 0; i < len; i++)
            {
                str[i] = (key & 0x80) ^ ~str[i];
            }
        };

        dec_ansi(name.data(), uint32_t(name.length()));

        return name;
    }
};
