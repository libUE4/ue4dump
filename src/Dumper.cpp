#include "Dumper.hpp"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "UE/UEMemory.hpp"
using namespace UEMemory;

#include "UPackageGenerator.hpp"
#include "AutoFix/AutoFixOffsets.hpp"

namespace dumper_jf_ns
{
    static uintptr_t base_address = 0;
    struct JsonFunction
    {
        std::string Parent;
        std::string Name;
        uint64_t Address = 0;
    };
    static std::vector<JsonFunction> jsonFunctions;

    void to_json(json &j, const JsonFunction &jf)
    {
        if (jf.Parent.empty() || jf.Parent == "None" || jf.Parent == "null")
            return;
        if (jf.Name.empty() || jf.Name == "None" || jf.Name == "null")
            return;
        if (jf.Address == 0 || jf.Address <= base_address)
            return;

        std::string fname = IOUtils::replace_specials(jf.Parent, '_');
        fname += "$$";
        fname += IOUtils::replace_specials(jf.Name, '_');

        j = json{{"Name", fname}, {"Address", (jf.Address - base_address)}};
    }
}  // namespace dumper_jf_ns

namespace
{
    bool IsStpFpLr(uint32_t insn)
    {
        return (((insn & 0xFFC00000) == 0xA9000000) ||
                ((insn & 0xFFC00000) == 0xA9800000)) &&
               ((insn >> 5) & 0x1F) == 31;
    }

    bool IsStrInstruction(uint32_t insn)
    {
        return ((insn & 0xFFC00000) == 0xF8000000) ||
               ((insn & 0xFFC00000) == 0xFC000000) ||
               ((insn & 0xFFC00000) == 0xF9000000);
    }

    uintptr_t FindProcessEventIdx(uintptr_t processEvent)
    {
        auto *objects = UEWrappers::GetObjects();
        if (!objects) return 0;

        UE_UObject firstObject = objects->GetObjectPtr(0);
        if (!firstObject) return 0;

        uintptr_t vtable = vm_rpm_ptr<uintptr_t>((void *)firstObject.GetAddress());
        if (!kPtrValidator.isPtrReadable((void *)vtable))
            return 0;

        uintptr_t fallbackIdx = 0;
        for (uintptr_t i = 0; i < 500; ++i)
        {
            uintptr_t function = vm_rpm_ptr<uintptr_t>((void *)(vtable + i * sizeof(uintptr_t)));
            if (!kPtrValidator.isPtrReadable((void *)(function + 0x10)))
                continue;

            uint32_t insn0 = vm_rpm_ptr<uint32_t>((void *)(function));
            uint32_t insn1 = vm_rpm_ptr<uint32_t>((void *)(function + 0x4));
            uint32_t insn2 = vm_rpm_ptr<uint32_t>((void *)(function + 0x8));
            if (!(i > 50 && i < 100 &&
                  (IsStpFpLr(insn0) || IsStrInstruction(insn0)) &&
                  (IsStpFpLr(insn1) || IsStrInstruction(insn1)) &&
                  (IsStpFpLr(insn2) || IsStrInstruction(insn2))))
            {
                continue;
            }

            if (processEvent && function == processEvent)
                return i;

            if (fallbackIdx == 0)
                fallbackIdx = i;
        }

        return fallbackIdx;
    }

    struct ObjectReferenceCandidate
    {
        uintptr_t slot = 0;
        uintptr_t object = 0;
        int score = 0;
        std::string name;
        std::string reason;
    };

    struct WorldLayoutProbe
    {
        const char *name = "";
        uintptr_t persistentLevel = 0;
        uintptr_t owningGameInstance = 0;
        uintptr_t levelActors = 0;
        uintptr_t gameInstanceLocalPlayers = 0;
        uintptr_t localPlayerController = 0;
        uintptr_t playerControllerPawn0 = 0;
        uintptr_t playerControllerPawn1 = 0;
        uintptr_t playerControllerCameraManager = 0;
    };

    bool ProfileHasAppId(const IGameProfile *profile, const char *appId)
    {
        if (!profile || !appId || !*appId)
            return false;

        for (const auto &id : profile->GetAppIDs())
        {
            if (id == appId)
                return true;
        }
        return false;
    }

    bool IsReadablePtrValue(uintptr_t value)
    {
        return value >= 0x10000 && kPtrValidator.isPtrReadable((void *)value);
    }

    template <typename T>
    bool ReadValue(uintptr_t address, T &out)
    {
        if (!address)
            return false;
        return vm_rpm_ptr((void *)address, &out, sizeof(T));
    }

    bool ReadReadablePtr(uintptr_t address, uintptr_t &out)
    {
        out = 0;
        if (!ReadValue(address, out))
            return false;
        return IsReadablePtrValue(out);
    }

