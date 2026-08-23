#pragma once

#include <Poseidon/Foundation/Common/FltOpts.hpp>

// universal conversions

// list of all classes defined
// file objects
namespace Poseidon {
    class QIStream; class QOStream; class QIFStream; class QOFStream; class QOFStream;
    class QFBank; class IFileBuffer; class QIFStreamB; class QIOS;
    class FileServer;
    class ParamEntry; class ParamClass; class ParamFile;
    class SerializeBinStream;
    struct SoundPars; class IParamArrayValue; class IParamVisibleTest;
    class PreprocessorFunctions;
}
using Poseidon::QIStream;
using Poseidon::QOStream;
using Poseidon::QIFStream;
using Poseidon::QOFStream;
using Poseidon::QFBank;
using Poseidon::IFileBuffer;
using Poseidon::QIFStreamB;
using Poseidon::QIOS;
using Poseidon::FileServer;
using Poseidon::ParamEntry;
using Poseidon::ParamClass;
using Poseidon::ParamFile;
using Poseidon::SoundPars;
using Poseidon::IParamArrayValue;
using Poseidon::IParamVisibleTest;
using Poseidon::PreprocessorFunctions;
using Poseidon::SerializeBinStream;

class SecondaryThread;

// object
class Visual;
// Note: Frame and FrameBase are defined in namespace Poseidon (Core/Visual.hpp)
namespace Poseidon { class Shape; }
using Poseidon::Shape;
namespace Poseidon { class LODShape; }
using Poseidon::LODShape;
namespace Poseidon { class LODShapeWithShadow; }
using Poseidon::LODShapeWithShadow;
namespace Poseidon { class Object; } using Poseidon::Object;
namespace Poseidon::render::frame { struct CameraView; }
namespace Poseidon { class Camera; } using Poseidon::Camera;

// animations
namespace Poseidon { class AnimationPhase; }
using Poseidon::AnimationPhase;
namespace Poseidon { class Animation; } using Poseidon::Animation;
namespace Poseidon { class AnimationWithCenter; } using Poseidon::AnimationWithCenter;

// object clases
namespace Poseidon { class Church; } using Poseidon::Church;
class DogHouse;
class ChickenHouse;
class BirdsTree;

// vehicles

class Simul;
namespace Poseidon { class EntityAI; } using Poseidon::EntityAI;
namespace Poseidon { class Entity; } using Poseidon::Entity;
typedef EntityAI VehicleWithAI;
typedef Entity Vehicle;
namespace Poseidon { class Person; } using Poseidon::Person;

namespace Poseidon { class EntityAIType; } using Poseidon::EntityAIType;
namespace Poseidon { class EntityType; } using Poseidon::EntityType;
typedef EntityAIType VehicleType;
typedef EntityType VehicleNonAIType;

namespace Poseidon { class AttachedOnVehicle; } using Poseidon::AttachedOnVehicle;
namespace Poseidon { class Transport; } using Poseidon::Transport;

class PauseMenu;
namespace Poseidon { class AbstractUI; }
using Poseidon::AbstractUI;
namespace Poseidon { class AbstractOptionsUI; }
using Poseidon::AbstractOptionsUI;

namespace Poseidon { class AmmoType; }
using Poseidon::AmmoType;
class WeaponInfo;

namespace Poseidon { class Tank; } using Poseidon::Tank;
namespace Poseidon { class TankType; } using Poseidon::TankType;
namespace Poseidon { class TankWithAI; } using Poseidon::TankWithAI;
namespace Poseidon { class Helicopter; } using Poseidon::Helicopter;
namespace Poseidon { class HelicopterAuto; } using Poseidon::HelicopterAuto;
namespace Poseidon { class HelicopterType; } using Poseidon::HelicopterType;

namespace Poseidon { class SeaGull; } using Poseidon::SeaGull;
namespace Poseidon { class SeaGullAuto; } using Poseidon::SeaGullAuto;

namespace Poseidon { class Car; } using Poseidon::Car;
namespace Poseidon { class CarType; } using Poseidon::CarType;

namespace Poseidon { class Man; } using Poseidon::Man;
namespace Poseidon { class ManType; } using Poseidon::ManType;
#define SoldierWithAI Soldier
namespace Poseidon { class Soldier; } using Poseidon::Soldier;

namespace Poseidon { class Shot; } using Poseidon::Shot;
namespace Poseidon { class Missile; } using Poseidon::Missile;
namespace Poseidon { class ShotShell; } using Poseidon::ShotShell;
namespace Poseidon { class ShotBullet; } using Poseidon::ShotBullet;
namespace Poseidon { class Smoke; }
using Poseidon::Smoke;
namespace Poseidon { class Explosion; }
using Poseidon::Explosion;
namespace Poseidon { class Cloudlet; }
using Poseidon::Cloudlet;
namespace Poseidon { class SmokeSourceVehicle; }
using Poseidon::SmokeSourceVehicle;
namespace Poseidon { class SmokeSourceOnVehicle; }
using Poseidon::SmokeSourceOnVehicle;

