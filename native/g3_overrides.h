/* native/g3_overrides.h - Gothic 3's native replacements (game.toml [translate] overrides and native).
 *
 * Every generated module includes this before it names its functions, so FN_<addr> below sends
 * every direct call, tail call and jump-table case for that address to g3n_<addr> instead
 * (kit chunk.h). The translated original stays linked as fn_<addr>: each g3n_ runs the native
 * version only while its group is on (G3OPT, native/g3_opt.c) and calls fn_<addr> otherwise.
 *
 * Changing this header recompiles every module (about the whole game); the .c files beside it
 * build alone. So the list is fixed up front: add an address only when a group truly needs it. */
#ifndef G3_OVERRIDES_H
#define G3_OVERRIDES_H

/* rand (rand.c): stage 1: CRT rand through per-thread data (_getptd, TLS shims) on every particle */
#define FN_1006eb6d g3n_1006eb6d /* SharedBase _rand */
void g3n_1006eb6d(X86 *c);
#define FN_1006eb60 g3n_1006eb60 /* SharedBase srand */
void g3n_1006eb60(X86 *c);
#define FN_100323a0 g3n_100323a0 /* SharedBase bCRange3::GetRandom */
void g3n_100323a0(X86 *c);
#define FN_1006ffe8 g3n_1006ffe8 /* SharedBase __getptd */
void g3n_1006ffe8(X86 *c);
#define FN_1006ff71 g3n_1006ff71 /* SharedBase _getptd_noexit */
void g3n_1006ff71(X86 *c);

/* particles (particles.c): stage 1: particle update (PUP Opt.ParticleUpdate) */
#define FN_103b5800 g3n_103b5800 /* Engine TickParticles */
void g3n_103b5800(X86 *c);
#define FN_103bf2b0 g3n_103bf2b0 /* Engine CalcValue */
void g3n_103bf2b0(X86 *c);
#define FN_103c0140 g3n_103c0140 /* Engine CalcValueLinear */
void g3n_103c0140(X86 *c);
#define FN_103af5d0 g3n_103af5d0 /* Engine CalcColor */
void g3n_103af5d0(X86 *c);
#define FN_103af790 g3n_103af790 /* Engine CalcColorFactor */
void g3n_103af790(X86 *c);
#define FN_103afae0 g3n_103afae0 /* Engine CalcExtents */
void g3n_103afae0(X86 *c);
#define FN_103b3630 g3n_103b3630 /* Engine UpdateVertices */
void g3n_103b3630(X86 *c);

/* d3d (d3d.c): stage 5: the D3D9 draw path (vertex declarations, redundant state) */
#define FN_10311240 g3n_10311240 /* Engine eCDX9Primitive::Execute */
void g3n_10311240(X86 *c);
#define FN_10304020 g3n_10304020 /* Engine SetVertexDeclaration */
void g3n_10304020(X86 *c);
#define FN_10303bd0 g3n_10303bd0 /* Engine CreateVertexDeclaration */
void g3n_10303bd0(X86 *c);

/* str (strings.c): stage 6: string comparison (PUP fast_ascii_stricmp) */
#define FN_1006c3df g3n_1006c3df /* SharedBase ___ascii_stricmp */
void g3n_1006c3df(X86 *c);
#define FN_1006c15b g3n_1006c15b /* SharedBase _tolower */
void g3n_1006c15b(X86 *c);
#define FN_10001ae0 g3n_10001ae0 /* SharedBase bCString::CompareFast */
void g3n_10001ae0(X86 *c);
#define FN_10005390 g3n_10005390 /* SharedBase bCString::operator== (JMP 10001ae0) */
void g3n_10005390(X86 *c);
#define FN_10004460 g3n_10004460 /* SharedBase bCString::FindNoCase */
void g3n_10004460(X86 *c);

