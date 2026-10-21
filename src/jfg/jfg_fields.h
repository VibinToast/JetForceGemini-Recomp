// Jet Force Gemini port: the fields of the game's data that the port reads and writes, by name.
//
// Each field is its offset in a structure and its type, so a read or write always has the right size (8-, 16- and
// 32-bit accesses differ in the recompiled game's byte-swapped RDRAM). Use: Obj::PosX::get(obj), Obj::PosX::set(obj, x),
// Character::Ammo::get(ch, weapon). A pointer field is a game address (uint32_t). get/set with an rdram argument use
// that pointer (the MEM_* macros, as code given rdram does); without one, jfg::g_rdram (rd*/wr*, jfg_game.h).
// The accessors compile to the same code as the reads and writes they replace.
//
// The names are the port's own (the decomp's names are private for now). Where a field's meaning is a guess, its
// comment says so. Angles are uint16_t (they wrap); the pitches the port clamps are int16_t.
#pragma once

#include <cstdint>
#include <type_traits>

#include "jfg_game.h"

namespace jfg {

template <typename T, uint32_t Offset>
struct Field {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4, "fields are 8, 16 or 32 bits");
    static constexpr uint32_t offset = Offset;

    static inline T get(uint32_t base) {
        if constexpr (std::is_same_v<T, float>) return rdf(base + Offset);
        else if constexpr (sizeof(T) == 1) return (T)rd8(base + Offset);
        else if constexpr (sizeof(T) == 2) return (T)rd16(base + Offset);
        else return (T)rd32(base + Offset);
    }
    static inline void set(uint32_t base, T value) {
        if constexpr (std::is_same_v<T, float>) wrf(base + Offset, value);
        else if constexpr (sizeof(T) == 1) wr8(base + Offset, (uint8_t)value);
        else if constexpr (sizeof(T) == 2) wr16(base + Offset, (uint16_t)value);
        else wr32(base + Offset, (uint32_t)value);
    }

    // With the caller's rdram (recomp.h's MEM_* macros index a variable called rdram; jfg_game.h names it g_rdram).
    static inline T get(uint8_t* rdram, uint32_t base) {
        static_assert(!std::is_same_v<T, float>, "floats: use get(base)");
        if constexpr (sizeof(T) == 1) return (T)MEM_BU(Offset, (gpr)(int32_t)base);
        else if constexpr (sizeof(T) == 2) return (T)MEM_HU(Offset, (gpr)(int32_t)base);
        else return (T)MEM_W(Offset, (gpr)(int32_t)base);
    }
    static inline void set(uint8_t* rdram, uint32_t base, T value) {
        static_assert(!std::is_same_v<T, float>, "floats: use set(base, value)");
        if constexpr (sizeof(T) == 1) MEM_B(Offset, (gpr)(int32_t)base) = (int8_t)value;
        else if constexpr (sizeof(T) == 2) MEM_H(Offset, (gpr)(int32_t)base) = (int16_t)value;
        else MEM_W(Offset, (gpr)(int32_t)base) = (int32_t)value;
    }
};

// An array of fields: element i is at Offset + i * Stride. (Written out rather than through Field, so it compiles
// exactly like the reads it replaced: one more level of inlining changed the code around it.)
template <typename T, uint32_t Offset, uint32_t Stride = sizeof(T)>
struct ArrayField {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4, "fields are 8, 16 or 32 bits");
    static constexpr uint32_t offset = Offset, stride = Stride;

    static inline T get(uint32_t base, uint32_t i) {
        if constexpr (std::is_same_v<T, float>) return rdf(base + Offset + i * Stride);
        else if constexpr (sizeof(T) == 1) return (T)rd8(base + Offset + i * Stride);
        else if constexpr (sizeof(T) == 2) return (T)rd16(base + Offset + i * Stride);
        else return (T)rd32(base + Offset + i * Stride);
    }
    static inline void set(uint32_t base, uint32_t i, T value) {
        if constexpr (std::is_same_v<T, float>) wrf(base + Offset + i * Stride, value);
        else if constexpr (sizeof(T) == 1) wr8(base + Offset + i * Stride, (uint8_t)value);
        else if constexpr (sizeof(T) == 2) wr16(base + Offset + i * Stride, (uint16_t)value);
        else wr32(base + Offset + i * Stride, (uint32_t)value);
    }
};