    void AppendReason(std::string &reason, const char *text)
    {
        if (!reason.empty())
            reason += ", ";
        reason += text;
    }

    void FindAlignedPointerReferences(uintptr_t start, size_t length, uintptr_t target,
                                      std::vector<uintptr_t> &out, size_t maxRefs)
    {
        if (!start || !length || !target || out.size() >= maxRefs)
            return;

        const uintptr_t alignedStart = GetPtrAlignedOf(start);
        const uintptr_t rawEnd = start + length;
        const uintptr_t alignedEnd = rawEnd & ~(uintptr_t(sizeof(uintptr_t)) - 1);
        if (alignedEnd <= alignedStart)
            return;

        constexpr size_t kChunkSize = 0x100000;
        std::vector<uint8_t> buffer;
        for (uintptr_t cursor = alignedStart;
             cursor + sizeof(uintptr_t) <= alignedEnd && out.size() < maxRefs;)
        {
            size_t toRead = std::min<size_t>(kChunkSize, alignedEnd - cursor);
            toRead &= ~(sizeof(uintptr_t) - 1);
            if (toRead < sizeof(uintptr_t))
                break;

            buffer.resize(toRead);
            if (vm_rpm_ptr((void *)cursor, buffer.data(), toRead))
            {
                for (size_t i = 0; i + sizeof(uintptr_t) <= toRead && out.size() < maxRefs;
                     i += sizeof(uintptr_t))
                {
                    uintptr_t value = 0;
                    std::memcpy(&value, buffer.data() + i, sizeof(value));
                    if (value == target)
                        out.push_back(cursor + i);
                }
            }

            cursor += toRead;
        }
    }

    int ScoreWorldLayout(uintptr_t world, const WorldLayoutProbe &layout, std::string &reason)
    {
        int score = 0;

        uintptr_t level = 0;
        if (ReadReadablePtr(world + layout.persistentLevel, level))
        {
            score += 30;
            AppendReason(reason, "PersistentLevel");

            uintptr_t actors = 0;
            int32_t actorCount = 0;
            const bool hasActors = ReadReadablePtr(level + layout.levelActors, actors);
            const bool hasActorCount = ReadValue(level + layout.levelActors + sizeof(uintptr_t), actorCount);
            if (hasActors && hasActorCount && actorCount > 0 && actorCount < 1000000)
            {
                score += 45;
                AppendReason(reason, "Actors");
            }
        }

        uintptr_t gameInstance = 0;
        if (ReadReadablePtr(world + layout.owningGameInstance, gameInstance))
        {
            score += 35;
            AppendReason(reason, "OwningGameInstance");

            uintptr_t localPlayers = 0;
            int32_t localPlayersNum = 0;
            int32_t localPlayersMax = 0;
            const bool hasLocalPlayers = ReadReadablePtr(gameInstance + layout.gameInstanceLocalPlayers,
                                                         localPlayers);
            const bool hasLocalPlayersNum = ReadValue(gameInstance + layout.gameInstanceLocalPlayers + sizeof(uintptr_t),
                                                      localPlayersNum);
            const bool hasLocalPlayersMax = ReadValue(gameInstance + layout.gameInstanceLocalPlayers + sizeof(uintptr_t) + sizeof(int32_t),
                                                      localPlayersMax);
            if (hasLocalPlayers && hasLocalPlayersNum && hasLocalPlayersMax &&
                localPlayersNum > 0 && localPlayersNum <= 16 &&
                localPlayersMax >= localPlayersNum && localPlayersMax <= 64)
            {
                score += 80;
                AppendReason(reason, "LocalPlayers");

                uintptr_t localPlayer = 0;
                if (ReadReadablePtr(localPlayers, localPlayer))
                {
                    score += 20;
                    AppendReason(reason, "LocalPlayer[0]");

                    uintptr_t controller = 0;
                    if (ReadReadablePtr(localPlayer + layout.localPlayerController, controller))
                    {
                        score += 40;
                        AppendReason(reason, "PlayerController");

                        uintptr_t pawn = 0;
                        if ((layout.playerControllerPawn0 &&
                             ReadReadablePtr(controller + layout.playerControllerPawn0, pawn)) ||
                            (layout.playerControllerPawn1 &&
                             ReadReadablePtr(controller + layout.playerControllerPawn1, pawn)))
                        {
                            score += 20;
                            AppendReason(reason, "Pawn");
                        }

                        uintptr_t camera = 0;
                        if (layout.playerControllerCameraManager &&
                            ReadReadablePtr(controller + layout.playerControllerCameraManager, camera))
                        {
                            score += 20;
                            AppendReason(reason, "CameraManager");
                        }
                    }
                }
            }
        }

        if (score > 0 && layout.name && *layout.name)
        {
            reason = std::string(layout.name) + ": " + reason;
        }

        return score;
    }