namespace Poseidon { class CloudletSource; }
using Poseidon::CloudletSource;
namespace Poseidon { class SmokeSource; }
using Poseidon::SmokeSource;
namespace Poseidon { class DustSource; }
using Poseidon::DustSource;
namespace Poseidon { class WeaponCloudsSource; }
using Poseidon::WeaponCloudsSource;

// AI

namespace Poseidon { class AI; } using Poseidon::AI;
namespace Poseidon { class AIUnit; } using Poseidon::AIUnit;
namespace Poseidon { class AISubgroup; } using Poseidon::AISubgroup;
namespace Poseidon { class AIGroup; } using Poseidon::AIGroup;
namespace Poseidon { class AICenter; } using Poseidon::AICenter;

// Mission Templates
namespace Poseidon { struct ArcadeTemplate; } using Poseidon::ArcadeTemplate;
namespace Poseidon { struct ArcadeIntel; } using Poseidon::ArcadeIntel;
namespace Poseidon { struct ArcadeGroupInfo; } using Poseidon::ArcadeGroupInfo;
namespace Poseidon { struct ArcadeUnitInfo; } using Poseidon::ArcadeUnitInfo;
namespace Poseidon { struct ArcadeWaypointInfo; } using Poseidon::ArcadeWaypointInfo;
namespace Poseidon { struct ArcadeSensorInfo; } using Poseidon::ArcadeSensorInfo;
struct ArcadeFlagInfo;
namespace Poseidon { struct ArcadeMarkerInfo; } using Poseidon::ArcadeMarkerInfo;
struct ArcadeBuildingInfo;

class TankPilot;
class HeliPilot;
class CarPilot;
class SoldierPilot;

class Animator;
class AnimatorLinear;
class AnimatorSet;

// scene
namespace Poseidon { class Scene; } using Poseidon::Scene;
namespace Poseidon { class World; } using Poseidon::World;
namespace Poseidon { class Landscape; }
using Poseidon::Landscape;
class WaterLevel;

namespace Poseidon { union GeographyInfo; } using Poseidon::GeographyInfo;

// timing
namespace Poseidon::Foundation { class AbstractTime; class Time; class TimeSec; class UITime; }

// vertex
namespace Poseidon { class Color; }
namespace Poseidon::Model { class Vertex; }
using Poseidon::Model::Vertex;
namespace Poseidon { class TLVertex; }
using Poseidon::TLVertex;
namespace Poseidon::Asset::Formats::MLOD { class VertexTable; }
namespace Poseidon { class TLVertexTable; }
using Poseidon::TLVertexTable;

// different engine implementations

namespace Poseidon { class Engine; }
using Poseidon::Engine;

class EngineMyst;

class EngineDD;
class EngineRampFull;
class EngineRampWin;

class EngineD3DFull;
class EngineD3DWin;

// polygons
namespace Poseidon { class Poly; }
using Poseidon::Poly;

class SFaceArray;
namespace Poseidon { class FaceArray; }
using Poseidon::FaceArray;

class ClippedFaces;

// light
class LightDirectional; // main light types
namespace Poseidon { class LightSun; }
using Poseidon::LightSun;

namespace Poseidon { class LightPoint; }
using Poseidon::LightPoint;
namespace Poseidon { class LightReflector; }
using Poseidon::LightReflector;

namespace Poseidon { class LightList; } using Poseidon::LightList;

// textures for software ramp mode
class Ramp;
class TextureRamp;
class TextBankRamp;
class MipmapLevelRamp;

// abstract textures

class PacLevel;
namespace Poseidon { class Texture; }
using Poseidon::Texture;
namespace Poseidon { class AbstractTextBank; }
using Poseidon::AbstractTextBank;
namespace Poseidon { class AnimatedTexture; }
using Poseidon::AnimatedTexture;

namespace Poseidon { class Font; }
using Poseidon::Font;

#define AbstractMipmapLevel PacLevelMem
namespace Poseidon { class PacLevelMem; }
using Poseidon::PacLevelMem;
class PacLevel;

// 3D sound engine

namespace Poseidon
{
class IWave;
class IAudioSystem;

class Wave;
class SoundSystem;

class RadioChannel;
class RadioMessage;
class Speaker;
}
using Poseidon::IWave;
using Poseidon::IAudioSystem;
using Poseidon::Wave;
using Poseidon::SoundSystem;
using Poseidon::RadioChannel;
using Poseidon::RadioMessage;
using Poseidon::Speaker;

// enums
typedef short VertexIndex; // better memory usage

typedef unsigned int ClipFlags;
enum
{
	ClipNone=0,
	ClipFront=1,ClipBack=2,
	ClipLeft=4,ClipRight=8,
	ClipBottom=16,ClipTop=32,
	ClipUser0=0x40,
	//ClipUser1=0x80, // currently not used
	ClipAll = ClipFront|ClipBack|ClipLeft|ClipRight|ClipBottom|ClipTop,