/* mem (memory.c): stage 6: the static CRT memcpy/memmove copies (PUP fast_memcpy) */
#define FN_10078780 g3n_10078780 /* SharedBase _memcpy */
void g3n_10078780(X86 *c);
#define FN_1006bba0 g3n_1006bba0 /* SharedBase _memmove */
void g3n_1006bba0(X86 *c);
#define FN_1056ad40 g3n_1056ad40 /* Engine _memcpy */
void g3n_1056ad40(X86 *c);
#define FN_10564220 g3n_10564220 /* Engine _memmove */
void g3n_10564220(X86 *c);
#define FN_10af1980 g3n_10af1980 /* Game _memcpy */
void g3n_10af1980(X86 *c);
#define FN_10ae9d80 g3n_10ae9d80 /* Game _memmove */
void g3n_10ae9d80(X86 *c);
#define FN_10dde620 g3n_10dde620 /* GFC _memcpy */
void g3n_10dde620(X86 *c);
#define FN_10dd99b0 g3n_10dd99b0 /* GFC _memmove */
void g3n_10dd99b0(X86 *c);
#define FN_10e34ef0 g3n_10e34ef0 /* GUI _memcpy */
void g3n_10e34ef0(X86 *c);
#define FN_10e2d980 g3n_10e2d980 /* GUI _memmove */
void g3n_10e2d980(X86 *c);
#define FN_10f58ef0 g3n_10f58ef0 /* Script _memcpy */
void g3n_10f58ef0(X86 *c);
#define FN_10f4d5a0 g3n_10f4d5a0 /* Script _memmove */
void g3n_10f4d5a0(X86 *c);

/* vec (vector.c): SharedBase vector and matrix primitives the particle and culling loops call per item */
#define FN_10011c00 g3n_10011c00 /* SharedBase operator* (vector by scalar) */
void g3n_10011c00(X86 *c);
#define FN_10010e20 g3n_10010e20 /* SharedBase bCVector::GetSquareMagnitude */
void g3n_10010e20(X86 *c);
#define FN_100103d0 g3n_100103d0 /* SharedBase bCVector::Scale */
void g3n_100103d0(X86 *c);
#define FN_100103f0 g3n_100103f0 /* SharedBase Translate */
void g3n_100103f0(X86 *c);
#define FN_1001ae70 g3n_1001ae70 /* SharedBase InvTranslate */
void g3n_1001ae70(X86 *c);
#define FN_100138d0 g3n_100138d0 /* SharedBase bCMatrix3::ModifyXAxis */
void g3n_100138d0(X86 *c);
#define FN_10016b10 g3n_10016b10 /* SharedBase bCMatrix::SetToProduct */
void g3n_10016b10(X86 *c);
#define FN_1001b9e0 g3n_1001b9e0 /* SharedBase bCBox::Merge(bCVector) */
void g3n_1001b9e0(X86 *c);
#define FN_1003a9b0 g3n_1003a9b0 /* SharedBase bCInfDoubleCylinder::Cull */
void g3n_1003a9b0(X86 *c);
#define FN_10042f30 g3n_10042f30 /* SharedBase bCSphere::Cull */
void g3n_10042f30(X86 *c);
#define FN_10035fd0 g3n_10035fd0 /* SharedBase bCFrustum::Cull */
void g3n_10035fd0(X86 *c);

/* scene (scene.c): frustum culling and the render list's gathering, about 17% of a frame in the NoMenu world */
#define FN_103ff970 g3n_103ff970 /* Engine eCFrustumDatabase::ExtractFromEntity */
void g3n_103ff970(X86 *c);
#define FN_1040c9d0 g3n_1040c9d0 /* Engine eCFrustumFilter::CullFrustum (leaf) */
void g3n_1040c9d0(X86 *c);
#define FN_10494fc0 g3n_10494fc0 /* Engine FUN_10494fc0 (leaf, __fastcall, under eCEntityAdmin::Process) */
void g3n_10494fc0(X86 *c);
#define FN_104956f0 g3n_104956f0 /* Engine FUN_104956f0 (under eCEntityAdmin::Process) */
void g3n_104956f0(X86 *c);

/* leaf (leaf.c): small math leaves called per entity and per light, about 5% of a frame self time */
#define FN_10010060 g3n_10010060 /* SharedBase bCVector::Normalize */
void g3n_10010060(X86 *c);
#define FN_10011bd0 g3n_10011bd0 /* SharedBase bCEulerAngles::operator- */
void g3n_10011bd0(X86 *c);
#define FN_10015be0 g3n_10015be0 /* SharedBase bCMatrix::bCMatrix(bCMatrix const &) */
void g3n_10015be0(X86 *c);
#define FN_1001da30 g3n_1001da30 /* SharedBase bCSphere::Intersects(bCSphere const &) */
void g3n_1001da30(X86 *c);
#define FN_10295f40 g3n_10295f40 /* Engine FUN_10295f40 (leaf, __fastcall float *) */
void g3n_10295f40(X86 *c);
#define FN_10299740 g3n_10299740 /* Engine FUN_10299740 (leaf, __fastcall float *) */
void g3n_10299740(X86 *c);
#define FN_102995d0 g3n_102995d0 /* Engine FUN_102995d0 (float) */
void g3n_102995d0(X86 *c);

