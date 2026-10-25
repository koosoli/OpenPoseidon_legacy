// The upper-body action layer (owner, 2026-10-20: "mag reload while walking"). CWA plays an action such as a reload as
// the man's whole move, so a reload stops him where he stands. This layer plays the action's move on the trunk, the
// arms and the weapons only, over whatever his legs are doing: the reload move's upper-body bones, taken relative to
// its pelvis, are put on the walking pelvis, and blended in and out. The action's own finish function (MFReload:
// ProcessMoveFunction) runs when the move ends, so the magazine logic is the engine's, unchanged.
//
// Everything that reads the pose goes through it: the drawn palette (PrepareBasePose), memory points (AnimatePoint:
// the eye, the muzzle) and proxies (AnimateMatrix: the rifle in his hands). While it runs the weapons are disabled and
// IsActionInProgress reports the action, as for the full-body move.
//
// Used for the player's reload while he moves, standing or crouched (KeyboardPilot); standing still, and prone, the
// full-body reload plays as before. The layer is local: other machines in multiplayer see the reload finish, not the
// arms move.

#include <Poseidon/World/Entities/Infantry/SoldierOldCommon.hpp>

#include <algorithm>
#include <vector>

namespace Poseidon
{

namespace
{
constexpr float UpperBlendIn = 0.15f;  // s
constexpr float UpperBlendOut = 0.2f;  // s
// The bones the layer takes from the action: everything above the pelvis. The pelvis anchors it.
const char* const UpperBones[] = {"bricho", "zebra",  "hrudnik", "krk",    "hlava",  "prameno", "lrameno", "pbiceps",
                                  "lbiceps", "ploket", "lloket",  "pruka",  "lruka",  "zbran",   "roura"};
constexpr const char* AnchorBone = "pzadek";
// The landing layer: the hips and legs go toward the absorb pose and back over LandTime (s), the upper body riding on
// the hips; LandPeak* say how far, from a gentle step (a hop) to a deeper give (a ~4 m/s touchdown). Halved after the
// owner's smoke test (2026-10-20: "the feathering with the knees is a bit strong"; was 0.3..0.7).
constexpr float LandTime = 0.46f;
constexpr float LandRise = 0.06f;
constexpr float LandPeakMin = 0.15f;
constexpr float LandPeakMax = 0.4f;
const char* const LowerBones[] = {"pzadek", "lzadek", "pstehno", "lstehno", "pholen", "lholen", "pchodidlo",
                                  "lchodidlo", "pprsty", "lprsty"};

// An Arma skeleton (OFP2_ManSkeleton: Pelvis, Spine..Spine3, LeftUpLeg ...) has none of the
// Czech names above, so with Arma 1's moves the walking reload played on no bone at all
// (owner, 2026-10-25). Its pelvis is the anchor, its legs are the bones named so, and every
// other bone is upper body -- Arma 1's own BlendAnims split the man the same way.
bool ArmaLegBone(const char* name)
{
    std::string n = name ? name : "";
    for (char& c : n)
        c = (char)tolower((unsigned char)c);
    return n.find("leg") != std::string::npos || n.find("foot") != std::string::npos ||
           n.find("toe") != std::string::npos;
}

int AnchorIndex(const Skeleton* skeleton)
{
    const int i = skeleton->FindBone(AnchorBone);
    return i >= 0 ? i : skeleton->FindBone("pelvis");
}

// the bones the layer takes: the Czech list, else (an Arma skeleton) all but the pelvis and legs
template <class F> void ForUpperBones(const Skeleton* skeleton, F&& f)
{
    if (skeleton->FindBone(AnchorBone) >= 0)
    {
        for (const char* name : UpperBones)
            if (const int i = skeleton->FindBone(name); i >= 0)
                f(i);
        return;
    }
    const int anchor = AnchorIndex(skeleton);
    for (int i = 0; i < skeleton->NBones(); i++)
        if (i != anchor && !ArmaLegBone(skeleton->GetBone(i)))
            f(i);
}

template <class F> void ForLowerBones(const Skeleton* skeleton, F&& f)
{
    if (skeleton->FindBone(AnchorBone) >= 0)
    {
        for (const char* name : LowerBones)
            if (const int i = skeleton->FindBone(name); i >= 0)
                f(i);
        return;
    }
    const int anchor = AnchorIndex(skeleton);
    for (int i = 0; i < skeleton->NBones(); i++)
        if (i == anchor || ArmaLegBone(skeleton->GetBone(i)))
            f(i);
}
} // namespace

bool Man::StartUpperBodyAction(MoveId move, ActionContextBase* context)
{
    if (move == MoveIdNone || !Type()->GetAnimation(move) || _upperMove != MoveIdNone)
        return false;
    _upperMove = move;
    _upperTime = 0;
    _upperFactor = 0;
    _upperContext = context;
    return true;
}

void Man::SimulateUpperBodyAction(float deltaT)
{
    if (_landAge >= 0)
    {
        _landAge += deltaT;
        if (_landAge > LandTime || _isDead)
            _landAge = -1;
    }
    if (_upperMove == MoveIdNone)
        return;
    // interrupted: dead, down, swimming, in a vehicle -- the action is abandoned, as a full-body move would be
    const AIUnit* unit = Brain();
    if (_isDead || IsDown() || IsSwimming() || _ladderBuilding || (unit && unit->GetVehicleIn()))
    {
        _upperMove = MoveIdNone;
        _upperContext = nullptr;
        _upperFactor = 0;
        return;
    }
    const MoveInfo* info = Type()->GetMoveInfo(_upperMove);
    const float speed = info ? info->GetSpeed() : 1.0f;
    if (_upperTime < 1.0f)
    {
        _upperTime = std::min(1.0f, _upperTime + speed * deltaT);
        _upperFactor = std::min(1.0f, _upperFactor + deltaT / UpperBlendIn);
        if (_upperTime >= 1.0f && _upperContext)
        {
            Ref<ActionContextBase> context = _upperContext;
            _upperContext = nullptr;
            ProcessMoveFunction(context);
        }
        return;
    }
    _upperFactor = std::max(0.0f, _upperFactor - deltaT / UpperBlendOut);
    if (_upperFactor <= 0)
        _upperMove = MoveIdNone;
}

void Man::ApplyUpperBodyLayer(Matrix4Array& matrices) const
{
    if (!UpperBodyLayerActive())
        return;
    AnimationRT* anim = Type()->GetAnimation(_upperMove);
    const Skeleton* skeleton = Type()->GetSkeleton();
    if (!anim || !skeleton)
        return;
    const int anchor = AnchorIndex(skeleton);
    if (anchor < 0 || anchor >= matrices.Size())
        return;
    MATRIX_4_ARRAY(action, 128);
    anim->PrepareMatrices(action, std::min(_upperTime, 0.999f), 1.0f);
    if (anchor >= action.Size())
        return;
    // the action's upper body relative to its pelvis, put on this pose's pelvis
    const Matrix4 onto = matrices[anchor] * Matrix4(MInverseGeneral, action[anchor]);
    const float f = _upperFactor;
    ForUpperBones(skeleton, [&](int i) {
        if (i >= matrices.Size() || i >= action.Size())
            return;
        Matrix4 layered = onto * action[i];
        Matrix4 out;
        out.InlineSetMultiply(matrices[i], 1 - f);
        out.InlineAddMultiply(layered, f);
        matrices[i] = out;
    });
}

void Man::StartLandingLayer(float touchdownSpeed)
{
    InitSwimMoves();
    if (_jumpLandMove == MoveIdNone || !Type()->GetAnimation(_jumpLandMove))
        return;
    _landAge = 0;
    _landPeak = std::clamp(LandPeakMin + touchdownSpeed * 0.1f, LandPeakMin, LandPeakMax);
}

float Man::LandingWeight() const
{
    if (_landAge < 0)
        return 0;
    float w = _landAge < LandRise ? _landAge / LandRise : 1 - (_landAge - LandRise) / (LandTime - LandRise);
    w = std::clamp(w, 0.0f, 1.0f);
    return _landPeak * w * w * (3 - 2 * w); // smoothstep: no kink at either end
}

void Man::ApplyLandingLayer(Matrix4Array& matrices) const
{
    const float w = LandingWeight();
    if (w <= 0.001f || _jumpLandMove == MoveIdNone)
        return;
    AnimationRT* anim = Type()->GetAnimation(_jumpLandMove);
    const Skeleton* skeleton = Type()->GetSkeleton();
    if (!anim || !skeleton)
        return;
    const int anchor = AnchorIndex(skeleton);
    if (anchor < 0 || anchor >= matrices.Size())
        return;
    MATRIX_4_ARRAY(land, 128);
    anim->PrepareMatrices(land, std::clamp(0.05f + 0.8f * _landAge / LandTime, 0.0f, 0.999f), 1.0f);
    if (anchor >= land.Size())
        return;
    const Matrix4 hipsBefore = matrices[anchor];
    std::vector<bool> lower(matrices.Size(), false);
    ForLowerBones(skeleton, [&](int i) {
        if (i >= matrices.Size() || i >= land.Size())
            return;
        Matrix4 out;
        out.InlineSetMultiply(matrices[i], 1 - w);
        out.InlineAddMultiply(land[i], w);
        matrices[i] = out;
        lower[i] = true;
    });
    // everything above the hips rides on them, as it was
    const Matrix4 follow = matrices[anchor] * Matrix4(MInverseGeneral, hipsBefore);
    for (int i = 0; i < matrices.Size(); i++)
    {
        if (!lower[i])
            matrices[i] = follow * matrices[i];
    }
}

const std::vector<Matrix4>* Man::UpperBodyPalette() const
{
    if (!PoseLayersActive() || UseCorpsePose())
        return nullptr;
    // one palette per pose: the points and proxies of a frame all read the same one
    const bool same = _upperPaletteValid && _upperPaletteKey[0] == _primaryTime && _upperPaletteKey[1] == _secondaryTime &&
                      _upperPaletteKey[2] == _upperTime && _upperPaletteKey[3] == _upperFactor &&
                      _upperPaletteKey[4] == _primaryFactor && _upperPaletteKey[5] == _landAge &&
                      _upperPaletteMoves[0] == _primaryMove.id &&
                      _upperPaletteMoves[1] == _secondaryMove.id;
    if (!same)
    {
        MATRIX_4_ARRAY(pose, 128);
        PrepareBasePose(pose);
        _upperPalette.assign(pose.Data(), pose.Data() + pose.Size());
        _upperPaletteKey[0] = _primaryTime;
        _upperPaletteKey[1] = _secondaryTime;
        _upperPaletteKey[2] = _upperTime;
        _upperPaletteKey[3] = _upperFactor;
        _upperPaletteKey[4] = _primaryFactor;
        _upperPaletteKey[5] = _landAge;
        _upperPaletteMoves[0] = _primaryMove.id;
        _upperPaletteMoves[1] = _secondaryMove.id;
        _upperPaletteValid = true;
    }
    return &_upperPalette;
}

bool Man::UpperBodyPoint(int level, int index, Vector3& res) const
{
    const std::vector<Matrix4>* palette = UpperBodyPalette();
    if (!palette || index < 0)
        return false;
    const AnimationRTWeight& wg = Type()->GetWeights()[level][index];
    if (wg.Size() <= 0)
        return false;
    Shape* shape = _shape->LevelOpaque(level);
    shape->SaveOriginalPos();
    const Vector3 pos = shape->OrigPos(index);
    Vector3 out = VZero;
    for (int w = 0; w < wg.Size(); w++)
    {
        const int i = wg[w].GetSel();
        if (i < 0 || i >= int(palette->size()))
            return false;
        out += (*palette)[i].FastTransform(pos) * wg[w].GetWeight();
    }
    res = out;
    return true;
}

bool Man::UpperBodyMatrix(Matrix4& mat, const AnimationRTWeight& wgt) const
{
    const std::vector<Matrix4>* palette = UpperBodyPalette();
    if (!palette || wgt.Size() <= 0)
        return false;
    Matrix4 res = MZero;
    for (int w = 0; w < wgt.Size(); w++)
    {
        const int i = wgt[w].GetSel();
        if (i < 0 || i >= int(palette->size()))
            return false;
        res += ((*palette)[i] * mat) * wgt[w].GetWeight();
    }
    res.Orthogonalize();
    mat = res;
    return true;
}

} // namespace Poseidon