    void ScoreWorldCandidate(const IGameProfile *profile, ObjectReferenceCandidate &candidate)
    {
        candidate.score = 10;
        candidate.reason = "referenced UObject";

        std::vector<WorldLayoutProbe> layouts;
        if (ProfileHasAppId(profile, "com.tencent.tmgp.dfm"))
        {
            layouts.push_back({
                "DFM",
                0xF8,   // UWorld::PersistentLevel
                0x190,  // UWorld::OwningGameInstance
                0x98,   // ULevel::Actors
                0x38,   // UGameInstance::LocalPlayers
                0x30,   // UPlayer::PlayerController
                0x3F0,  // APlayerController::AcknowledgedPawn
                0x3A0,  // DFM fallback pawn used by existing draw project
                0x408,  // APlayerController::PlayerCameraManager
            });
        }

        for (const auto &layout : layouts)
        {
            std::string reason;
            const int score = ScoreWorldLayout(candidate.object, layout, reason);
            if (score > candidate.score)
            {
                candidate.score = score;
                candidate.reason = reason;
            }
        }
    }

    void AddObjectReferenceCandidates(const std::vector<KittyMemoryEx::ProcMap> &segments,
                                      const UE_UObject &object,
                                      std::vector<ObjectReferenceCandidate> &candidates,
                                      size_t maxRefs)
    {
        std::vector<uintptr_t> refs;
        for (auto it = segments.rbegin(); it != segments.rend() && refs.size() < maxRefs; ++it)
        {
            if (!it->writeable)
                continue;
            FindAlignedPointerReferences(it->startAddress, it->length,
                                         uintptr_t(object.GetAddress()), refs, maxRefs);
        }

        for (uintptr_t ref : refs)
        {
            const auto exists = std::find_if(candidates.begin(), candidates.end(),
                                             [ref](const ObjectReferenceCandidate &candidate)
                                             {
                                                 return candidate.slot == ref;
                                             });
            if (exists != candidates.end())
                continue;

            ObjectReferenceCandidate candidate{};
            candidate.slot = ref;
            candidate.object = uintptr_t(object.GetAddress());
            candidate.name = object.GetFullName();
            candidates.push_back(std::move(candidate));
        }
    }

    void AddUniqueSegment(std::vector<KittyMemoryEx::ProcMap> &segments,
                          const KittyMemoryEx::ProcMap &seg)
    {
        if (!seg.isValid() || !seg.writeable)
            return;

        for (const auto &existing : segments)
        {
            if (existing.startAddress == seg.startAddress && existing.endAddress == seg.endAddress)
                return;
        }
        segments.push_back(seg);
    }

    std::vector<KittyMemoryEx::ProcMap> BuildWritableUESearchSegments(const ElfScanner &ueElf,
                                                                      pid_t pid)
    {
        std::vector<KittyMemoryEx::ProcMap> segments;
        if (!ueElf.isValid())
            return segments;

        uintptr_t moduleEnd = ueElf.base() + ueElf.loadSize();

        // 先放 bss/写段。Android linker 会把部分 UE .bss 标成 [anon:.bss] / -w-p，
        // GEngine/GWorld 这类全局指针常在这里，旧逻辑只扫 ELF segments 会漏。
        for (const auto &seg : ueElf.bssSegments())
        {
            AddUniqueSegment(segments, seg);
            if (seg.endAddress > moduleEnd)
                moduleEnd = static_cast<uintptr_t>(seg.endAddress);
        }

        for (const auto &seg : ueElf.segments())
        {
            AddUniqueSegment(segments, seg);
            if (seg.endAddress > moduleEnd)
                moduleEnd = static_cast<uintptr_t>(seg.endAddress);
        }

        for (const auto &map : KittyMemoryEx::getAllMaps(pid))
        {
            if (!map.writeable)
                continue;

            const bool isUELibMap =
                map.pathname.find("libUE4.so") != std::string::npos ||
                map.pathname.find("libUnreal.so") != std::string::npos;
            const bool isAnonBss = map.pathname.find("[anon:.bss]") != std::string::npos;
            const bool nearUnreal =
                ueElf.base() && map.startAddress >= ueElf.base() &&
                map.startAddress < (moduleEnd + 0x08000000ULL);

            if ((isUELibMap && map.writeable) || (isAnonBss && nearUnreal))
                AddUniqueSegment(segments, map);
        }

        return segments;
    }
}