/* shadow (shadow.c): Engine's CRT qsort, the shadow pass's and the frustum lists' sort */
#define FN_10564ec0 g3n_10564ec0 /* Engine _qsort */
void g3n_10564ec0(X86 *c);

/* pdo (pdo.c): the particle system's processing under OnProcess 103aed10 and its depth sort */
#define FN_10564592 g3n_10564592 /* Engine _rand */
void g3n_10564592(X86 *c);
#define FN_1056bee7 g3n_1056bee7 /* Engine __getptd */
void g3n_1056bee7(X86 *c);
#define FN_1056be70 g3n_1056be70 /* Engine _getptd_noexit */
void g3n_1056be70(X86 *c);
#define FN_103c00c0 g3n_103c00c0 /* Engine eCVectorScale::CalcValue */
void g3n_103c00c0(X86 *c);
#define FN_103af640 g3n_103af640 /* Engine eCParticle_PS::CalcVelocityFactor */
void g3n_103af640(X86 *c);
#define FN_103af6b0 g3n_103af6b0 /* Engine eCParticle_PS::CalcRevolutionFactor */
void g3n_103af6b0(X86 *c);
#define FN_1122a320 g3n_1122a320 /* PhysXCore vector add (the box-tree ray walk's leaves) */
void g3n_1122a320(X86 *c);
#define FN_11163d50 g3n_11163d50 /* PhysXCore vector subtract */
void g3n_11163d50(X86 *c);
#define FN_1117a160 g3n_1117a160 /* PhysXCore vector times float */
void g3n_1117a160(X86 *c);
#define FN_1118b390 g3n_1118b390 /* PhysXCore box centre */
void g3n_1118b390(X86 *c);
#define FN_1118b3d0 g3n_1118b3d0 /* PhysXCore box half extents */
void g3n_1118b3d0(X86 *c);
#define FN_11289670 g3n_11289670 /* PhysXCore ray (segment) against a box tree, collecting leaves */
void g3n_11289670(X86 *c);
#define FN_11289e10 g3n_11289e10 /* PhysXCore ray (segment) against a quantized box tree */
void g3n_11289e10(X86 *c);
/* also pdo: CalcExtents 103afae0 (its FN_ line is under particles above), and the depth
 * comparator 103af9c0 inline in shadow.c's qsort (no FN_: it is only called through a pointer) */

/* move (move.c): NPC movement and motion in a fight outside PhysX */
#define FN_1054a160 g3n_1054a160 /* Engine FUN_1054a160 (a motion's tracks matched to the skeleton's nodes) */
void g3n_1054a160(X86 *c);

/* uv (uv.c): the particles' vertex update (UpdateVertices 103b3630): its SharedBase leaves natively */
#define FN_100103b0 g3n_100103b0 /* SharedBase bCVector::Scale(float) */
void g3n_100103b0(X86 *c);
#define FN_10010570 g3n_10010570 /* SharedBase bCVector::SetCrossProduct */
void g3n_10010570(X86 *c);
#define FN_10011140 g3n_10011140 /* SharedBase bCVector::GetTranslated */
void g3n_10011140(X86 *c);
#define FN_10011080 g3n_10011080 /* SharedBase bCVector::GetNormalized */
void g3n_10011080(X86 *c);
#define FN_10011040 g3n_10011040 /* SharedBase bCVector::operator- (unary) */
void g3n_10011040(X86 *c);

/* savegame.c */
#define FN_109b9e90 g3n_109b9e90 /* Game gCHUDFileManager save: F5 keeps the previous quick save (native/savegame.c) */
void g3n_109b9e90(X86 *c);

