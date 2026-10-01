#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

#include <Poseidon/World/Entities/Infantry/Head.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Filesystem/Utf8Paths.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Network/NetworkCustomAssets.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>
#include <Poseidon/Audio/VoiceLangPath.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/UI/Settings/GameSettingsConfig.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>

using namespace Poseidon;
namespace Poseidon
{
RString GetUserDirectory();
}

namespace Poseidon
{
FaceAnimationLook& GFaceAnimation()
{
    static FaceAnimationLook look = []
    {
        FaceAnimationLook l;
        if (const char* e = getenv("POSEIDON_FACE_ANIM"))
        {
            l.enabled = (atoi(e) != 0);
        }
        if (const char* e = getenv("POSEIDON_FACE_ANIM_ALWAYS"))
        {
            l.always = (atoi(e) != 0);
        }
        return l;
    }();
    return look;
}
} // namespace Poseidon

namespace Poseidon
{
FaceAnimDiag& GFaceDiag()
{
    static FaceAnimDiag d;
    return d;
}

bool FaceDebugEnabled()
{
    static const bool on = []
    {
        const char* e = getenv("POSEIDON_FACE_DEBUG");
        return e && atoi(e) != 0;
    }();
    return on;
}

void FaceDiagTick(const char* where)
{
    if (!FaceDebugEnabled())
    {
        return;
    }
    static int n = 0;
    if (++n % 200 != 0)
    {
        return;
    }
    const FaceAnimDiag& d = GFaceDiag();
    LOG_INFO(Audio,
             "FACE-001 [{}] req={} noVb={} set={} offs={} cpuKept={} gpuSkin={} updPosed={} updSkip={} "
             "drawPosed={} drawPal={} verts={} | shared={} clean={} cnt={} hash={} sent={} maxDelta={:.4f} | lvl={} lvlVerts={} drawVerts={} lipSel={} eyeSel={} lmSel={} rmSel={} | pend={} upl={} dir={} refused={} | restore={} vbSet={:#x} vbUnset={:#x}",
             where, d.request, d.requestNoVb, d.requestSet, d.offsets, d.cpuSkinKept, d.gpuSkinned, d.updSkinPosed,
             d.updSkinSkip, d.drawPosed, d.drawPalette, d.updBytes, d.updShared, d.updClean, d.updCount,
             d.updHash, d.updSent, d.poseMaxDelta, d.poseLevel, d.poseVerts, d.drawVerts, d.lipSelSize, d.eyeSelSize, d.lmSelSize, d.rmSelSize,
             d.updPending, d.updUploader, d.updDirect, d.updRefused, d.updRestore, d.vbSet, d.vbUnset);
}
} // namespace Poseidon

bool Head::HasVertexAnimation() const
{
    const FaceAnimationLook& look = GFaceAnimation();
    if (!look.enabled)
    {
        return false;
    }
    if (look.always)
    {
        return true; // wink and micro-mimics too, at the cost of CPU skinning every character
    }
    return _lipInfo != nullptr || _randomLip;
}

bool ManLipInfo::AttachWave(IWave* wave, float freq)
{
    if (!wave)
    {
        LOG_INFO(Audio, "Lip: AttachWave called with null wave - skipping lip sync");
        return false;
    }
    const RString waveName = wave->Name();
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s", (const char*)waveName);
    char* ext = strrchr(buffer, '.');
    if (!ext)
    {
        LOG_INFO(Audio, "Lip: wave name '{}' has no extension - no lip lookup possible", (const char*)waveName);
        return false;
    }
    ext++;
    strcpy(ext, "lip");
    const RString derived(buffer);
    const RString voiceLang(GetSelectedVoiceLanguage().c_str());
    const char* resolvedVia = "derived";
    if (!QIFStreamB::FileExist(buffer))
    {
        // Voice-language fallbacks.  The derived "<wave>.lip" doesn't exist on disk;
        // depending on whether the wave's Name() carries the language suffix or not,
        // try the opposite direction so audio + lip stay paired:
        //   * suffixed wave ("<base>.<lang>.lip") missing -> drop the suffix and try "<base>.lip"
        //   * unsuffixed wave ("<base>.lip") missing      -> add the suffix and try "<base>.<lang>.lip"
        const RString stripped = WithoutLangSuffix(derived, voiceLang);
        if (stripped.GetLength() > 0 && QIFStreamB::FileExist(stripped))
        {
            snprintf(buffer, sizeof(buffer), "%s", (const char*)stripped);
            resolvedVia = "without-lang-suffix fallback";
        }
        else
        {
            const RString suffixed = WithLangSuffix(derived, voiceLang);
            if (suffixed.GetLength() == 0 || !QIFStreamB::FileExist(suffixed))
            {
                LOG_INFO(Audio,
                         "Lip: NO MATCH - wave='{}' voiceLang='{}' tried derived='{}', stripped='{}', "
                         "suffixed='{}'; character will not move lips for this wave",
                         (const char*)waveName, (const char*)voiceLang, (const char*)derived, (const char*)stripped,
                         (const char*)suffixed);
                return false;
            }
            snprintf(buffer, sizeof(buffer), "%s", (const char*)suffixed);
            resolvedVia = "with-lang-suffix fallback";
        }
    }

    const RString lipPath(buffer);
    QIFStreamB f;
    f.AutoOpen(buffer);
    while (!f.eof())
    {
        f.readLine(buffer, sizeof(buffer));
        float time;
        int phase;
        if (sscanf(buffer, "%f, %d", &time, &phase) == 2)
        {
            int index = _items.Add();
            _items[index].time = time;
            _items[index].phase = phase;
        }
        else
        {
            float frame;
            if (sscanf(buffer, "frame = %f", &frame) == 1)
            {
                _frame = frame;
                _invFrame = 1.0 / frame;
            }
        }
    }

    LOG_DEBUG(Audio, "Lip: loaded '{}' via {} (wave='{}', voiceLang='{}', frame={:.3f}s, phonemes={}, freq={:.3f})",
              (const char*)lipPath, resolvedVia, (const char*)waveName, (const char*)voiceLang, _frame, _items.Size(),
              freq);

    _current = 0;
    _freq = freq;
    _start = Glob.time;
    return true;
}