bool UEDumper::Init(IGameProfile *profile, bool reuseInitializedState)
{
    _lastError.clear();
    if (!profile)
    {
        _lastError = "ERROR_INVALID_PROFILE";
        return false;
    }

    if (reuseInitializedState)
    {
        const UEVars *vars = profile->GetUEVars();
        if (!vars || !vars->GetBaseAddress() || !vars->GetNamesPtr() ||
            !vars->GetGUObjectsArrayPtr() || !vars->GetObjObjectsPtr() ||
            !vars->GetObjObjects_Objects() || !vars->GetOffsets())
        {
            _lastError = "ERROR_REUSE_UNINITIALIZED_PROFILE";
            return false;
        }

        _profile = profile;
        UEWrappers::Init(vars);
        return true;
    }

    UEVarsInitStatus initStatus = profile->InitUEVars();
    if (initStatus != UEVarsInitStatus::SUCCESS)
    {
        _lastError = UEVars::InitStatusToStr(initStatus);
        return false;
    }
    _profile = profile;

    // Run automatic offset fixup for all profiles (including AutoFix).
    // This validates and patches version-derived offsets against the actual
    // running game's memory layout. Failures are non-fatal (logged only).
    AutoFix::RunFixup(profile);
    return true;
}

bool UEDumper::Dump(std::unordered_map<std::string, BufferFmt> *outBuffersMap)
{
    dumper_jf_ns::jsonFunctions.clear();
    outBuffersMap->insert({"Logs.txt", BufferFmt()});
    BufferFmt &logsBufferFmt = outBuffersMap->at("Logs.txt");

    {
        if (_dumpExeInfoNotify) _dumpExeInfoNotify(false);
        DumpExecutableInfo(logsBufferFmt);
        if (_dumpExeInfoNotify) _dumpExeInfoNotify(true);
    }

    {
        if (_dumpNamesInfoNotify) _dumpNamesInfoNotify(false);
        DumpNamesInfo(logsBufferFmt);
        if (_dumpNamesInfoNotify) _dumpNamesInfoNotify(true);
    }

    {
        if (_dumpObjectsInfoNotify) _dumpObjectsInfoNotify(false);
        DumpObjectsInfo(logsBufferFmt);
        if (_dumpObjectsInfoNotify) _dumpObjectsInfoNotify(true);
    }

    {
        if (_dumpOffsetsInfoNotify) _dumpOffsetsInfoNotify(false);
        outBuffersMap->insert({"Offsets.hpp", BufferFmt()});
        BufferFmt &offsetsBufferFmt = outBuffersMap->at("Offsets.hpp");
        DumpOffsetsInfo(logsBufferFmt, offsetsBufferFmt);
        if (_dumpOffsetsInfoNotify) _dumpOffsetsInfoNotify(true);
    }

    outBuffersMap->insert({"Objects.txt", BufferFmt()});
    BufferFmt &objsBufferFmt = outBuffersMap->at("Objects.txt");
    std::vector<std::pair<uint8_t *const, std::vector<UE_UObject>>> packages;
    GatherUObjects(logsBufferFmt, objsBufferFmt, packages, _objectsProgressCallback);

    if (packages.empty())
    {
        logsBufferFmt.append("Error: Packages are empty.\n");
        logsBufferFmt.append("==========================\n");
        _lastError = "ERROR_EMPTY_PACKAGES";
        return false;
    }

    outBuffersMap->insert({"AIOHeader.hpp", BufferFmt()});
    outBuffersMap->insert({"SDK_Enums.hpp", BufferFmt()});
    outBuffersMap->insert({"SDK_Structs.hpp", BufferFmt()});
    outBuffersMap->insert({"SDK_Classes.hpp", BufferFmt()});
    outBuffersMap->insert({"SDK_Offset.hpp", BufferFmt()});
    BufferFmt &aioBufferFmt = outBuffersMap->at("AIOHeader.hpp");
    BufferFmt &enumsBufferFmt = outBuffersMap->at("SDK_Enums.hpp");
    BufferFmt &structsBufferFmt = outBuffersMap->at("SDK_Structs.hpp");
    BufferFmt &classesBufferFmt = outBuffersMap->at("SDK_Classes.hpp");
    BufferFmt &offsetBufferFmt = outBuffersMap->at("SDK_Offset.hpp");
    DumpAIOHeader(logsBufferFmt, aioBufferFmt, enumsBufferFmt, structsBufferFmt, classesBufferFmt, offsetBufferFmt, packages, _dumpProgressCallback);

    dumper_jf_ns::base_address = _profile->GetUnrealELF().base();
    logsBufferFmt.append("正在生成 script.json...\n函数数量: {}\n", dumper_jf_ns::jsonFunctions.size());
    logsBufferFmt.append("==========================\n");

    outBuffersMap->insert({"script.json", BufferFmt()});
    BufferFmt &scriptBufferFmt = outBuffersMap->at("script.json");

    json js;
    js["__author"] = "曦曦(DreamFekk)";
    js["__author_url"] = "https://github.com/DreamFekk";
    js["__notice"] = "禁止圈钱盗卖";
    js["Functions"] = json::array();
    for (const auto &jf : dumper_jf_ns::jsonFunctions)
    {
        js["Functions"].push_back(jf);
    }

    scriptBufferFmt.append("{}", js.dump(4));

    return true;
}