/* anim (anim.c): character animation, pose sampling and blending down the bone hierarchy */
#define FN_1054a020 g3n_1054a020 /* Engine FUN_1054a020 (a track's pose copied out) */
void g3n_1054a020(X86 *c);
#define FN_1055bce0 g3n_1055bce0 /* Engine FUN_1055bce0 (quaternion blend, short arc) */
void g3n_1055bce0(X86 *c);
#define FN_1054bf00 g3n_1054bf00 /* Engine FUN_1054bf00 (quaternion blend in place, ECX/EDX) */
void g3n_1054bf00(X86 *c);
#define FN_1054b510 g3n_1054b510 /* Engine FUN_1054b510 (two packed keys decoded and blended) */
void g3n_1054b510(X86 *c);
#define FN_10540e00 g3n_10540e00 /* Engine FUN_10540e00 (a box grown by a point, fastcall) */
void g3n_10540e00(X86 *c);
#define FN_1054a580 g3n_1054a580 /* Engine FUN_1054a580 (a rotation track sampled at a time) */
void g3n_1054a580(X86 *c);

/* mods (mods.c): the Poprawki set's script mods, each only while it is in active-mods.json */
#define FN_10989810 g3n_10989810 /* Game gCGUIEntityRenderer::DrawFocusNames (Font Scale) */
void g3n_10989810(X86 *c);
#define FN_10a793e0 g3n_10a793e0 /* Game gCScriptProcessingUnit::sAIOutputInstr (Subtitles) */
void g3n_10a793e0(X86 *c);
/* G3Fixes (docs/mods/dll-g3fixes.md, hook numbers from its table); the hooks inside functions are
   game.toml native_sites, native_site_<at> in mods.c */
#define FN_10f4b230 g3n_10f4b230 /* Script Entity::EndTransform (1, Magic Mist) */
void g3n_10f4b230(X86 *c);
#define FN_10ffcdd0 g3n_10ffcdd0 /* Script_Game _AI_UseInventoryItem (2, UseFunc) */
void g3n_10ffcdd0(X86 *c);
#define FN_10fddb80 g3n_10fddb80 /* Script_Game IsDeadlyDamage (3) */
void g3n_10fddb80(X86 *c);
#define FN_10fdd530 g3n_10fdd530 /* Script_Game CanBurn (4) */
void g3n_10fdd530(X86 *c);
#define FN_10fdd7b0 g3n_10fdd7b0 /* Script_Game CanFreeze (5) */
void g3n_10fdd7b0(X86 *c);
#define FN_1105c3c0 g3n_1105c3c0 /* Script_Game Respawn (6) */
void g3n_1105c3c0(X86 *c);
#define FN_10fce570 g3n_10fce570 /* Script_Game ZS_ObserveSuspect (7) */
void g3n_10fce570(X86 *c);
#define FN_10fc2f50 g3n_10fc2f50 /* Script_Game OnFollowPlayer (8) */
void g3n_10fc2f50(X86 *c);
#define FN_10fc3d60 g3n_10fc3d60 /* Script_Game OnGuidePlayer (9) */
void g3n_10fc3d60(X86 *c);
#define FN_102c6bd0 g3n_102c6bd0 /* Engine eCConsole::OnExecute (10, resetpos, revive, tprevive) */
void g3n_102c6bd0(X86 *c);
#define FN_10dd4680 g3n_10dd4680 /* GFC CFFGFCWnd::OnPaint and 7 more, one thunk (11, icons) */
void g3n_10dd4680(X86 *c);
#define FN_10276490 g3n_10276490 /* Engine eCApplication::Process (18, mCG3Fixes::Process) */
void g3n_10276490(X86 *c);
#define FN_10a5f180 g3n_10a5f180 /* Game gCPlayerMemory_PS::CalcMissingValue (20) */
void g3n_10a5f180(X86 *c);

/* Extended Frying (mods.c, docs/mods/dll-extendedfrying.md): Immersive World's dependency */
#define FN_109b2280 g3n_109b2280 /* Game gCHUDListCtrlInvTransforms slot 67: the frying page's list */
void g3n_109b2280(X86 *c);
#define FN_1101a740 g3n_1101a740 /* Script_Game OnPlayerFryStack */
void g3n_1101a740(X86 *c);

/* phases (phases.c): the render half of a frame, timed for the kit's per-frame phases (logic: mods.c, 10276490) */
#define FN_10276100 g3n_10276100 /* Engine eCApplication::DoRender */
void g3n_10276100(X86 *c);