float ManLipInfo::ElapsedOffset() const
{
    return _freq * (Glob.time - _start);
}

bool ManLipInfo::GetPhase(int& phase)
{
    float offset = _freq * (Glob.time - _start);
    while (_current < _items.Size() && _items[_current].time < offset)
    {
        _current++;
    }
    if (_current > _items.Size())
    {
        return false;
    }
    if (_current == 0)
    {
        phase = -1;
    }
    else
    {
        phase = _items[_current - 1].phase;
    }
    return true;
}

float ManLipInfo::GetPhase()
{
    float offset = _freq * (Glob.time - _start);

    float floor = fastFloor(offset * _invFrame) * _frame;

    while (_current < _items.Size() && _items[_current].time < floor)
    {
        _current++;
    }
    if (_current > _items.Size())
    {
        return -1;
    }

    float oldPhase = 0;
    if (_current > 0)
    {
        oldPhase = _items[_current - 1].phase;
    }

    int c = _current;
    while (c < _items.Size() && _items[c].time < floor + _frame)
    {
        c++;
    }

    float newPhase = 0;
    if (c > 0 && c < _items.Size())
    {
        newPhase = _items[c - 1].phase;
    }

    float dif = offset - floor;
    return (1.0 / 7) * _invFrame * (dif * newPhase + (_frame - dif) * oldPhase);
}

HeadType::HeadType() = default;

void HeadType::Load(const ParamEntry& cfg)
{
    RStringB microName = cfg >> "microMimics";
    const ParamEntry& micro = Pars >> "CfgMimics" >> microName;
    _lBrowRandom.Load(micro >> "lBrow");
    _mBrowRandom.Load(micro >> "mBrow");
    _rBrowRandom.Load(micro >> "rBrow");
    _lMouthRandom.Load(micro >> "lMouth");
    _mMouthRandom.Load(micro >> "mMouth");
    _rMouthRandom.Load(micro >> "rMouth");
}

void HeadType::InitShape(const ParamEntry& cfg, LODShape* shape)
{
    _personality.Init(shape, "osobnost", nullptr);
    _textureOrig = _personality.GetTexture(shape);

    _glasses.Init(shape, "brejle", nullptr);

    _lBrow.Init(shape, "loboci", nullptr);
    _mBrow.Init(shape, "soboci", nullptr);
    _rBrow.Init(shape, "poboci", nullptr);
    _lMouth.Init(shape, "lkoutek", nullptr);
    _mMouth.Init(shape, "skoutek", nullptr);
    _rMouth.Init(shape, "pkoutek", nullptr);

    _eyelid.Init(shape, "vicka", nullptr);
    _lip.Init(shape, "spodni ret", nullptr);
}

Head::Head(const HeadType& type, LODShape* lShape)
{
    // Simulate interpolates these even before random speech is enabled.
    _actualRandomLip = 0.0f;
    _wantedRandomLip = 0.0f;
    _speedRandomLip = 0.0f;
    _winkPhase = 2;
    _forceWinkPhase = -1;
    _nextWink = Glob.time; // recalculate now

    _mimicMode = nullptr;

    _mimicPhase = 0;
    _nextMimicTime = 1e10;

    _lBrow = VZero;
    _mBrow = VZero;
    _rBrow = VZero;
    _lMouth = VZero;
    _mMouth = VZero;
    _rMouth = VZero;
    _lBrowOld = VZero;
    _mBrowOld = VZero;
    _rBrowOld = VZero;
    _lMouthOld = VZero;
    _mMouthOld = VZero;
    _rMouthOld = VZero;

    _rBrowRandom.SetWanted(VZero, 0);
    _mBrowRandom.SetWanted(VZero, 0);
    _lBrowRandom.SetWanted(VZero, 0);

    _rMouthRandom.SetWanted(VZero, 0);
    _mMouthRandom.SetWanted(VZero, 0);
    _lMouthRandom.SetWanted(VZero, 0);

    SetFace(type, true, lShape, "Default");
    SetGlasses(type, lShape, "None");
    SetMimic("Default");

    _randomLip = false;
}