void UEDumper::DumpExecutableInfo(BufferFmt &logsBufferFmt)
{
    auto ue_elf = _profile->GetUnrealELF();
    logsBufferFmt.append("e_machine: 0x{:X}\n", ue_elf.header().e_machine);
    logsBufferFmt.append("Library: {}\n", ue_elf.filePath().c_str());
    logsBufferFmt.append("BaseAddress: 0x{:X}\n", ue_elf.base());

    for (const auto &it : ue_elf.segments())
        logsBufferFmt.append("{}\n", it.toString());

    logsBufferFmt.append("==========================\n");
}

void UEDumper::DumpNamesInfo(BufferFmt &logsBufferFmt)
{
    uintptr_t baseAddr = _profile->GetUEVars()->GetBaseAddress();
    uintptr_t namesPtr = _profile->GetUEVars()->GetNamesPtr();

    if (!_profile->IsUsingFNamePool())
    {
        logsBufferFmt.append("GNames: [<Base> + 0x{:X}] = 0x{:X}\n",
                             namesPtr - baseAddr, namesPtr);
    }
    else
    {
        logsBufferFmt.append("FNamePool: [<Base> + 0x{:X}] = 0x{:X}\n",
                             namesPtr - baseAddr, namesPtr);
    }

    logsBufferFmt.append("Test dumping first 5 name entries\n");
    for (int i = 0; i < 5; i++)
    {
        logsBufferFmt.append("GetNameByID({}): {}\n", i, _profile->GetUEVars()->GetNameByID(i));
    }

    logsBufferFmt.append("==========================\n");
}

void UEDumper::DumpObjectsInfo(BufferFmt &logsBufferFmt)
{
    uintptr_t baseAddr = _profile->GetUEVars()->GetBaseAddress();
    uintptr_t objectArrayPtr = _profile->GetUEVars()->GetGUObjectsArrayPtr();
    uintptr_t objObjectsPtr = _profile->GetUEVars()->GetObjObjectsPtr();

    logsBufferFmt.append("GUObjectArray: [<Base> + 0x{:X}] = 0x{:X}\n", objectArrayPtr - baseAddr, objectArrayPtr);
    logsBufferFmt.append("ObjObjects: [<Base> + 0x{:X}] = 0x{:X}\n", objObjectsPtr - baseAddr, objObjectsPtr);
    logsBufferFmt.append("ObjObjects Num: {}\n", UEWrappers::GetObjects()->GetNumElements());

    logsBufferFmt.append("Test Dumping First 5 Name Entries\n");
    for (int i = 0; i < 5; i++)
    {
        UE_UObject obj = UEWrappers::GetObjects()->GetObjectPtr(i);
        logsBufferFmt.append("GetObjectPtr({}): {}\n", i, obj.GetName());
    }

    logsBufferFmt.append("==========================\n");
}