/* strings.c: the bCString hash map lookup the quick save leans on (group str) */
#define FN_10a73590 g3n_10a73590 /* Game FUN_10a73590 */
void g3n_10a73590(X86 *c);

/* savezip.c: a save's compression with libdeflate (group zlib) */
#define FN_10ae30f0 g3n_10ae30f0 /* Game zlib deflate, the save writer's only */
void g3n_10ae30f0(X86 *c);

/* createlod (createlod.c): CreateLoD's split into hard-skinning groups without the square vertex search (D-043
 * point 2, decision of 29.10.2026: the subtree under 1053f420, docs/perf/createlod.md) */
#define FN_1053f420 g3n_1053f420 /* Engine FUN_1053f420 (an actor's meshes split into groups; the key maps' lifetime) */
void g3n_1053f420(X86 *c);
#define FN_10548ad0 g3n_10548ad0 /* Engine FUN_10548ad0 (a triangle's corners into its group) */
void g3n_10548ad0(X86 *c);

/* ao (ao.c): a SpeedTree's ambient occlusion at its load, the emitter walks in C (D-043 point 2, decision of
 * 29.10.2026, D-051, docs/perf/ao-drzew.md) */
#define FN_1035a0b0 g3n_1035a0b0 /* Engine eCAOTreeNode::ComputeSphereOcclusionFactor */
void g3n_1035a0b0(X86 *c);
#define FN_10359e90 g3n_10359e90 /* Engine eCAOTreeNode::ComputeOcclusionFactor */
void g3n_10359e90(X86 *c);

/* aocache (aocache.c): a SpeedTree's ambient occlusion from a result computed once, offline, at the boundary of the
 * whole eCAOMeshBase::Compute (D-043 point 2, D-052, docs/perf/ao-drzew.md) */
#define FN_10357ba0 g3n_10357ba0 /* Engine eCAmbientOcclusionPreprocessor::eCAOMeshBase::Compute */
void g3n_10357ba0(X86 *c);

/* fmodogg (fmodogg.c): an Ogg Vorbis sample decoded natively and handed to FMOD as PCM, at the boundary of fmodex's
 * export System::createSound (D-053, docs/perf/fmod-ogg.md) */
#define FN_113bf8d0 g3n_113bf8d0 /* fmodex FMOD::System::createSound */
void g3n_113bf8d0(X86 *c);

/* navzone (navzone.c): every NPC's zone test each frame, the whole gCNavZone_PS::Test3DPointInZone at its boundary
 * (D-043 point 2) */
#define FN_10a27180 g3n_10a27180 /* Game gCNavZone_PS::Test3DPointInZone */
void g3n_10a27180(X86 *c);

/* snap (snap.c): the render's gathering and execution apart, and its writes to the logic's state moved to the end of
 * the gathering (D-054 points 2 and 3, docs/PLAN.md step 3) */
#define FN_1040e430 g3n_1040e430 /* Engine eCRenderSystemAdmin::Render */
void g3n_1040e430(X86 *c);
#define FN_10400530 g3n_10400530 /* Engine eCFrustumDatabase::Execute */
void g3n_10400530(X86 *c);
#define FN_103afa70 g3n_103afa70 /* Engine eCParticle_PS::DepthSortParticles */
void g3n_103afa70(X86 *c);
#define FN_104035c0 g3n_104035c0 /* Engine eCFrustumElementItem::Request */
void g3n_104035c0(X86 *c);
#define FN_10468f60 g3n_10468f60 /* Engine eCSpeedTreeAdmin::OnBeginRenderSolid */
void g3n_10468f60(X86 *c);

/* fmodmem (fmodmem.c): FMOD's memory callbacks from the kit's span instead of SharedBase's MemHeap */
#define FN_103cb030 g3n_103cb030 /* Engine eCAudioSystem::FModAllocMemory */
void g3n_103cb030(X86 *c);
#define FN_102b7190 g3n_102b7190 /* Engine eCAudioSystem::FModReallocMemory */
void g3n_102b7190(X86 *c);
#define FN_102b71b0 g3n_102b71b0 /* Engine eCAudioSystem::FModFreeMemory */
void g3n_102b71b0(X86 *c);

#endif