void Head::AttachWave(IWave* wave, float freq)
{
    _lipInfo = new ManLipInfo();
    if (!_lipInfo->AttachWave(wave, freq))
    {
        _lipInfo = nullptr;
    }
}

void Head::NextRandomLip()
{
    _wantedRandomLip = GRandGen.RandomValue();
    float time = 0.05 + 0.1 * GRandGen.RandomValue();
    _nextChangeRandomLip = Glob.time + time;
    _speedRandomLip = 1.0 / time;
}

void Head::SetRandomLip(bool set)
{
    if (set && !_randomLip)
    {
        _actualRandomLip = 0;
        NextRandomLip();
    }
    _randomLip = set;
}

// FACE-001 kill door: POSEIDON_FACE_GAIN scales every face offset. At 30 the deformation
// is unmissable, which separates "the offsets never reach the screen" from "they reach it
// and are too small to measure" without any argument about magnitudes.
static float FaceOffsetGain()
{
    static const float g = []
    {
        const char* e = getenv("POSEIDON_FACE_GAIN");
        return e ? static_cast<float>(atof(e)) : 1.0f;
    }();
    return g;
}

namespace Poseidon
{
bool FaceBoneRigEnabled()
{
    static const bool on = []
    {
        const char* e = getenv("POSEIDON_FACE_BONES");
        return !(e && atoi(e) == 0);
    }();
    return on;
}

// The eight face selections, in FaceGroup order. These are the same asset names
// HeadType::InitShape binds; they are looked up here by name so the rig does not depend on
// HeadType having been initialised first (MovesType and ManType are built independently).
static const char* const GFaceSelNames[NFaceGroups] = {
    "vicka",       // FaceEyelid
    "spodni ret",  // FaceLip
    "lkoutek",     // FaceLMouth
    "skoutek",     // FaceMMouth
    "pkoutek",     // FaceRMouth
    "loboci",      // FaceLBrow
    "soboci",      // FaceMBrow
    "poboci",      // FaceRBrow
};

// Synthetic bone names. They deliberately do NOT match any named selection, so
// Skeleton::Prepare's FindNamedSel lookup skips them and only ReplaceSelection below binds
// anything to them.
static const char* const GFaceBoneNames[NFaceGroups] = {
    "%face_eyelid", "%face_lip", "%face_lmouth", "%face_mmouth",
    "%face_rmouth", "%face_lbrow", "%face_mbrow", "%face_rbrow",
};

void SetupFaceBoneRig(Skeleton* skeleton, LODShapeWithShadow* lShape, WeightInfo& weights)
{
    if (!FaceBoneRigEnabled() || !skeleton || !lShape)
    {
        return;
    }
    // MovesTypes with different configs can share the same cached WeightInfo. Rebinding
    // an existing rig finds only already-claimed vertices and would disable its palette
    // metadata while leaving those vertices bound to the synthetic bones.
    if (weights._faceBoneBase >= 0)
    {
        return;
    }
    // Reserve the bones. NewBone is a find-or-append, so this is idempotent, and because it
    // appends, any bone a later-loading RTM introduces lands ABOVE the face bones -- the
    // indices captured by the one-time SetSkinData upload never move under us.
    const int base = skeleton->NewBone(RStringB(GFaceBoneNames[0]));
    for (int g = 1; g < NFaceGroups; g++)
    {
        const int b = skeleton->NewBone(RStringB(GFaceBoneNames[g]));
        if (b != base + g)
        {
            LOG_WARN(World, "FACE-002: face bones are not contiguous ({} != {}); rig disabled",
                     b, base + g);
            weights._faceBoneBase = -1;
            return;
        }
    }
    // The palette is 128 entries and the last is reserved for unweighted vertices, so a
    // skeleton that has already grown past that has no room for the rig.
    if (base + NFaceGroups > 127)
    {
        LOG_WARN(World, "FACE-002: no palette room for the face rig (base {}); disabled", base);
        weights._faceBoneBase = -1;
        return;
    }

    weights._faceBoneBase = base;
    int bound = 0;
    for (int level = 0; level < lShape->NLevels() && level < MAX_LOD_LEVELS; level++)
    {
        Shape* shape = lShape->Level(level);
        // Only graphical levels: memory / geometry / fire levels are posed through code
        // paths that do not run ApplyFaceBones, and a face bone left at identity there
        // would tear the face off the head.
        if (!shape || !lShape->IsNormalLevel(level))
        {
            continue;
        }
        for (int g = 0; g < NFaceGroups; g++)
        {
            const int selIndex = shape->FindNamedSel(RStringB(GFaceSelNames[g]));
            if (selIndex < 0)
            {
                continue;
            }
            const int parent =
                weights[level].ReplaceSelection(shape, selIndex, base + g, base, base + NFaceGroups);
            if (parent >= 0)
            {
                weights._faceParent[level][g] = static_cast<signed char>(parent);
                bound++;
            }
        }
        weights[level].Normalize();
    }
    if (bound <= 0)
    {
        weights._faceBoneBase = -1; // no face selections in this model at all
    }
    LOG_INFO(World, "FACE-002: face bone rig base={} groups bound={} on '{}'", base, bound,
             (const char*)lShape->GetName());
}
} // namespace Poseidon

