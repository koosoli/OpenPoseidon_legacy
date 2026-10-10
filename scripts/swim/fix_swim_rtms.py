# Sinkhole W3: repair the earlier Claude-made swim animations from the Malprave fork (swim.pbo).
# Usage: python fix_swim_rtms.py <extracted swim.pbo dir> <out dir>; then repack with
#   PoseidonTools pbo pack <out dir> swim.pbo --prefix swim
# Reads Malprave's RTM reader and the fitted CWA rest skeleton (G:\Malprave\tools\swimanim, read only).
# The swim animations are Claude-made and ship in content/default-packs/swim/rtm.
#
# 1. Legs (owner: "feet are backwards", three reports). Compared with the stock animations (DTA\Anim.pbo),
#    Malprave's thighs and shins are turned 158-180 deg about their own length: each bone's front (rest
#    +x, square to its axis) faces the body's back (-0.4..-1.0 against the pelvis's front; stock +0.4..+1.0)
#    while the joints sit where they should. Each phase, each thigh and shin is turned back about its own
#    axis (hip/knee/ankle unmoved) so its front faces where a hinge knee puts it: the thigh's away from the
#    folding shin, the shin's back up the thigh; a near-straight knee (KNEE_STRAIGHT) takes the pelvis's.
#    Feet, on the corrected shin: the foot (and the toes, rigidly) = the shin's rotation followed by a
#    twist-free swing about the rest ankle from the STOCK standing foot (no swing) toward POINTED
#    (POINT_DEG in front of the shin line), by how far the spine lies over: none up to UPRIGHT_DEG from
#    vertical, all of it from PRONE_DEG. (Two earlier foot-only repairs worked in the twisted shin's frame
#    and so still read as backwards.)
# 2. Shoulders (owner: "the neck is stretching unrealistically"). The shoulder bones are locked to the
#    chest (0 deg in every phase) while the upper arm turns 102-151 deg at the shoulder joint; stock
#    animations carry about a third of an arm movement in the shoulder bone (30-55 deg against the
#    chest, upper arm 38-98). Locked, a raised arm drags the collar and deltoid down and the neck looks
#    pulled long. Each phase: the shoulder takes SHRUG of the arm's swing against the chest (at most
#    SHRUG_MAX deg) about the chest-shoulder joint, and the arm (upper arm, forearm, hand) keeps its
#    orientation and moves with the new shoulder joint.
import sys, json, glob, os, numpy as np
sys.path.insert(0, r'G:\Malprave\tools\swimanim')
from rtm import read_rtm, write_rtm, mat4
J = json.load(open(r'G:\Malprave\tools\swimanim\joints.json'))
POINT_DEG = 20.0
UPRIGHT_DEG = 25.0
PRONE_DEG = 55.0
KNEE_STRAIGHT = 12.0
SHRUG = 0.35
SHRUG_MAX = 50.0
def unmat(M): return np.concatenate([M[:3,0], M[:3,1], M[:3,2], M[:3,3]])
def swing(v, u, maxdeg, share=1.0):
    v = v/np.linalg.norm(v); u = u/np.linalg.norm(u)
    ax = np.cross(v, u); s = np.linalg.norm(ax); c = v@u
    if s < 1e-8: return np.eye(3), 0.0
    ang = min(np.arctan2(s, c) * share, np.radians(maxdeg)); ax /= s
    K = np.array([[0,-ax[2],ax[1]],[ax[2],0,-ax[0]],[-ax[1],ax[0],0]])
    return np.eye(3) + np.sin(ang)*K + (1-np.cos(ang))*K@K, np.degrees(ang)
def about(R, p):  # 4x4: rotate R about rest point p
    M = np.eye(4); M[:3,:3] = R; M[:3,3] = p - R @ p; return M
def fix_feet(n, phases, angs):
    for side in 'lp':
        shin, foot, toe = n.index(side+'holen'), n.index(side+'chodidlo'), n.index(side+'prsty')
        a = np.array(J[side+'holen>'+side+'chodidlo']); k = np.array(J[side+'chodidlo>'+side+'prsty'])
        knee = np.array(J[side+'stehno>'+side+'holen'])
        d = (a - knee) / np.linalg.norm(a - knee)                 # the shin, knee -> ankle (rest frame)
        f0 = (k - a) / np.linalg.norm(k - a)                      # the rest foot (in front of the shin)
        e = f0 - (f0 @ d) * d; e /= np.linalg.norm(e)             # the shin's front, square to it
        u = np.cos(np.radians(POINT_DEG)) * d + np.sin(np.radians(POINT_DEG)) * e
        for t, mats in phases:
            S_ = mat4(mats[shin])
            Q, ang = swing(k - a, u, 180.0, prone_share(n, mats))
            angs.append(ang)
            F = S_ @ about(Q, a)
            mats[foot] = unmat(F); mats[toe] = unmat(F)
def axis_rotation(w, ang):
    K = np.array([[0,-w[2],w[1]],[w[2],0,-w[0]],[-w[1],w[0],0]])
    return np.eye(3) + np.sin(ang)*K + (1-np.cos(ang))*K@K