	// surface attributes
	ClipLandMask=0xf00,ClipLandStep=0x100,
	ClipLandNone=ClipLandStep*0, // exclusive - "or" and "and" used
	ClipLandOn=ClipLandStep*1,
	ClipLandUnder=ClipLandStep*2,
	ClipLandAbove=ClipLandStep*4,
	ClipLandKeep=ClipLandStep*8,

	// decal attributes
	ClipDecalMask=0x3000,ClipDecalStep=0x1000,
	ClipDecalNone=ClipDecalStep*0, // exclusive - "or" and "and" used
	ClipDecalNormal=ClipDecalStep*1,
	ClipDecalVertical=ClipDecalStep*2,

	// fogging attributes
	ClipFogMask=0xc000,ClipFogStep=0x4000,
	ClipFogNormal=ClipFogStep*0,
	ClipFogDisable=ClipFogStep*1,
	ClipFogSky=ClipFogStep*2,
	ClipFogShadow=ClipFogStep*3,

	// per vertex lighting attributes
	ClipLightMask=0xf0000,ClipLightStep=0x10000,
	ClipLightNormal=ClipLightStep*0,
	ClipLightSky=ClipLightStep*1,
	ClipLightCloud=ClipLightStep*2,
	ClipLightSun=ClipLightStep*3,
	ClipLightSunHalo=ClipLightStep*4,
	ClipLightMoon=ClipLightStep*5,
	ClipLightMoonHalo=ClipLightStep*6,
	ClipLightStars=ClipLightStep*7,
	ClipLightLine=ClipLightStep*8, // line - alpha used to simulate width

	ClipUserMask=0xff00000,ClipUserStep=0x100000, // used for star brigtness...
	MaxUserValue=0xff,

	//ClipVarMask=0xf0000000,ClipVarStep=0x10000000,
	//ClipUserIsPalette=ClipVarStep*8U,

	//ClipUserIsAlpha=ClipVarStep*1,
	//ClipUserIsBrightness=ClipVarStep*2,
	//ClipUserIsDammage=ClipVarStep*4,

	// all hints mask
	ClipHints=ClipLandMask|ClipDecalMask|ClipFogMask|ClipLightMask|ClipUserMask
};

enum
{
	GrassTexture=1, // special grass layer - use special detail texture
	OnSurface=2, //UnderSurface=4,AboveSurface=8,
	// Nonplanar means that polygon cannot be culled
	// OnSurface means that polygon should be splitted to fit landscape surface
	IsOnSurface=4,NoZBuf=8,NoZWrite=0x10,
	// NoZBuf,NoZWrite disable parts of z-buffer functionality
	NoShadow=0x20,IsShadow=0x40,ShadowDisabled=0x80,
	// NoShadow means that the face should not cast a shadow
	// IsShadow means that the polygon is shadow polygon (dark&transparent)
	// ShadowDisabled means face has been light culled and should not be drawn
	IsAlpha=0x100, // alpha transparent texture - should be z-sorted before drawing
	IsTransparent=0x200, // is chromakey texture (should be z-sorted before drawing)
	IsWater=0x400,IsLight=0x800,
	//IsFlare=0x1000,
	PointSampling=0x1000, // disable is bi- or tri- linear filter
	// IsWater means special transparent face used for reflections
	// IsLight is transparent face used for volumetrical lights
	// IsFlare is transparent face used for flares (basically the same as IsLight)
	NoClamp=0x2000,ClampU=0x4000,ClampV=0x8000,
	// NoClamp means that the texture is tiling and need not be clamped
	IsAnimated=0x10000, // there is an animated texture attached on this face
	//DoAntiAliasing=0x20000,	// toggle AA on/off
	IsAlphaOrdered=0x20000,	// alpha order important - cannot reorder
	NoDropdown=0x40000, // disable speed drop down
	IsAlphaFog=0x80000, // use alpha blending instead of fog
	FogDisabled=0x100000, // fog disabled for entire object
	IsColored=0x200000, // object constant color is set
	IsHidden=0x400000, // face hidden
	BestMipmap=0x800000, // best mipmap forced
	DetailTexture=0x1000000, // use default detail texture
	SpecularTexture=0x2000000, // use default specular texture
	ZBiasMask=0xc000000,ZBiasStep=0x4000000, // control z-bias
	IsHiddenProxy=0x10000000, // face hard-hidden (proxy)
	//PointFilter=0x10000000,
	NoTexMerger=0x20000000, // disable texture merging
	SpecLighting=0x40000000, // lighting unsuitable for D3D
	DisableSun=0x80000000,
};

namespace Poseidon
{
inline int toInt( int x ) {return int(x);}
// Expose the global float/double overloads (FltOpts.hpp) in this namespace.
// Without this, the int overload above hides them, so an unqualified
// toInt(<float>) inside namespace Poseidon binds to toInt(int) and silently
// truncates instead of rounding (e.g. ColorP::B8(0.5) gave 127, not 128).
using ::toInt;

#define MAX_Z 1.0f

// Types defined in Core/ (namespace Poseidon)
class Frame;

} // namespace Poseidon
