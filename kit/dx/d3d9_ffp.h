// d3d9_ffp.h - the Direct3D 9 fixed-function pipeline as shaders.
//
// A draw with no vertex or pixel shader bound uses the fixed-function
// pipeline. The GPU path draws only shaders, so this writes the vs_2_0 and
// ps_2_0 bytecode that does what the fixed-function pipeline would for the
// device's current state and vertex declaration, plus the constants that
// bytecode reads. The renderer then treats it like any game shader.
//
// Modelled: pre-transformed (POSITIONT) and transformed (POSITION, world x
// view x projection) vertices, the diffuse and specular vertex colours, up to
// eight texture coordinate sets with D3DTSS_TEXCOORDINDEX and texture
// transforms, the texture stage colour and alpha operations with their
// arguments (complement, alpha replicate, TEMP and RESULTARG), the texture
// factor and per-stage constants, and SPECULARENABLE. Not modelled, each
// logged once when a draw asks for it: lighting (the vertex colour or white
// stands in), texture coordinate generation, bump mapping and vertex fog.
#pragma once
#include "d3d9_pipeline.h"

#include <cstddef>
#include <cstdint>

struct D9FfpShaders {
    D9ShaderBytes vs, ps;     // the generated programs; empty for a stage the caller did not ask for
    uint64_t vs_key = 0, ps_key = 0; // d9sh::code_key of each
    float vconst[40][4] = {}; // what the vertex program reads, c0 upwards
    float pconst[16][4] = {}; // what the pixel program reads, c0 upwards
};

// The fixed-function programs for this pipeline state and vertex declaration
// (D3DVERTEXELEMENT9 records). `viewport` is x, y, width, height in pixels,
// which pre-transformed positions are relative to. Programs are cached by
// their bytes, so the same state gives the same objects.
// One line describing the fixed-function state a draw used, for the log.
#include <string>
std::string d9_ffp_describe(const D9Pipeline &pl, const uint8_t *decl, size_t decl_size);

void d9_ffp_shaders(const D9Pipeline &pl, const uint8_t *decl, size_t decl_size, const int32_t viewport[4],
                    bool want_vs, bool want_ps, D9FfpShaders *out);