void UEDumper::DumpOffsetsInfo(BufferFmt &logsBufferFmt, BufferFmt &offsetsBufferFmt)
{
    uintptr_t baseAddr = _profile->GetUEVars()->GetBaseAddress();
    uintptr_t namesPtr = _profile->GetUEVars()->GetNamesPtr();
    uintptr_t objectsArrayPtr = _profile->GetUEVars()->GetGUObjectsArrayPtr();
    uintptr_t objObjectsPtr = _profile->GetUEVars()->GetObjObjectsPtr();
    uintptr_t Matrix = _profile->GetUEVars()->GetMatrix();
    uintptr_t Physx = _profile->GetUEVars()->GetPhysx();
    uintptr_t FrameCount = _profile->GetUEVars()->GetFrameCount();
    uintptr_t StaticFindObject = _profile->GetUEVars()->GetStaticFindObject();
    uintptr_t NativeAndroidApp = _profile->GetUEVars()->GetNativeAndroidApp();
    uintptr_t ProcessEvent = _profile->GetUEVars()->GetProcessEvent();
    uintptr_t ProcessEventIdx = FindProcessEventIdx(ProcessEvent);
    if (!ProcessEventIdx && ProcessEvent && ProcessEvent >= baseAddr &&
        (ProcessEvent - baseAddr) == 0xE8B79F0)
    {
        ProcessEventIdx = 0x45;
    }
    // Find UEngine & UWorld
    uintptr_t UEnginePtr = 0, UWorldPtr = 0;
    if (((UE_UObject)UEWrappers::GetObjects()->GetObjectPtr(1)).GetIndex() == 1)
    {
        const auto ueElf = _profile->GetUnrealELF();
        auto ueSegs = BuildWritableUESearchSegments(ueElf, kMgr.processID());
        logsBufferFmt.append("UE writable search ranges: {}\n", ueSegs.size());

        UE_UClass UEngineClass = UEWrappers::GetObjects()->FindObject("Class Engine.Engine").Cast<UE_UClass>();
        UE_UClass UWorldClass = UEWrappers::GetObjects()->FindObject("Class Engine.World").Cast<UE_UClass>();

        logsBufferFmt.append("Finding GEngine & GWorld...\n");
        logsBufferFmt.append("{} -> 0x{:X}\n", UEngineClass.GetFullName(), uintptr_t(UEngineClass.GetAddress()));
        logsBufferFmt.append("{} -> 0x{:X}\n", UWorldClass.GetFullName(), uintptr_t(UWorldClass.GetAddress()));

        if (UEngineClass || UWorldClass)
        {
            std::vector<ObjectReferenceCandidate> worldCandidates;

            UEWrappers::GetObjects()->ForEachObject([&](UE_UObject object)
            {
                if (!object.HasFlags(EObjectFlags::ClassDefaultObject))
                {
                    bool isUEngine = UEngineClass && object.IsA(UEngineClass);
                    bool isUWorld = UWorldClass && object.IsA(UWorldClass);
                    if (!isUEngine && !isUWorld) return false;

                    if (isUWorld)
                    {
                        AddObjectReferenceCandidates(ueSegs, object, worldCandidates, 64);
                        return ((!UEngineClass || UEnginePtr != 0) &&
                                (!UWorldClass || worldCandidates.size() >= 64));
                    }

                    // reverse search, start with .bss
                    for (auto it = ueSegs.rbegin(); it != ueSegs.rend(); ++it)
                    {
                        if (!it->writeable) continue;

                        uintptr_t ptr = FindAlignedPointerRefrence(it->startAddress, it->length, object.GetAddress());
                        if (ptr != 0)
                        {
                            if (isUEngine && !UEnginePtr)
                                UEnginePtr = ptr;

                            break;
                        }
                    }
                }
                return ((!UEngineClass || UEnginePtr != 0) &&
                        (!UWorldClass || worldCandidates.size() >= 64));
            });

            if (!worldCandidates.empty())
            {
                for (auto &candidate : worldCandidates)
                    ScoreWorldCandidate(_profile, candidate);

                std::stable_sort(worldCandidates.begin(), worldCandidates.end(),
                                 [](const ObjectReferenceCandidate &lhs,
                                    const ObjectReferenceCandidate &rhs)
                                 {
                                     return lhs.score > rhs.score;
                                 });

                const ObjectReferenceCandidate &best = worldCandidates.front();
                UWorldPtr = best.slot;
                logsBufferFmt.append("GWorld candidates: {}\n", worldCandidates.size());
                for (size_t i = 0; i < std::min<size_t>(worldCandidates.size(), 8); ++i)
                {
                    const auto &candidate = worldCandidates[i];
                    logsBufferFmt.append("  [{}] slot [<Base> + 0x{:X}] -> 0x{:X}, score {}, {}, {}\n",
                                         i, candidate.slot - baseAddr, candidate.object,
                                         candidate.score, candidate.name, candidate.reason);
                }
            }
        }

        if (!UEnginePtr)
            logsBufferFmt.append("Couldn't find refrence to GEngine.\n");
        else
            logsBufferFmt.append("GEngine: [<Base> + 0x{:X}] = 0x{:X}\n", UEnginePtr - baseAddr, UEnginePtr);

        if (!UWorldPtr)
            logsBufferFmt.append("Couldn't find refrence to GWorld.\n");
        else
            logsBufferFmt.append("GWorld: [<Base> + 0x{:X}] = 0x{:X}\n", UWorldPtr - baseAddr, UWorldPtr);

        logsBufferFmt.append("==========================\n");
    }

    if (!Matrix)
        logsBufferFmt.append("Couldn't find refrence to Matrix.\n");
    else
        logsBufferFmt.append("Matrix: [<Base> + 0x{:X}] = 0x{:X}\n", Matrix - baseAddr, Matrix);
    if (!Physx)
        logsBufferFmt.append("Couldn't find refrence to Physx.\n");
    else
        logsBufferFmt.append("Physx: [<Base> + 0x{:X}] = 0x{:X}\n", Physx - baseAddr, Physx);
    if (!FrameCount)
        logsBufferFmt.append("Couldn't find refrence to FrameCount.\n");
    else
        logsBufferFmt.append("FrameCount: [<Base> + 0x{:X}] = 0x{:X}\n", FrameCount - baseAddr, FrameCount);
    if (!StaticFindObject)
        logsBufferFmt.append("Couldn't find refrence to StaticFindObject.\n");
    else
        logsBufferFmt.append("StaticFindObject: [<Base> + 0x{:X}] = 0x{:X}\n", StaticFindObject - baseAddr, StaticFindObject);
    if (!NativeAndroidApp)
        logsBufferFmt.append("Couldn't find refrence to NativeAndroidApp.\n");
    else
        logsBufferFmt.append("NativeAndroidApp: [<Base> + 0x{:X}] = 0x{:X}\n", NativeAndroidApp - baseAddr, NativeAndroidApp);
    if (!ProcessEvent)
        logsBufferFmt.append("Couldn't find refrence to ProcessEvent.\n");
    else
        logsBufferFmt.append("ProcessEvent: [<Base> + 0x{:X}] = 0x{:X}\n", ProcessEvent - baseAddr, ProcessEvent);
    if (!ProcessEventIdx)
        logsBufferFmt.append("Couldn't find ProcessEventIdx.\n");
    else
        logsBufferFmt.append("ProcessEventIdx: {}\n", ProcessEventIdx);
    logsBufferFmt.append("==========================\n");
    UE_Pointers uEPointers{};
    uEPointers.Names = namesPtr - baseAddr;
    uEPointers.UObjectArray = objectsArrayPtr - baseAddr;
    uEPointers.ObjObjects = objObjectsPtr - baseAddr;
    uEPointers.Engine = UEnginePtr ? (UEnginePtr - baseAddr) : 0;
    uEPointers.World = UWorldPtr ? (UWorldPtr - baseAddr) : 0;
    uEPointers.Matrix = Matrix ? (Matrix - baseAddr) : 0;
    uEPointers.Physx = Physx ? (Physx - baseAddr) : 0;
    uEPointers.FrameCount = FrameCount ? (FrameCount - baseAddr) : 0;
    uEPointers.StaticFindObject = StaticFindObject ? (StaticFindObject - baseAddr) : 0;
    uEPointers.NativeAndroidApp = NativeAndroidApp ? (NativeAndroidApp - baseAddr) : 0;
    uEPointers.ProcessEvent = ProcessEvent ? (ProcessEvent - baseAddr) : 0;
    uEPointers.ProcessEventIdx = ProcessEventIdx;

    offsetsBufferFmt.append("#pragma once\n\n#include <cstdint>\n\n\n");
    offsetsBufferFmt.append("{}\n\n{}", _profile->GetOffsets()->ToString(), uEPointers.ToString());
}

