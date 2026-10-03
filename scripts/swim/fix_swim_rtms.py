# Sinkhole W3: repair Malprave's swim animations (swim.pbo, retargeted by IK from Arma 1 key poses).
# Usage: python fix_swim_rtms.py <extracted swim.pbo dir> <out dir>; then repack with
#   PoseidonTools pbo pack <out dir> swim.pbo --prefix swim
# Reads Malprave's RTM reader and the fitted CWA rest skeleton (G:\Malprave\tools\swimanim, read only).
# The swim animations themselves are local test content and are not committed (provenance).
#
# 1. Feet (owner: "feet are backwards"). In the shin's own frame the foot points BEHIND the shin
#    (rest: forward, +x), the ankle bent back ~100 deg (stock CWA animations turn the foot 0-50 deg
#    against the shin). Each phase: the animated ankle->toe direction, in the shin's rest frame, is
#    mirrored to the front of the shin (x -> |x|), keeping how far the foot is pointed; the foot (and
#    the toes, rigidly) = the shin's rotation followed by the smallest swing about the rest ankle onto
#    that direction -- no twist. The swing is limited to FOOT_MAX deg.
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
FOOT_MAX = 60.0
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
        for t, mats in phases:
            S_ = mat4(mats[shin]); F0 = mat4(mats[foot])
            toeA = (F0 @ np.append(k, 1))[:3]; ankA = (S_ @ np.append(a, 1))[:3]
            u = np.linalg.inv(S_[:3,:3]) @ (toeA - ankA)   # animated foot, in the shin's rest frame
            u[0] = abs(u[0])                                 # in front of the shin
            Q, ang = swing(k - a, u, FOOT_MAX); angs.append(ang)
            F = S_ @ about(Q, a)
            mats[foot] = unmat(F); mats[toe] = unmat(F)
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
src, dst = sys.argv[1], sys.argv[2]
for f in sorted(glob.glob(os.path.join(src, '*.rtm'))):
    r = read_rtm(f); n = r['names']; name = os.path.basename(f); fa, sa = [], []
    fix_feet(n, r['phases'], fa)
    fix_shoulders(n, r['phases'], sa)
    write_rtm(os.path.join(dst, name), r['step'], n, r['phases'])
    print('%-14s foot swing %3.0f..%3.0f  shoulder %3.0f..%3.0f deg' % (name, min(fa), max(fa), min(sa), max(sa)))