// --- objects --------------------------------------------------------------------------------------------------------
// A game object (anything in play: characters, enemies, squad members, bolts, mines, Floyd, ...).
namespace Obj {
using Rotation   = ArrayField<uint16_t, 0x00>; // yaw, pitch, roll (index 0, 1, 2)
using Yaw        = Field<uint16_t, 0x00>;
using PosX       = Field<float, 0x0C>;
using PosY       = Field<float, 0x10>;
using PosZ       = Field<float, 0x14>;
using AnimPhase  = Field<float, 0x28>;     // animation position, 0..1 in looping animations
using Opacity    = Field<uint8_t, 0x39>;   // 0xFF = fully opaque
using ModelIndex = Field<int8_t, 0x3A>;    // which of its model instances (Models) is drawn
using Anim       = Field<int8_t, 0x3B>;    // current animation
using Def        = Field<uint32_t, 0x40>;  // its definition (ObjDef)
using Kind       = Field<int16_t, 0x48>;   // object type
using Damage     = Field<uint32_t, 0x4C>;  // its damage data (Damage), 0 = none
using Shadow     = Field<uint32_t, 0x50>;  // its shadow data, 0 = no shadow
using Data       = Field<uint32_t, 0x68>;  // its kind's own data (PlayerData, SquadMemberData, SquadronData, BoltData, MineData, ...)
using Models     = Field<uint32_t, 0x6C>;  // for a 3D model object: its model instances (ModelInstance pointers)
}

// An object definition (Obj::Def), shared by every object of a kind.
namespace ObjDef {
using DrawRange   = Field<uint16_t, 0x16>; // drawn up to this distance, 0 = always (meaning partly a guess)
using DrawType    = Field<uint8_t, 0x1E>;  // 0 3D model, 1 sprite, 4 custom
using LodDistance = Field<float, 0xCC>;    // past this depth the next, simpler model is drawn; 0 = no LOD
}

// Damage data (Obj::Damage).
namespace Damage {
using Health = Field<int16_t, 0x06>; // 256 per unit
}

// A 3D model instance (Obj::Models[Obj::ModelIndex]).
namespace ModelInstance {
using Model     = Field<uint32_t, 0x00>; // the Model
using PoseTicks = Field<uint8_t, 0x0A>;  // ticks to animate at the next draw; the draw poses the model when non-zero
using Buffer    = Field<uint8_t, 0x0B>;  // which of its two buffers (bones, vertices) is current, 0 or 1
using Bones     = ArrayField<uint32_t, 0x10>; // bone matrices, one per buffer
}

namespace Model {
using Animated = Field<uint8_t, 0x4E>; // non-zero: an animated model
}

// A character's own data while in play (Obj::Data of the player's object).
namespace PlayerData {
using SavedX       = Field<float, 0x3C>;    // position kept for working out its velocity
using SavedY       = Field<float, 0x40>;
using SavedZ       = Field<float, 0x44>;
using BodyYawCopy  = Field<uint16_t, 0x106>; // the object's yaw and the copies the control code keeps and writes back
using BodyYaw      = Field<uint16_t, 0x11C>;
using BodyYawCopy2 = Field<uint16_t, 0x122>;
using BodyYawCopy3 = Field<uint16_t, 0x1C6>;
using BodyYawCopy4 = Field<uint16_t, 0x1CA>;
using ReticleYaw   = Field<int16_t, 0x1CE>;  // the aim's offset from the body, and two copies
using ReticleYawCopy  = Field<int16_t, 0x1DC>;
using ReticleYawCopy2 = Field<int16_t, 0x246>;
using ReticlePitch = Field<int16_t, 0x1E0>;
using ViewPitch    = Field<int16_t, 0x1E2>;
using AimZoom      = Field<float, 0x1EC>;   // sniper zoom, 1..5
using AimZoomRate  = Field<float, 0x1F0>;
using Charge       = Field<int16_t, 0x53E>; // weapon charge
using TargetLock   = Field<uint8_t, 0x541>; // high nibble: the target lock's countdown
using PistolHeat   = Field<uint8_t, 0x542>; // rapid-fire heat
using State        = Field<uint8_t, 0x568>; // StateAiming, StateJumping, ...
using ThrowLock    = Field<int8_t, 0x577>;  // controls held during a throw, in frames
using LastWeapon   = Field<uint8_t, 0x585>; // the weapon slot selected before
using Fuel         = Field<int16_t, 0x58C>; // jetpack fuel
constexpr uint8_t StateJumping = 3, StateSwimming = 4, StateAiming = 0x0B;
}