void UEDumper::GatherUObjects(BufferFmt &logsBufferFmt, BufferFmt &objsBufferFmt, UEPackagesArray &packages, const ProgressCallback &progressCallback)
{
    logsBufferFmt.append("Gathering UObjects...\n");

    if (UEWrappers::GetObjects()->GetNumElements() <= 0)
    {
        logsBufferFmt.append("UEWrappers::GetObjects()->GetNumElements() <= 0\n");
        logsBufferFmt.append("==========================\n");
        return;
    }

    if (((UE_UObject)UEWrappers::GetObjects()->GetObjectPtr(1)).GetIndex() != 1)
    {
        logsBufferFmt.append("UEWrappers::GetObjects()->GetObjectPtr(1).GetIndex() != 1\n");
        logsBufferFmt.append("==========================\n");
        return;
    }

    int objectsCount = UEWrappers::GetObjects()->GetNumElements();
    SimpleProgressBar objectsProgress(objectsCount);
    if (progressCallback)
        progressCallback(objectsProgress);

    for (int i = 0; i < objectsCount; i++)
    {
        UE_UObject object = UEWrappers::GetObjects()->GetObjectPtr(i);
        if (object)
        {
            if (object.IsA<UE_UFunction>() || object.IsA<UE_UStruct>() || object.IsA<UE_UEnum>())
            {
                bool found = false;
                auto packageObj = object.GetPackageObject();
                for (auto &pkg : packages)
                {
                    if (pkg.first == packageObj)
                    {
                        pkg.second.push_back(object);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    packages.push_back(std::make_pair(packageObj, std::vector<UE_UObject>(1, object)));
                }
            }

            objsBufferFmt.append("[{:010}] [{:016x}]: {}\n", object.GetIndex(),reinterpret_cast<uintptr_t>(object.GetAddress()) ,object.GetFullName());
        }

        objectsProgress++;
        if (progressCallback)
            progressCallback(objectsProgress);
    }

    logsBufferFmt.append("Gathered {} Objects (Packages {})\n", objectsCount, packages.size());
    logsBufferFmt.append("==========================\n");
}

void UEDumper::DumpAIOHeader(BufferFmt &logsBufferFmt, BufferFmt &aioBufferFmt, BufferFmt &enumsBufferFmt, BufferFmt &structsBufferFmt, BufferFmt &classesBufferFmt, BufferFmt &offsetBufferFmt, UEPackagesArray &packages, const ProgressCallback &progressCallback)
{
    int packages_saved = 0;
    std::string packages_unsaved{};

    int classes_saved = 0;
    int structs_saved = 0;
    int enums_saved = 0;

    static bool processInternal_once = false;
    std::vector<dumper_jf_ns::JsonFunction> jsonFallbackFunctions;

    auto pushFallbackExec = [&](const std::string &owner, const auto &func)
    {
        if (owner.empty() || func.Name.empty() || !func.Func)
            return;

        std::string execFuncName = "exec";
        execFuncName += func.Name;
        jsonFallbackFunctions.push_back({owner, execFuncName, func.Func});
    };

    aioBufferFmt.append("#pragma once\n\n#include \"SDK_Enums.hpp\"\n#include \"SDK_Structs.hpp\"\n#include \"SDK_Classes.hpp\"\n");
    enumsBufferFmt.append("#pragma once\n\n#include <cstdio>\n#include <string>\n#include <cstdint>\n\n\n");
    structsBufferFmt.append("#pragma once\n\n#include <cstdio>\n#include <string>\n#include <cstdint>\n\n\n");
    classesBufferFmt.append("#pragma once\n\n#include <cstdio>\n#include <string>\n#include <cstdint>\n\n\n");
    offsetBufferFmt.append("#pragma once\n\n// Auto-generated UE SDK member offsets\n// Use as: SDKOffset::ClassName::MemberName\n#include <cstdint>\n\nnamespace SDKOffset\n{{\n\n");

    SimpleProgressBar dumpProgress(int(packages.size()));
    if (progressCallback)
        progressCallback(dumpProgress);

    for (UE_UPackage package : packages)
    {
        package.Process();

        dumpProgress++;
        if (progressCallback)
            progressCallback(dumpProgress);

        if (!package.AppendToBuffers(&enumsBufferFmt, &structsBufferFmt, &classesBufferFmt))
        {
            packages_unsaved += "\t";
            packages_unsaved += (package.GetObject().GetName() + ",\n");
            continue;
        }

        packages_saved++;
        classes_saved += package.Classes.size();
        structs_saved += package.Structures.size();
        enums_saved += package.Enums.size();

        package.AppendOffsetsToBuffer(&offsetBufferFmt);
        for (const auto &cls : package.Classes)
        {
            for (const auto &func : cls.Functions)
            {
                // UObject::ProcessInternal for blueprint functions
                if (!processInternal_once && (func.EFlags & FUNC_BlueprintEvent) && func.Func)
                {
                    dumper_jf_ns::jsonFunctions.push_back({"UObject", "ProcessInternal", func.Func});
                    processInternal_once = true;
                }

                pushFallbackExec(cls.Name, func);

                if ((func.EFlags & FUNC_Native) && func.Func)
                {
                    std::string execFuncName = "exec";
                    execFuncName += func.Name;
                    dumper_jf_ns::jsonFunctions.push_back({cls.Name, execFuncName, func.Func});
                }
            }
        }

        for (const auto &st : package.Structures)
        {
            for (const auto &func : st.Functions)
            {
                pushFallbackExec(st.Name, func);

                if ((func.EFlags & FUNC_Native) && func.Func)
                {
                    std::string execFuncName = "exec";
                    execFuncName += func.Name;
                    dumper_jf_ns::jsonFunctions.push_back({st.Name, execFuncName, func.Func});
                }
            }
        }
    }

    if (dumper_jf_ns::jsonFunctions.empty() && !jsonFallbackFunctions.empty())
    {
        logsBufferFmt.append("Auto 模式原生函数标志异常，script.json 改用 Func 地址兜底导出。\n");
        dumper_jf_ns::jsonFunctions = std::move(jsonFallbackFunctions);
    }

    offsetBufferFmt.append("\n}}\n");

    logsBufferFmt.append("Saved packages: {}\nSaved classes: {}\nSaved structs: {}\nSaved enums: {}\n", packages_saved, classes_saved, structs_saved, enums_saved);

    if (packages_unsaved.size())
    {
        packages_unsaved.erase(packages_unsaved.size() - 2);
        logsBufferFmt.append("Unsaved packages: [\n{}\n]\n", packages_unsaved);
    }

    logsBufferFmt.append("==========================\n");
}
