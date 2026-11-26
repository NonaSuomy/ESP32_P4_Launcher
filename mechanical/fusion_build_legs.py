"""70-degree desk legs for the Guition JC1060P470C 7" ESP32-P4 board (also the 2026 "V2" panel).

Run in Fusion (Utilities > Scripts, or the Fusion MCP "script" feature). Builds a new direct-modelling
document: two legs + a reference board (screen 8 mm, 4 standoffs, back case), and writes
P4_7in_leg_70deg.stl next to this script. Print two legs, lying on the side (15.8 mm face down).

Each leg screws onto the two standoffs of one side (M2.5, holes 166 x 99 mm, standoff tops 16.38 mm
behind the glass, data from ESP32_P4_FingerprintLoginSystem/mechanical); the glass rests on a ledge.
"""
import adsk.core, adsk.fusion, math
import os
OUT = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.getcwd()
ANG = 70.0; WALL = 5.0; W = 15.8; LEDGE_N = 12.0; LEDGE_T = 3.0
HOLE_D1 = 7.36; HOLE_PITCH = 99.0; PLATE_TOP = 115.0
BRACE_D = 60.0; LB = 70.0; BOARD_T = 16.38
HOLE_R = 1.45; CB_R = 2.9; CB_DEPTH = 2.5; INSET = 7.0
a = math.radians(ANG); S, C = math.sin(a), math.cos(a)
P0Y = -((-WALL)*C + (-LEDGE_T)*S)
def xy(n, d): return (n*S - d*C, P0Y + n*C + d*S)
def P(x, y, z=0.0): return adsk.core.Point3D.create(x/10, y/10, z/10)
def run(_context: str):
    app = adsk.core.Application.get()
    doc = app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
    doc.name = 'P4 7in V2 bracket legs 70deg'
    design = adsk.fusion.Design.cast(app.activeProduct)
    design.designType = adsk.fusion.DesignTypes.DirectDesignType
    design.fusionUnitsManager.distanceDisplayUnits = adsk.fusion.DistanceUnits.MillimeterDistanceUnits
    root = design.rootComponent
    K = xy(-WALL, -LEDGE_T); A = xy(LEDGE_N, -LEDGE_T); B = xy(LEDGE_N, 0); Cc = xy(0, 0)
    E = xy(0, PLATE_TOP); F = xy(-WALL, PLATE_TOP); G = xy(-WALL, BRACE_D); H = (-LB, 0.0)
    pts = [K, A, B, Cc, E, F, G, H]
    sk = root.sketches.add(root.xYConstructionPlane); sk.name = 'Leg profile'
    L = sk.sketchCurves.sketchLines
    pp = [P(*p) for p in pts]
    for i in range(len(pp)): L.addByTwoPoints(pp[i], pp[(i+1) % len(pp)])
    # lightening window: brace triangle (K, G, H) shrunk toward its incenter
    tri = [K, G, H]
    la = [math.dist(tri[1], tri[2]), math.dist(tri[0], tri[2]), math.dist(tri[0], tri[1])]
    per = sum(la); ic = (sum(la[i]*tri[i][0] for i in range(3))/per, sum(la[i]*tri[i][1] for i in range(3))/per)
    s2 = per/2; area = math.sqrt(s2*(s2-la[0])*(s2-la[1])*(s2-la[2])); r = area/s2
    f = (r - INSET)/r
    win = [(ic[0] + (p[0]-ic[0])*f, ic[1] + (p[1]-ic[1])*f) for p in tri]
    wp = [P(*p) for p in win]
    for i in range(3): L.addByTwoPoints(wp[i], wp[(i+1) % 3])
    prof = None
    for i in range(sk.profiles.count):
        pr = sk.profiles.item(i)
        if pr.profileLoops.count == 2: prof = pr
    ext = root.features.extrudeFeatures
    inp = ext.createInput(prof, adsk.fusion.FeatureOperations.NewBodyFeatureOperation)
    inp.setSymmetricExtent(adsk.core.ValueInput.createByReal(W/10), True)
    body = ext.add(inp).bodies.item(0); body.name = 'Leg'
    tb = adsk.fusion.TemporaryBRepManager.get()
    for dd in (HOLE_D1, HOLE_D1 + HOLE_PITCH):
        for (n1, n2, rr) in ((-WALL-1, 1, HOLE_R), (-WALL-1, -WALL+CB_DEPTH, CB_R)):
            p1 = xy(n1, dd); p2 = xy(n2, dd)
            cyl = tb.createCylinderOrCone(P(*p1), rr/10, P(*p2), rr/10)
            tool = root.bRepBodies.add(cyl)
            tc = adsk.core.ObjectCollection.create(); tc.add(tool)
            ci = root.features.combineFeatures.createInput(body, tc)
            ci.operation = adsk.fusion.FeatureOperations.CutFeatureOperation
            root.features.combineFeatures.add(ci)
    # reference board (hole pattern 166 x 99, holes 7.36 from the bottom edge, 7.9 from the side)
    occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    bc = occ.component; bc.name = 'Board 7in (reference only)'
    def slab(n0, n1, d0, d1, z0, z1):
        tb2 = adsk.fusion.TemporaryBRepManager.get()
        c0 = xy((n0+n1)/2, (d0+d1)/2)
        ob = adsk.core.OrientedBoundingBox3D.create(P(c0[0], c0[1], (z0+z1)/2),
             adsk.core.Vector3D.create(S, C, 0), adsk.core.Vector3D.create(-C, S, 0),
             (n1-n0)/10, (d1-d0)/10, (z1-z0)/10)
        return bc.bRepBodies.add(tb2.createBox(ob))
    bb = slab(BOARD_T-8.0, BOARD_T, 0, 113.72, 0, 181.8); bb.name = 'Screen'
    pc = slab(0.5, BOARD_T-8.0, (113.72-74.67)/2-5, (113.72+74.67)/2+5, 90.9-67.3, 90.9+67.3); pc.name = 'Back case'
    for hz in (7.9, 7.9+166):
        for hd in (HOLE_D1, HOLE_D1+HOLE_PITCH):
            p1 = xy(0, hd); p2 = xy(BOARD_T-8.0, hd)
            bc.bRepBodies.add(tb.createCylinderOrCone(P(p1[0], p1[1], hz), 0.25, P(p2[0], p2[1], hz), 0.25))
    m = adsk.core.Matrix3D.create(); m.translation = adsk.core.Vector3D.create(0, 0, -0.79); occ.transform = m
    lib = app.materialLibraries.itemByName('Fusion Appearance Library')
    ap = lib.appearances.itemByName('Glass (Clear)') if lib else None
    if ap: bb.appearance = ap
    # second leg at the other side's holes (166 mm away)
    t2 = tb.copy(body)
    tr = adsk.core.Matrix3D.create(); tr.translation = adsk.core.Vector3D.create(0, 0, 16.6)
    tb.transform(t2, tr)
    c2 = root.bRepBodies.add(t2); c2.name = 'Leg 2'
    b = body.boundingBox
    print('leg bbox mm', [round(v*10, 1) for v in (b.minPoint.x, b.minPoint.y, b.maxPoint.x, b.maxPoint.y)], 'vol cm3', round(body.volume, 1), 'faces', body.faces.count, 'inset r', round(r, 1))
    em = design.exportManager
    o = em.createSTLExportOptions(body, OUT + '/P4_7in_leg_70deg.stl')
    o.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
    em.execute(o)