// Floyd's own data (Obj::Data of Floyd).
namespace SidekickData {
using Yaw       = Field<uint16_t, 0x60>;
using Pitch     = Field<int16_t, 0x62>;
using Roll      = Field<uint16_t, 0x64>;
using PitchRate = Field<int16_t, 0x68>;
}

// A squadron (Obj::Data of a squadron object): the group squad members belong to.
namespace SquadronData {
using MemberCount = Field<uint8_t, 0x00>;
using FirstMember = Field<uint32_t, 0x58>; // its first member's object
using Created     = Field<uint8_t, 0x63>;  // members brought into play (meaning partly a guess)
}

// A squad member (Obj::Data of a squad member's object).
namespace SquadMemberData {
using Squadron   = Field<uint32_t, 0x24>; // its squadron's object
using NextMember = Field<uint32_t, 0x28>; // the next member's object
using Target     = Field<uint32_t, 0x64>; // the object it targets
using Carried    = Field<uint32_t, 0xB4>; // its weapon or carried object
constexpr uint32_t Size = 0xB0;           // at least (the port reads up to +0xAC)
}

// A bolt (Obj::Data of a weapon's shot).
namespace BoltData {
using Damage = Field<uint8_t, 0x61>;
using Speed  = Field<float, 0x64>;
using Life   = Field<int16_t, 0x6A>;
using Scale  = Field<float, 0x6C>;
using Owner  = Field<uint32_t, 0x70>; // the object that fired it
using Weapon = Field<int8_t, 0x7C>;
}

// A mine (Obj::Data of a mine).
namespace MineData {
using State = Field<uint8_t, 0x01>; // 5, 6: going off / removed
using Armed = Field<uint8_t, 0xAC>; // cleared when it's removed (meaning a guess)
}

// --- the game ---------------------------------------------------------------------------------------------------------
// A character's saved state (the game's data, ADDR_gameplay, + Game::Characters + character * Character::Size).
namespace Character {
using Health       = Field<int16_t, 0x00>;  // 256 per unit
using HealthLevel  = Field<int16_t, 0x02>;  // 6..12
using WeaponsOwned = Field<uint16_t, 0x0A>; // bitmask
using Ammo         = ArrayField<uint16_t, 0x14>;  // [16], by weapon
using MaxCharge    = ArrayField<uint8_t, 0x54>;   // [16], by weapon; << 5 is the maximum charge
using Collected    = ArrayField<uint8_t, 0x66>;   // 9 bytes of bits
using WeaponCount  = Field<uint8_t, 0x6F>;  // weapons owned; switching needs 2 or more
using CurrentWeapon = Field<uint8_t, 0x70>; // NoWeapon = none
using Resume       = Field<uint8_t, 0x74>;  // where it resumes (see jfg_area_progress.cpp)
constexpr uint8_t NoWeapon = 0xFF;
constexpr uint32_t Size = 0x76;
}

// The game's data (rd32(ADDR_gameplay)).
namespace Game {
using SaveSlot    = Field<int8_t, 0x03>;   // the save file played from, -1 = none
using CountRescued = Field<uint16_t, 0x04>;
using CountLeft   = Field<uint16_t, 0x06>;
using TribalsRescued = ArrayField<uint16_t, 0x5C>; // by room
using TribalsKilled  = ArrayField<uint16_t, 0xDC>; // by room
constexpr uint32_t Characters = 0x15C;     // Character[3]
constexpr uint32_t RegionStats = 0x334;    // 8 bytes per region: u8 best rescued, u8 killed, u16, u32
using WeaponShots = ArrayField<uint32_t, 0x474>; // [16]
using RegionTime  = Field<uint32_t, 0x514>;
}