def perp(v, w):  # v's part square to unit w, normalised (zero when v is along w)
    p = v - (v @ w) * w; l = np.linalg.norm(p)
    return p / l if l > 1e-6 else p
def untwist(mats, b, p_rest, q_rest, front):
    """Turn bone b about its own axis (rest joint p -> q, both kept where they are) so its front -- the rest
    +x square to the axis -- faces `front`."""
    B = mat4(mats[b]); P = (B @ np.append(p_rest, 1))[:3]; Q_ = (B @ np.append(q_rest, 1))[:3]
    w = (Q_ - P) / np.linalg.norm(Q_ - P)
    cur = perp(B[:3,:3] @ perp(np.array([1.0, 0, 0]), (q_rest - p_rest) / np.linalg.norm(q_rest - p_rest)), w)
    want = perp(front, w)
    ang = np.arctan2(np.cross(cur, want) @ w, cur @ want)
    M = np.eye(4); R = axis_rotation(w, ang); M[:3,:3] = R; M[:3,3] = P - R @ P
    mats[b] = unmat(M @ B)
    return np.degrees(abs(ang))
def fix_legs(n, phases, angs):
    for side in 'lp':
        hip_b, th_b, sh_b = n.index(side+'zadek'), n.index(side+'stehno'), n.index(side+'holen')
        hip = np.array(J[side+'zadek>'+side+'stehno']); knee = np.array(J[side+'stehno>'+side+'holen'])
        ank = np.array(J[side+'holen>'+side+'chodidlo'])
        for t, mats in phases:
            H = mat4(mats[th_b]); S_ = mat4(mats[sh_b])
            hipW = (H @ np.append(hip, 1))[:3]; kneeW = (H @ np.append(knee, 1))[:3]; ankW = (S_ @ np.append(ank, 1))[:3]
            th = (kneeW - hipW) / np.linalg.norm(kneeW - hipW); sh = (ankW - kneeW) / np.linalg.norm(ankW - kneeW)
            body = mat4(mats[hip_b])[:3,:3] @ np.array([1.0, 0, 0])      # the pelvis's front
            bend = np.degrees(np.arccos(np.clip(th @ sh, -1, 1)))
            k = np.clip((bend - KNEE_STRAIGHT) / KNEE_STRAIGHT, 0.0, 1.0)  # how much the hinge decides
            thigh_front = k * -perp(sh, th) + (1 - k) * perp(body, th)    # away from the folding shin
            shin_front = k * perp(th, sh) + (1 - k) * perp(body, sh)      # facing back up the thigh
            angs.append(untwist(mats, th_b, hip, knee, thigh_front))
            angs.append(untwist(mats, sh_b, knee, ank, shin_front))
def prone_share(n, mats):
    """0 for an upright body (spine within UPRIGHT_DEG of vertical), 1 lying (from PRONE_DEG), smooth between."""
    hip = (mat4(mats[n.index('pzadek')]) @ np.append(J['pzadek>bricho'], 1))[:3]
    neck = (mat4(mats[n.index('hrudnik')]) @ np.append(J['hrudnik>krk'], 1))[:3]
    s = neck - hip
    tilt = np.degrees(np.arctan2(np.hypot(s[0], s[2]), s[1]))
    x = np.clip((tilt - UPRIGHT_DEG) / (PRONE_DEG - UPRIGHT_DEG), 0.0, 1.0)
    return x * x * (3 - 2 * x)
def fix_shoulders(n, phases, angs):
    chest = n.index('hrudnik')
    for side in 'lp':
        sh, up, fore, hand = (n.index(side+b) for b in ('rameno', 'biceps', 'loket', 'ruka'))
        jS = np.array(J['hrudnik>'+side+'rameno']); jB = np.array(J[side+'rameno>'+side+'biceps'])
        jE = np.array(J[side+'biceps>'+side+'loket'])
        for t, mats in phases:
            C = mat4(mats[chest]); B = mat4(mats[up])
            elbowA = (B @ np.append(jE, 1))[:3]; rootA = (B @ np.append(jB, 1))[:3]
            u = np.linalg.inv(C[:3,:3]) @ (elbowA - rootA)   # the upper arm, in the chest's rest frame
            Q, ang = swing(jE - jB, u, SHRUG_MAX, SHRUG); angs.append(ang)
            S2 = C @ about(Q, jS)
            d = (S2 @ np.append(jB, 1))[:3] - rootA          # where the arm's root moves to
            mats[sh] = unmat(S2)
            for b in (up, fore, hand):
                M = mat4(mats[b]); M[:3,3] += d; mats[b] = unmat(M)
if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    for f in sorted(glob.glob(os.path.join(src, '*.rtm'))):
        r = read_rtm(f); n = r['names']; name = os.path.basename(f); la, fa, sa = [], [], []
        fix_legs(n, r['phases'], la)
        fix_feet(n, r['phases'], fa)
        fix_shoulders(n, r['phases'], sa)
        write_rtm(os.path.join(dst, name), r['step'], n, r['phases'])
        print('%-14s leg untwist %3.0f..%3.0f  foot swing %3.0f..%3.0f  shoulder %3.0f..%3.0f deg'
              % (name, min(la), max(la), min(fa), max(fa), min(sa), max(sa)))
