#pragma once

#include <cstdint>

namespace ersc_fix::game {

inline uint32_t kWorldChrManPtr = 0x3D69FF8;
inline uint32_t kCsEventFlagManPtr = 0x3D6C4B8;
inline uint32_t kGetEventFlag = 0x5FA250;

constexpr uint32_t kWcmPlayerSetCapacity = 0x10EF0;
constexpr uint32_t kWcmPlayerSetEntries = 0x10EF8;
constexpr uint32_t kWcmMainPlayer = 0x1E508;

constexpr uint32_t kWcmCurrentBlock = 0x1E51C;
constexpr uint32_t kWcmSummonBuddyManager = 0x1E538;
constexpr uint32_t kBuddyManChrSet = 0x30;
constexpr uint32_t kChrSetCapacity = 0x10;
constexpr uint32_t kChrSetEntries = 0x18;

constexpr uint32_t kChrHandle = 0x8;
constexpr uint32_t kChrStateBytes = 0x18;
constexpr uint32_t kChrModelIns = 0x50;
constexpr uint32_t kChrCtrl = 0x58;
constexpr uint32_t kChrFlagsBytes = 0x1B0;

constexpr uint32_t kChrLoadState = 0x1B0;
constexpr uint32_t kChrLoadStateExtra = 0x1B8;
constexpr uint32_t kLoadStateRemoteBit = 0x80;
constexpr uint32_t kChrNpcParamId = 0x60;
constexpr uint32_t kChrNpcId = 0x64;

constexpr uint32_t kChrUpdateInterval = 0xB4;
constexpr uint32_t kChrFlags1C4 = 0x1C4;
constexpr uint8_t kChrFlagUpdateIntervalPinned = 0x01;
constexpr uint32_t kChrUpdateMode = 0x414;
constexpr int32_t kPinnedUpdateInterval = 30;
constexpr uint32_t kChrModules = 0x190;
constexpr uint32_t kModulesRide = 0xE8;
constexpr uint32_t kModulesPhysics = 0x68;
constexpr uint32_t kPhysicsPosition = 0x70;
constexpr uint32_t kPhysicsPositionPrev = 0x80;

constexpr uint32_t kCtrlDeadFlags = 0xF0;
constexpr uint32_t kCtrlStateObj = 0xC8;
constexpr uint32_t kCtrlStateDeadWord = 0x24;
constexpr uint32_t kPlayerLifeState = 0x7F0;

constexpr uint32_t kChrActivationState = 0x1C0;
constexpr uint32_t kChrBlockId = 0x30;
constexpr uint32_t kChrFadeAlpha = 0x24C;
constexpr uint32_t kChrFadeDelta = 0x250;
constexpr uint32_t kLoadStateAwayClearedBit = 0x10;
constexpr uint32_t kChrSetEntryPtr = 0x10;
constexpr uint32_t kEntryBlockState = 0x8;
constexpr uint32_t kEntryFlags = 0xA;

constexpr uint32_t kCtrlManipulator = 0x18;
constexpr uint32_t kCtrlManipulatorOverride = 0x3B0;

inline uint32_t kVtNetworkManipulator = 0x2A2E1B8;
inline uint32_t kVtPadManipulator = 0x2A2E788;

inline uint32_t kRideMount = 0;
inline uint32_t kChrSetDisabled = 0;
inline uint32_t kSetEventFlag = 0;
inline uint32_t kVolatileReset = 0;
inline uint32_t kFlagBlockApply = 0;
inline uint32_t kPackBlockFlags = 0;
inline uint32_t kPackVolatileFlags = 0;
inline uint32_t kFlagSlotLegacy = 0;
inline uint32_t kBulletSpawn = 0;
inline uint32_t kFieldInsResolve = 0;
inline uint32_t kRideDismount = 0;

inline uint32_t kNetPlayerSpawn = 0;

constexpr uint32_t kPgdRideHorseHandle = 0x950;
constexpr uint32_t kPgdRideState = 0x8E0;
constexpr uint32_t kRideStateFlags = 0x34;
constexpr uint32_t kPlayerGameData = 0x580;
inline uint32_t kVtNetAIManipulator = 0x2A2D858;
inline uint32_t kVtRideManipulator = 0x2A2F118;

constexpr uint32_t kNetManipTarget = 0xE0;
constexpr uint32_t kNetManipVelocity = 0x100;

constexpr uint32_t kNetAiHasRideData = 0x1DD;
constexpr uint32_t kNetAiShouldMount = 0x1DE;
constexpr uint32_t kNetAiRideHandle = 0x1E0;

constexpr uint32_t kRideNode = 0x10;
constexpr uint32_t kRideLastMounted = 0x18;
constexpr uint32_t kRideHasParam = 0x31;
constexpr uint32_t kRideIsRideChr = 0x33;
constexpr uint32_t kRideIsMounting = 0x162;
constexpr uint32_t kRideIsMounted = 0x163;
constexpr uint32_t kRideNodeState = 0x50;
constexpr uint32_t kRideNodeMounting = 3, kRideNodeRiding = 5, kRideNodeDismount = 7;

constexpr uint32_t kChrRideRequestObj = 0x18;
constexpr uint32_t kRideReqState = 0x84;
constexpr uint32_t kRideReqHandle = 0x88;
constexpr uint32_t kRideReqMount = 5;
constexpr uint32_t kLoadStateHealthy = 0x496;
constexpr int32_t kTorrentNpcId = 8002;

inline uint32_t kSessionManPtr = 0x3D7E540;
constexpr uint32_t kSessMyId = 0x8;
constexpr uint32_t kSessRole = 0xC;
constexpr uint32_t kSessState = 0x10;
constexpr uint32_t kSessBlockOwnership = 0x30;

constexpr uint32_t kOwnBlockOwnerMap = 0x8;
constexpr uint32_t kOwnOwnedSet = 0x20;
constexpr uint32_t kOwnSessionList = 0x38;
constexpr uint32_t kSessEntryId = 0x10;
constexpr uint32_t kSessEntryLoadedHead = 0x20;
constexpr uint32_t kSessEntryLoadedKey = 0x1C;

constexpr uint32_t kTreeNodeLeft = 0x0;
constexpr uint32_t kTreeNodeParent = 0x8;
constexpr uint32_t kTreeNodeRight = 0x10;
constexpr uint32_t kTreeNodeIsNil = 0x19;
constexpr uint32_t kTreeNodeValue = 0x20;
constexpr uint32_t kTreeNodeMapValue = 0x28;

constexpr uint32_t kWcmChrSetHolder = 0x10D90;
constexpr uint32_t kChrSetHolderBaseIndex = 0x140;
constexpr uint32_t kWcmChrSets = 0x10EE0;
constexpr uint32_t kWcmChrSetStride = 0x58;
constexpr uint32_t kWcmNetChrSync = 0x1E5E0;
constexpr uint32_t kNetSyncSlotCount = 0x8;
constexpr uint32_t kNetSyncSlots = 0x10;
constexpr uint32_t kNetSyncSlotChrSet = 0x8;
constexpr uint32_t kNetSyncSlotItems = 0x10;
constexpr uint32_t kChrNetHandle = 0x70;

struct FieldInsHandle {
    int32_t block_id;
    uint32_t selector;
    bool valid() const { return selector != 0xFFFFFFFFu && block_id != -1; }
    bool operator==(const FieldInsHandle& o) const { return block_id == o.block_id && selector == o.selector; }
};

}
