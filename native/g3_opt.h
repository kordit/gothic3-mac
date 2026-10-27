/* native/g3_opt.h - which native groups run (G3OPT in the trace control file, RECOMP_G3OPT at start). */
#ifndef G3_OPT_H
#define G3_OPT_H
#include <stdint.h>

enum {
    G3OPT_RAND = 1u << 0, /* rand */
    G3OPT_PARTICLES = 1u << 1, /* particles */
    G3OPT_OCCL = 1u << 2, /* occl */
    G3OPT_D3D = 1u << 3, /* d3d */
    G3OPT_STR = 1u << 4, /* str */
    G3OPT_MEM = 1u << 5, /* mem */
    G3OPT_AUDIO = 1u << 6, /* audio */
    G3OPT_VEC = 1u << 7, /* vec */
    G3OPT_SCENE = 1u << 8, /* scene */
    G3OPT_LEAF = 1u << 9, /* leaf */
    G3OPT_SHADOW = 1u << 10, /* shadow */
    G3OPT_OCCLMOVE = 1u << 11, /* occlmove: occl's hidden-before nodes drawn instead of waited for while the camera moves */
    G3OPT_PDO = 1u << 12, /* pdo: the particle system's processing (DoProcessV2) and depth sort */
    G3OPT_AI = 1u << 13, /* ai: the AI scripts' hot path in a fight (speech file lookup by name) */
    G3OPT_VFS = 1u << 14, /* vfs: FileSystem directory scan (FindFirstFileA for speech files) */
    G3OPT_CULL = 1u << 15, /* cull: the per-entity culling chain under ExtractFromEntity */
    G3OPT_EXEC = 1u << 16, /* exec: pieces of the render list's execution (particle depth sort, shader constants) */
    G3OPT_MOVE = 1u << 20, /* move: NPC movement and motion outside PhysX (skeleton-to-motion track matching) */
    G3OPT_ANIM = 1u << 19, /* anim: character animation, pose sampling and blending down the bone hierarchy */
    G3OPT_UV = 1u << 18, /* uv: the particles' vertex update (UpdateVertices) without SharedBase guest calls */
    G3OPT_ZLIB = 1u << 17, /* zlib: FileSystem's inflate (archive reads: textures, world loading, streaming) */
    G3OPT_LOAD = 1u << 21, /* load: the image cache's loader thread (read at engine start only) */
};

extern volatile uint32_t g3_opt;
/* G3OCCLVIS=<mask> in the trace control file (diagnostics, 0 = off): occlusion counts read at these
 * ResolveOCQuery call sites answer "visible" (native/occlusion.c): 1 entity queries in
 * ExtractFromFrustumOC (103fe7d9), 2 ResolveDynamicQuery (10403567), 4 UpdateIllumination
 * (1034e4bf), 8 the node queue (103fe9df, 103feb5b). */
extern volatile uint32_t g3_occl_vis;
#define G3OPT(bit) ((g3_opt & G3OPT_##bit) != 0)

#endif
