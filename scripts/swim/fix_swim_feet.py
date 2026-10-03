# Sinkhole W3: turn the swim RTMs' feet the right way round (owner: "feet are backwards").
# Malprave's swim animations were retargeted by IK from Arma 1 key poses; in the shin's own frame the
# foot points BEHIND the shin (rest: forward, +x), the ankle bent back ~100 deg (the foot bone turns
# 90-136 deg against the shin; stock CWA animations 0-50). Each phase: the animated ankle->toe direction,
# in the shin's rest frame, is mirrored to the front of the shin (x -> |x|), keeping how far the foot
# is pointed; the foot (and the toes, rigidly) = the shin's rotation followed by the smallest swing about
# the rest ankle onto that direction -- no twist. The swing is limited to MAX_SWING deg.
# Usage: python fix_swim_feet.py <extracted swim.pbo dir> <out dir>; then repack with
#   PoseidonTools pbo pack <out dir> swim.pbo --prefix swim
# Reads Malprave's RTM reader and the fitted CWA rest skeleton (G:\Malprave	ools\swimanim, read only).
# The swim animations themselves are local test content and are not committed (provenance).
import sys, json, glob, os, numpy as np
sys.path.insert(0, r'G:\Malprave\tools\swimanim')
from rtm import read_rtm, write_rtm, mat4
J = json.load(open(r'G:\Malprave\tools\swimanim\joints.json'))
MAX_SWING = 60.0
src, dst = sys.argv[1], sys.argv[2]
def unmat(M): return np.concatenate([M[:3,0], M[:3,1], M[:3,2], M[:3,3]])
def swing(v, u, maxdeg):
    v = v/np.linalg.norm(v); u = u/np.linalg.norm(u)
    ax = np.cross(v, u); s = np.linalg.norm(ax); c = v@u
    ang = np.arctan2(s, c)
    if s < 1e-8: return np.eye(3), 0.0
    ax /= s; ang = min(ang, np.radians(maxdeg))
    K = np.array([[0,-ax[2],ax[1]],[ax[2],0,-ax[0]],[-ax[1],ax[0],0]])
    return np.eye(3) + np.sin(ang)*K + (1-np.cos(ang))*K@K, np.degrees(ang)
for f in sorted(glob.glob(os.path.join(src, '*.rtm'))):
    r = read_rtm(f); n = r['names']; name = os.path.basename(f); angs = []
    for side in 'lp':
        shin, foot, toe = n.index(side+'holen'), n.index(side+'chodidlo'), n.index(side+'prsty')
        a = np.array(J[side+'holen>'+side+'chodidlo']); k = np.array(J[side+'chodidlo>'+side+'prsty'])
        for t, mats in r['phases']:
            S_ = mat4(mats[shin]); F0 = mat4(mats[foot])
            toeA = (F0 @ np.append(k, 1))[:3]; ankA = (S_ @ np.append(a, 1))[:3]
            u = np.linalg.inv(S_[:3,:3]) @ (toeA - ankA)   # animated foot, in the shin's rest frame
            u[0] = abs(u[0])                                 # in front of the shin
            Q, ang = swing(k - a, u, MAX_SWING); angs.append(ang)
            R = S_[:3,:3] @ Q
            F = np.eye(4); F[:3,:3] = R; F[:3,3] = ankA - R @ a
            mats[foot] = unmat(F); mats[toe] = unmat(F)
    write_rtm(os.path.join(dst, name), r['step'], n, r['phases'])
    print('%-14s swing %4.0f..%4.0f deg' % (name, min(angs), max(angs)))