// A weapon's definition (ADDR_weaponTable + weapon * WeaponDef::Size).
namespace WeaponDef {
using Damage     = Field<uint8_t, 0x03>;
using Damage2    = Field<uint8_t, 0x04>;  // set with Damage
using Delay      = Field<uint8_t, 0x05>;
using ThrowDelay = Field<int8_t, 0x06>;
using Life       = Field<uint8_t, 0x07>;
using SpreadX    = Field<int16_t, 0x08>;
using SpreadY    = Field<int16_t, 0x0A>;
using Speed      = Field<float, 0x10>;
constexpr uint32_t Size = 0x30;
}

// A camera (ADDR_cameraActorArray + camera * Camera::Size).
namespace Camera {
using Yaw   = Field<uint16_t, 0x00>; // minus the heading it looks along
using Pitch = Field<uint16_t, 0x02>;
using PosX  = Field<float, 0x0C>;
using PosY  = Field<float, 0x10>;
using PosZ  = Field<float, 0x14>;
using Fov   = Field<float, 0x2C>;
constexpr uint32_t Size = 0x4C;
}

// The level's header (rd32(ADDR_levelHeader)).
namespace LevelHeader {
using Region   = Field<uint8_t, 0x102>;
using RoomFlag = Field<uint8_t, 0x103>;
}

// The game's progress flags (ADDR_globalflags); the flag numbers each word holds.
namespace GlobalFlags {
using Flags0To19  = Field<uint32_t, 0x64>;
using Flags82To87 = Field<uint16_t, 0x68>;
using Flags66To70 = Field<uint8_t, 0x6A>;
using Flags73To76 = Field<uint8_t, 0x6B>;
}

// A controller's state (libultra's OSContPad; ADDR_sControllerCurrData + controller * Pad::Size).
namespace Pad {
using Buttons = Field<uint16_t, 0x00>;
using StickX  = Field<int8_t, 0x02>;
using StickY  = Field<int8_t, 0x03>;
constexpr uint32_t Size = 6;
}

// --- engine -----------------------------------------------------------------------------------------------------------
// The sound player (rd32(ADDR_gSoundPlayerPtr)).
namespace SoundPlayer {
constexpr uint32_t EventQueue = 0x14;      // an EventQueue inside it
using FrameTime = Field<uint32_t, 0x4C>;   // time between its periodic events
}

// libaudio's event queue.
namespace EventQueue {
using FreeList   = Field<uint32_t, 0x00>;
using AllocList  = Field<uint32_t, 0x08>;
using EventCount = Field<uint16_t, 0x12>;
}

// The music player (rd32(ADDR_tuneSeqPlayer)).
namespace MusicPlayer {
using State = Field<int32_t, 0x2C>; // 1 = playing
}

// The scheduler (OSSched).
namespace Scheduler {
using FrameCount = Field<uint32_t, 0x300>; // retraces since the last "frame done"
}

// The level's geometry (debug code only).
namespace Track {
using Segments     = Field<uint32_t, 0x04>; // TrackSegment[SegmentCount]
using SegmentCount = Field<int16_t, 0x1A>;
}
namespace TrackSegment {
using Verts      = Field<uint32_t, 0x00>; // TrackVertex[]
using Faces      = Field<uint32_t, 0x04>; // TrackFace[]
using Batches    = Field<uint32_t, 0x0C>; // TrackBatch[BatchCount + 1]
using BatchCount = Field<int16_t, 0x28>;
constexpr uint32_t Size = 0x48;
}
namespace TrackBatch {
using Texture    = Field<uint8_t, 0x00>;
using VertOffset = Field<int16_t, 0x06>;
using FaceOffset = Field<int16_t, 0x08>;
using Flags      = Field<uint32_t, 0x0C>;
constexpr uint32_t Size = 0x10;
}
namespace TrackFace {
using Vertex = ArrayField<uint8_t, 0x01>; // [3]
constexpr uint32_t Size = 0x10;
}
namespace TrackVertex {
using X = Field<int16_t, 0x00>;
using Y = Field<int16_t, 0x02>;
using Z = Field<int16_t, 0x04>;
constexpr uint32_t Size = 10;
}

} // namespace jfg