bool Head::ApplyFaceBones(const HeadType& type, const WeightInfo& weights, int level,
                          Matrix4Array& matrices, bool isDead)
{
    return ApplyFaceBonesImpl(type, weights, level, matrices, isDead, true);
}

bool Head::EvaluateDeadFaceBones(const HeadType& type, const WeightInfo& weights, int level,
                                 Matrix4Array& matrices, bool isDead) const
{
    if (!isDead) return false;
    // The dead/read-only branch neither samples lips nor writes this head or
    // counters. Do not copy Head: its legacy SRef lip pointer transfers on copy.
    return const_cast<Head*>(this)->ApplyFaceBonesImpl(type, weights, level, matrices, true, false);
}

bool Head::ApplyFaceBonesImpl(const HeadType& /*type*/, const WeightInfo& weights, int level, Matrix4Array& matrices,
                          bool isDead, bool recordState)
{
    if (!recordState && !isDead) return false;
    if (recordState) _faceBonesActive = false;
    const int base = weights._faceBoneBase;
    if (base < 0 || level < 0 || level >= MAX_LOD_LEVELS)
    {
        return false;
    }
    if (base + NFaceGroups > matrices.Size())
    {
        return false;
    }

    // The CPU route offset along `up = trans * VUp`, where `trans` was the head transform in
    // model space. The parent bone's own orientation IS that frame, already blended and
    // already carrying CombineTransform's head turn, so it is read from there rather than
    // recomputed.
    int anyParent = -1;
    for (int g = 0; g < NFaceGroups; g++)
    {
        const int p = weights._faceParent[level][g];
        if (p >= 0 && p < matrices.Size())
        {
            anyParent = p;
            break;
        }
    }
    if (anyParent < 0)
    {
        return false;
    }
    Matrix3 orient = matrices[anyParent].Orientation();
    orient.Orthogonalize();
    const Vector3 up = orient * VUp;

    Vector3 delta[NFaceGroups];
    for (int g = 0; g < NFaceGroups; g++)
    {
        delta[g] = VZero;
    }

    // ---- exactly the offsets Head::Animate would have written into the vertices ----
    if (!isDead)
    {
        float winkPhase = (_forceWinkPhase >= 0) ? 2 * _forceWinkPhase : 2.0f * _winkPhase;
        if (winkPhase > 1)
        {
            winkPhase = 2.0f - winkPhase;
        }
        delta[FaceEyelid] += winkPhase * -0.015f * up;

        if (_lipInfo)
        {
            const float lipPhase = _lipInfo->GetPhase();
            if (lipPhase > -0.1f)
            {
                delta[FaceLip] += lipPhase * -0.02f * up;
                delta[FaceLMouth] += lipPhase * -0.01f * up;
                delta[FaceRMouth] += lipPhase * -0.01f * up;
            }
            else
            {
                if (_lipInfo->CurrentCursor() < _lipInfo->PhonemeCount())
                {
                    LOG_WARN(Audio,
                             "Lip: terminated mid-stream at offset={:.3f}s, lipPhase={:.4f}, cursor={}/{} -- "
                             "character will stop lip animation until next say",
                             _lipInfo->ElapsedOffset(), lipPhase, _lipInfo->CurrentCursor(),
                             _lipInfo->PhonemeCount());
                }
                _lipInfo = nullptr;
            }
        }
        else if (_randomLip)
        {
            delta[FaceLip] += _actualRandomLip * -0.02f * up;
            delta[FaceLMouth] += _actualRandomLip * -0.01f * up;
            delta[FaceRMouth] += _actualRandomLip * -0.01f * up;
        }
    }

    const float coefNew = _mimicPhase;
    const float coefOld = 1.0f - _mimicPhase;
    const float gain = FaceOffsetGain();
#define FACE_MIMIC(g, x) \
    delta[g] += orient * (coefOld * x##Old + coefNew * x + (Vector3)x##Random)
    FACE_MIMIC(FaceRBrow, _rBrow);
    FACE_MIMIC(FaceMBrow, _mBrow);
    FACE_MIMIC(FaceLBrow, _lBrow);
    FACE_MIMIC(FaceRMouth, _rMouth);
    FACE_MIMIC(FaceMMouth, _mMouth);
    FACE_MIMIC(FaceLMouth, _lMouth);
#undef FACE_MIMIC

    for (int g = 0; g < NFaceGroups; g++)
    {
        const int p = weights._faceParent[level][g];
        if (p < 0 || p >= matrices.Size())
        {
            continue;
        }
        Matrix4 m = matrices[p];
        m.SetPosition(m.Position() + delta[g] * gain);
        matrices[base + g] = m;
        if (recordState && delta[g].SquareSize() > 1e-12f)
        {
            GFaceDiag().offsets++;
        }
    }
    if (recordState) _faceBonesActive = true;
    return true;
}

static void OffsetAnimation(const Animation& anim, Shape* shape, int level, Vector3Par offsetIn)
{
    int selection = anim.GetSelection(level);
    if (selection < 0)
    {
        return;
    }
    const Vector3 offset = offsetIn * FaceOffsetGain();
    const NamedSelection& sel = shape->NamedSel(selection);
    for (int i = 0; i < sel.Size(); i++)
    {
        int posI = sel[i];
        shape->SetPos(posI) += offset;
    }
    // FACE-001: InvalidateNormals alone is not enough. `bufferDirty` (InvalidateBuffer) is
    // the backend's ONLY change signal for a vertex buffer -- VertexBufferWgpu::Update
    // returns in O(1) on a clean buffer -- so the face offsets were written into vertices
    // that were never re-uploaded.
    shape->InvalidateBuffer();
    shape->InvalidateNormals();
    if (sel.Size() > 0 && offset.SquareSize() > 1e-12f)
    {
        GFaceDiag().offsets++;
    }
}

#define OFFSET_ANIM(x) \
    OffsetAnimation(type.x, shape, level, trans*(coefOld * x##Old + coefNew * x + coefRand * (Vector3)x##Random));

void Head::Animate(const HeadType& type, LODShape* lShape, int level, bool isDead, Matrix3Par trans, bool hiddenHead)
{
    const_cast<Matrix3&>(trans).Orthogonalize();

    Shape* shape = lShape->Level(level);

    // face
    if (_texture)
    {
        type._personality.SetTexture(lShape, level, _texture);
    }

    // glasses
    if (_glasses && !hiddenHead)
    {
        type._glasses.Unhide(lShape, level);
        type._glasses.SetTexture(lShape, level, _glasses);
    }
    else
    {
        type._glasses.Hide(lShape, level);
    }

    // FACE-002: the pose is already in the bone palette this frame, and it is per-object
    // there. Writing it into the shared vertices as well would double it on GL33 and, on
    // wgpu, would be thrown away by the next unit's bind-pose restore (FACE-001).
    if (_faceBonesActive)
    {
        return;
    }

    Vector3 up = trans * VUp;

    if (!isDead)
    {
        // wink
        float winkPhase = 0;
        if (_forceWinkPhase >= 0)
        {
            winkPhase = 2 * _forceWinkPhase;
        }
        else
        {
            winkPhase = 2.0 * _winkPhase;
        }
        if (winkPhase > 1)
        {
            winkPhase = 2.0 - winkPhase;
        }
        if (FaceDebugEnabled())
        {
            const int lipSel = type._lip.GetSelection(level);
            GFaceDiag().lipSelSize = lipSel >= 0 ? shape->NamedSel(lipSel).Size() : -1;
            const int eyeSel = type._eyelid.GetSelection(level);
            GFaceDiag().eyeSelSize = eyeSel >= 0 ? shape->NamedSel(eyeSel).Size() : -1;
            const int lmSel = type._lMouth.GetSelection(level);
            GFaceDiag().lmSelSize = lmSel >= 0 ? shape->NamedSel(lmSel).Size() : -1;
            const int rmSel = type._rMouth.GetSelection(level);
            GFaceDiag().rmSelSize = rmSel >= 0 ? shape->NamedSel(rmSel).Size() : -1;
        }
        OffsetAnimation(type._eyelid, shape, level, winkPhase * -0.015 * up);

        // lip
        if (_lipInfo)
        {
            float lipPhase = _lipInfo->GetPhase();
            if (lipPhase > -0.1)
            {
                OffsetAnimation(type._lip, shape, level, lipPhase * -0.02 * up);
                OffsetAnimation(type._lMouth, shape, level, lipPhase * -0.01 * up);
                OffsetAnimation(type._rMouth, shape, level, lipPhase * -0.01 * up);
            }
            else
            {
                // Natural end-of-utterance: lipPhase dips negative as the
                // terminator entry's -1 phase interpolates in.  That's
                // expected and silent.  Only log when the cursor stopped
                // short of the terminator — that's the bug shape (lip
                // animation gave up mid-stream, freezing the character's
                // mouth for the rest of the audio).
                if (_lipInfo->CurrentCursor() < _lipInfo->PhonemeCount())
                {
                    LOG_WARN(Audio,
                             "Lip: terminated mid-stream at offset={:.3f}s, lipPhase={:.4f}, cursor={}/{} -- "
                             "character will stop lip animation until next say",
                             _lipInfo->ElapsedOffset(), lipPhase, _lipInfo->CurrentCursor(), _lipInfo->PhonemeCount());
                }
                _lipInfo = nullptr;
            }
        }
        else if (_randomLip)
        {
            OffsetAnimation(type._lip, shape, level, _actualRandomLip * -0.02 * up);
            OffsetAnimation(type._lMouth, shape, level, _actualRandomLip * -0.01 * up);
            OffsetAnimation(type._rMouth, shape, level, _actualRandomLip * -0.01 * up);
        }
    }

    // mimic
    float coefNew = _mimicPhase;
    float coefOld = 1.0f - _mimicPhase;
    const float coefRand = +1.0f;

    OFFSET_ANIM(_rBrow);
    OFFSET_ANIM(_mBrow);
    OFFSET_ANIM(_lBrow);
    OFFSET_ANIM(_rMouth);
    OFFSET_ANIM(_mMouth);
    OFFSET_ANIM(_lMouth);
}

void Head::Deanimate(const HeadType& type, LODShape* lShape, int level, bool isDead, Matrix3Par trans, bool hiddenHead)
{
    // FACE-002: nothing was written into the shared vertices, so there is nothing to undo.
    if (_faceBonesActive)
    {
        return;
    }

    const_cast<Matrix3&>(trans).Orthogonalize();

    Shape* shape = lShape->Level(level);

    Vector3 up = trans * VUp;

    // mimic
    float coefNew = -_mimicPhase;
    float coefOld = -(1.0f - _mimicPhase);
    const float coefRand = -1;
    OFFSET_ANIM(_rBrow);
    OFFSET_ANIM(_mBrow);
    OFFSET_ANIM(_lBrow);
    OFFSET_ANIM(_rMouth);
    OFFSET_ANIM(_mMouth);
    OFFSET_ANIM(_lMouth);

    // wink
    if (!isDead)
    {
        float winkPhase = 0;
        if (_forceWinkPhase >= 0)
        {
            winkPhase = 2 * _forceWinkPhase;
        }
        else
        {
            winkPhase = 2.0 * _winkPhase;
        }
        if (winkPhase > 1)
        {
            winkPhase = 2.0 - winkPhase;
        }
        OffsetAnimation(type._eyelid, shape, level, winkPhase * 0.015 * up);
    }

    // lip
    if (!isDead && _lipInfo)
    {
        float lipPhase = _lipInfo->GetPhase();
        if (lipPhase > -0.1)
        {
            OffsetAnimation(type._lip, shape, level, lipPhase * 0.02 * up);
            OffsetAnimation(type._lMouth, shape, level, lipPhase * 0.01 * up);
            OffsetAnimation(type._rMouth, shape, level, lipPhase * 0.01 * up);
        }
    }
}

#undef OFFSET_ANIM

void RandomVector3Type::Load(const ParamEntry& cfg)
{
    rng = Vector3(cfg[0], cfg[1], cfg[2]);
    minT = cfg[3];
    maxT = cfg[4];
}

RandomVector3::RandomVector3()
{
    _spd = VZero;
    _cur = VZero;
    _timeToWanted = 0;
}

void RandomVector3::SetWanted(Vector3Par wanted, float time)
{
    if (time <= 0)
    {
        _spd = VZero;
        _cur = wanted;
        _timeToWanted = 0;
    }
    else
    {
        _spd = (wanted - _cur) / time;
        _timeToWanted = time;
    }
}

bool RandomVector3::Simulate(float deltaT)
{
    if (_timeToWanted <= 0)
    {
        return true;
    }
    float dt = deltaT;
    saturateMin(dt, _timeToWanted);
    _timeToWanted -= deltaT;
    _cur += _spd * dt;
    return false;
}

void RandomVector3::SetRandomTgt(Vector3Par rng, float minT, float maxT)
{
    float x = GRandGen.RandomValue() * rng.X() * 2 - rng.X();
    float y = GRandGen.RandomValue() * rng.Y() * 2 - rng.Y();
    float z = GRandGen.RandomValue() * rng.Z() * 2 - rng.Z();
    float t = GRandGen.RandomValue() * (maxT - minT) + minT;
    SetWanted(Vector3(x, y, z), t);
}

bool RandomVector3::SimulateAndSetRandomTgt(float deltaT, Vector3Par rng, float minT, float maxT)
{
    if (Simulate(deltaT))
    {
        SetRandomTgt(rng, minT, maxT);
        return true;
    }
    return false;
}

bool RandomVector3::SimulateAndSetRandomTgt(float deltaT, const RandomVector3Type& type)
{
    if (Simulate(deltaT))
    {
        SetRandomTgt(type.rng, type.minT, type.maxT);
        return true;
    }
    return false;
}

const float MimicPeriod = 5;

void Head::Simulate(const HeadType& type, float deltaT, SimulationImportance prec, bool dead)
{
    const float winkSpeed = 8.0;
    if (Glob.time >= _nextWink)
    {
        _winkPhase += winkSpeed * deltaT;
        if (_winkPhase >= 1)
        {
            _winkPhase = 0;
            _nextWink = Glob.time + 1.0f + 5.0f * GRandGen.RandomValue();
        }
    }

    const float mimicSpeed = 2.0;
    _mimicPhase += mimicSpeed * deltaT;
    saturateMin(_mimicPhase, 1.0f);

    if (_mimicPhase >= 1)
    {
        // change mimic when in some mimic mode
        if (_forceMimic.GetLength() > 0)
        {
        }
        else if (_mimicMode)
        {
            _nextMimicTime -= deltaT;
            if (_nextMimicTime < 0)
            {
                // select random mimic
                float rand = GRandGen.RandomValue();
                RStringB name = (*_mimicMode)[_mimicMode->GetSize() - 1];
                for (int i = 0; i < _mimicMode->GetSize() - 1; i += 2)
                {
                    float prop = (*_mimicMode)[i + 1];
                    rand -= prop;
                    if (rand <= 0)
                    {
                        name = (*_mimicMode)[i];
                        break;
                    }
                }
                SetMimic(name);
                _nextMimicTime = MimicPeriod;
            }
        }
    }

    float diff = _wantedRandomLip - _actualRandomLip;
    saturate(diff, -_speedRandomLip * deltaT, _speedRandomLip * deltaT);
    _actualRandomLip += diff;
    if (Glob.time >= _nextChangeRandomLip)
    {
        NextRandomLip();
    }

    if (prec <= SimulateVisibleNear)
    {
        if (!dead)
        {
            _lBrowRandom.SimulateAndSetRandomTgt(deltaT, type._lBrowRandom);
            _mBrowRandom.SimulateAndSetRandomTgt(deltaT, type._mBrowRandom);
            _rBrowRandom.SimulateAndSetRandomTgt(deltaT, type._rBrowRandom);
            _lMouthRandom.SimulateAndSetRandomTgt(deltaT, type._lMouthRandom);
            _mMouthRandom.SimulateAndSetRandomTgt(deltaT, type._mMouthRandom);
            _rMouthRandom.SimulateAndSetRandomTgt(deltaT, type._rMouthRandom);
        }
        else
        {
            _lBrowRandom.SetWanted(VZero, 0);
            _mBrowRandom.SetWanted(VZero, 0);
            _rBrowRandom.SetWanted(VZero, 0);
            _lMouthRandom.SetWanted(VZero, 0);
            _mMouthRandom.SetWanted(VZero, 0);
            _rMouthRandom.SetWanted(VZero, 0);
        }
    }
}

void Head::SetFace(const HeadType& type, bool women, LODShape* lShape, RString name, RString player)
{
    const ParamEntry* cls = (Pars >> "CfgFaces").FindEntry(name);
    if (!cls)
    {
        cls = (Pars >> "CfgFaces").FindEntry("Default");
        if (!cls)
        {
            return;
        }
    }

    bool faceWoman = false;
    if (cls->FindEntry("woman"))
    {
        faceWoman = *cls >> "woman";
    }
    // do not use women face to man head and vice versa
    if (faceWoman != women)
    {
        return;
    }

    RString textName = GetPictureName(*cls >> "texture");
    Ref<Texture> text = GlobLoadTexture(textName);
    if (text)
    {
        _texture = text;
    }
    else
    {
        LOG_ERROR(Physics, "Cannot find texture: {}", (const char*)textName);
    }

    if (stricmp(name, "custom") == 0)
    {
        if (player.GetLength() > 0)
        {
            RString relativeFace = Poseidon::BuildNetworkPlayerAssetTmpPath(player, RString("face.paa"));
            RString face = relativeFace.GetLength() > 0 ? Poseidon::GetUserDirectory() + relativeFace : RString();
            if (face.GetLength() > 0 && Poseidon::FileExistsUtf8(face))
            {
                Ref<Texture> text = GlobLoadTexture(face);
                if (text)
                {
                    _texture = text;
                }
            }
            else
            {
                relativeFace = Poseidon::BuildNetworkPlayerAssetTmpPath(player, RString("face.jpg"));
                face = relativeFace.GetLength() > 0 ? Poseidon::GetUserDirectory() + relativeFace : RString();
                if (face.GetLength() > 0 && Poseidon::FileExistsUtf8(face))
                {
                    Ref<Texture> text = GlobLoadTexture(face);
                    if (text)
                    {
                        _texture = text;
                    }
                }
            }
        }
        else
        {
            RString face = Poseidon::GetUserDirectory() + RString("face.paa");
            if (Poseidon::FileExistsUtf8(face))
            {
                Ref<Texture> text = GlobLoadTexture(face);
                if (text)
                {
                    _texture = text;
                }
            }
            else
            {
                face = Poseidon::GetUserDirectory() + RString("face.jpg");
                if (Poseidon::FileExistsUtf8(face))
                {
                    Ref<Texture> text = GlobLoadTexture(face);
                    if (text)
                    {
                        _texture = text;
                    }
                }
            }
        }
    }

    // check if there is some wounded texture associated
    if (_texture)
    {
        const ParamEntry& cfg = Pars >> "CfgFaceWounds" >> "wounds";
        for (int i = 0; i < cfg.GetSize() - 1; i += 2)
        {
            RStringB origName = cfg[i];
            RStringB woundName = cfg[i + 1];
            RStringB origTexName = GetDefaultName(origName, "data\\", ".pac");
            if (strcmp(origTexName, _texture->GetName()))
            {
                continue;
            }
            RStringB woundTexName = GetDefaultName(woundName, "data\\", ".pac");
            _textureWounded = GlobLoadTexture(woundTexName);
        }

        lShape->RegisterTexture(_texture, type._personality);
    }
    if (_textureWounded)
    {
        lShape->RegisterTexture(_textureWounded, type._personality);
    }
}

RString Head::GetFaceTextureName() const
{
    return _texture ? RString(_texture->GetName()) : RString();
}

void Head::SetGlasses(const HeadType& type, LODShape* lShape, RString name)
{
    const ParamEntry* cls = (Pars >> "CfgGlasses").FindEntry(name);
    if (!cls)
    {
        _glasses = nullptr;
        return;
    }

    RString textName = GetPictureName(*cls >> "texture");
    if (textName.GetLength() == 0)
    {
        _glasses = nullptr;
        return;
    }

    Ref<Texture> text = GlobLoadTexture(textName);
    if (text)
    {
        _glasses = text;
    }
    else
    {
        WarningMessage("Cannot find texture: %s", (const char*)textName);
    }

    if (_glasses)
    {
        lShape->RegisterTexture(_glasses, type._glasses);
    }
}

void Head::SetMimicMode(RStringB modeName)
{
    if (modeName.GetLength() > 0)
    {
        const ParamEntry& par = Pars >> "CfgMimics" >> modeName;
        if (_mimicMode != &par)
        {
            _mimicMode = &par;
            _nextMimicTime = 0;
        }
    }
    else
    {
        _mimicMode = nullptr;
    }
}

RStringB Head::GetMimicMode() const
{
    if (!_mimicMode)
    {
        return Poseidon::Foundation::RStringBEmpty;
    }
    return _mimicMode->GetName();
}

void Head::SetForceMimic(RStringB name)
{
    _forceMimic = name;
    SetMimic(name);
}

void Head::SetMimic(RStringB name)
{
    const ParamEntry* cls = (Pars >> "CfgMimics" >> "States").FindEntry(name);
    if (!cls)
    {
        // WarningMessage("Unknown mimic: %s", (const char *)name);
        LOG_ERROR(Physics, "Unknown mimic: {}", (const char*)name);
        return;
    }

    float coefNew = _mimicPhase;
    float coefOld = 1.0f - _mimicPhase;
    _lBrowOld = coefOld * _lBrowOld + coefNew * _lBrow;
    _mBrowOld = coefOld * _mBrowOld + coefNew * _mBrow;
    _rBrowOld = coefOld * _rBrowOld + coefNew * _rBrow;
    _lMouthOld = coefOld * _lMouthOld + coefNew * _lMouth;
    _mMouthOld = coefOld * _mMouthOld + coefNew * _mMouth;
    _rMouthOld = coefOld * _rMouthOld + coefNew * _rMouth;
    _mimicPhase = 0;

    const float coef = 0.01;
    _lBrow[0] = (*cls >> "lBrow")[0];
    _lBrow[0] *= coef;
    _lBrow[1] = (*cls >> "lBrow")[1];
    _lBrow[1] *= coef;
    _mBrow[0] = (*cls >> "mBrow")[0];
    _mBrow[0] *= coef;
    _mBrow[1] = (*cls >> "mBrow")[1];
    _mBrow[1] *= coef;
    _rBrow[0] = (*cls >> "rBrow")[0];
    _rBrow[0] *= coef;
    _rBrow[1] = (*cls >> "rBrow")[1];
    _rBrow[1] *= coef;
    _lMouth[0] = (*cls >> "lMouth")[0];
    _lMouth[0] *= coef;
    _lMouth[1] = (*cls >> "lMouth")[1];
    _lMouth[1] *= coef;
    _mMouth[0] = (*cls >> "mMouth")[0];
    _mMouth[0] *= coef;
    _mMouth[1] = (*cls >> "mMouth")[1];
    _mMouth[1] *= coef;
    _rMouth[0] = (*cls >> "rMouth")[0];
    _rMouth[0] *= coef;
    _rMouth[1] = (*cls >> "rMouth")[1];
    _rMouth[1] *= coef;
}
