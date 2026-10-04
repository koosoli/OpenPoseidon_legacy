ugcave test cave (Claude for Dec, 2 Oct 2026)
build_cave.py  writes ugcave.p3d (MLOD: visual, geometry, memory with terrain_hole1-4, roadway, paths, fire geometry);
               uses build_ug.py (the basement builder: Lod class, pbo writer) and mlod.py.
               python3 build_cave.py addon  -> addon\ugcave.p3d; then pack with build_ug.pbo('ugcave.pbo','ugcave',[...]).
wrp2.py        OFP OPRW v2 reader (heights, objects) used to find a flat, object-free site on Eden.
cave_plan.png  plan view: roadway (colour = height), solids, hole areas (red dashed), the 25 m limit, path network.
Installed: G:\DEC-CWA\AddOns\ugcave.pbo; mission G:\DEC-CWA\Missions\UgCaveTest.Eden. Knowledge file section 3.14.

Textures (2 Oct 23:00): make_tex.py writes cave_wall / cave_ceil / cave_floor / cave_rock .dds (procedural, DXT1 with
mipmaps, own encoder + decoder check) into addon\; build_cave.py picks a texture per visual face (walls: v from
ground level 0 to 5.2 m down 1, darker towards the ceiling; floors / ceilings tile every 4 m; roof tops eden\tn.paa
every 12 m). preview.py renders textured perspective views (software z-buffer) -> cave_textured.png.
Rebuild: python3 make_tex.py; python3 build_cave.py addon; pack config.cpp + ugcave.p3d + the 4 dds with
build_ug.pbo('ugcave.pbo', 'ugcave', files).
