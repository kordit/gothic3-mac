// d3d9_ffp.cpp - the Direct3D 9 fixed-function pipeline as vs_2_0 / ps_2_0
// bytecode (d3d9_ffp.h).
#include "d3d9_ffp.h"
#include "d3d9_shader.h"
#include "../runtime/guest.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace d9sh;

namespace {

// ---------------------------------------------------------------- tokens --
// Register types beyond d3d9_shader.h's: a pixel shader's t registers share
// R_ADDR's number, and its colour output is R_COLOROUT.
constexpr uint32_t R_PS_TEXTURE = 3;
constexpr uint32_t SWZ_XYZW = 0xe4, SWZ_WWWW = 0xff, SWZ_XXXX = 0x00;
constexpr uint32_t MOD_NEG = 1;

uint32_t reg_bits(uint32_t type, uint32_t index) {
    return 0x80000000u | (type & 7) << 28 | (type & 0x18) << 8 | (index & 0x7ff);
}
struct D {
    uint32_t type, index, mask = 0xf;
    bool sat = false;
};
struct S {
    uint32_t type, index, swz = SWZ_XYZW, mod = 0;
};
D dst(uint32_t type, uint32_t index, uint32_t mask = 0xf, bool sat = false) {
    return {type, index, mask, sat};
}
S src(uint32_t type, uint32_t index, uint32_t swz = SWZ_XYZW, uint32_t mod = 0) {
    return {type, index, swz, mod};
}
S neg(S s) {
    s.mod = MOD_NEG;
    return s;
}
S rep(S s, uint32_t component) {
    s.swz = component * 0x55;
    return s;
}

struct Asm {
    std::vector<uint32_t> t;
    explicit Asm(uint32_t version) {
        t.push_back(version);
    }
    void op(uint32_t code, const D &d, std::initializer_list<S> srcs) {
        t.push_back(code | uint32_t(1 + srcs.size()) << 24);
        t.push_back(reg_bits(d.type, d.index) | (d.mask & 0xf) << 16 | (d.sat ? 1u << 20 : 0));
        for (const S &s : srcs)
            t.push_back(reg_bits(s.type, s.index) | (s.swz & 0xff) << 16 | (s.mod & 0xf) << 24);
    }
    // dcl: the usage token, then the register as a destination.
    void dcl(uint32_t usage_token, const D &d) {
        t.push_back(OP_DCL | 2u << 24);
        t.push_back(0x80000000u | usage_token);
        t.push_back(reg_bits(d.type, d.index) | (d.mask & 0xf) << 16);
    }
    std::vector<uint8_t> bytes() {
        t.push_back(OP_END);
        std::vector<uint8_t> out(t.size() * 4);
        memcpy(out.data(), t.data(), out.size());
        return out;
    }
};

// ----------------------------------------------------------- declaration --
struct Element {
    bool present = false;
    uint32_t type = 0; // D3DDECLTYPE
};
struct Inputs {
    Element position, positiont, normal, color[2], texcoord[8];
};
Inputs read_declaration(const uint8_t *decl, size_t size) {
    Inputs in;
    for (size_t i = 0; decl && i + 8 <= size; i += 8) {
        const uint8_t *e = decl + i;
        if ((e[0] | e[1] << 8) == 0xff)
            break;
        uint8_t type = e[4], usage = e[6], index = e[7];
        Element *el = nullptr;
        if (usage == 0 && index == 0)
            el = &in.position;
        else if (usage == 9 && index == 0)
            el = &in.positiont;
        else if (usage == 3 && index == 0)
            el = &in.normal;
        else if (usage == 10 && index < 2)
            el = &in.color[index];
        else if (usage == 5 && index < 8)
            el = &in.texcoord[index];
        if (el) {
            el->present = true;
            el->type = type;
        }
    }
    return in;
}
// Components a D3DDECLTYPE carries: FLOAT1..4 are 0..3, the rest 2 or 4.
uint32_t components(uint32_t type) {
    static const uint8_t n[17] = {1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4, 3, 3, 2, 4};
    return type < 17 ? n[type] : 4;
}

void mat_mul(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}
// m4x4 reads c[n+i] as column i of the row-vector matrix D3D stores.
void put_columns(const float m[16], float (*c)[4]) {
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            c[i][k] = m[k * 4 + i];
}
void put_argb(uint32_t argb, float c[4]) {
    c[0] = float(argb >> 16 & 0xff) / 255.0f;
    c[1] = float(argb >> 8 & 0xff) / 255.0f;
    c[2] = float(argb & 0xff) / 255.0f;
    c[3] = float(argb >> 24) / 255.0f;
}

// ---------------------------------------------------------------- vertex --
// Inputs v0 position, v1 / v2 the colours, v3..v10 the texture coordinate
// sets. Constants: c0-c3 world x view x projection (or, pre-transformed, c0 the
// screen-to-clip scale and offset), c4 (1, 1, 1, 1), c5 (0, 0, 0, 0) and
// (1e-20 in x), c6-c37 the texture transforms of stages 0..7.
std::vector<uint8_t> vertex_program(const D9Pipeline &pl, const Inputs &in, const int32_t vp[4],
                                    uint32_t stages, float (*c)[4]) {
    Asm a(0xfffe0200u);
    const bool pretransformed = in.positiont.present;
    a.dcl(pretransformed ? 9u : 0u, dst(R_INPUT, 0));
    if (in.color[0].present)
        a.dcl(10u, dst(R_INPUT, 1));
    if (in.color[1].present)
        a.dcl(10u | 1u << 16, dst(R_INPUT, 2));
    for (uint32_t i = 0; i < 8; ++i)
        if (in.texcoord[i].present)
            a.dcl(5u | i << 16, dst(R_INPUT, 3 + i));
    for (int k = 0; k < 4; ++k)
        c[4][k] = 1.0f;
    c[5][0] = 1e-20f;
    if (pretransformed) {
        // D3D screen space to clip space: x' = x * 2 / w - 1 - 2 X / w, y' = 1 - (y - Y) * 2 / h,
        // then times w = 1 / rhw so the interpolation stays perspective-correct.
        const float w = float(vp[2] > 0 ? vp[2] : 1), h = float(vp[3] > 0 ? vp[3] : 1);
        c[0][0] = 2.0f / w;
        c[0][1] = -2.0f / h;
        c[0][2] = -1.0f - 2.0f * float(vp[0]) / w;
        c[0][3] = 1.0f + 2.0f * float(vp[1]) / h;
        a.op(OP_MAD, dst(R_TEMP, 0, 0x3), {src(R_INPUT, 0), src(R_CONST, 0), src(R_CONST, 0, 0xee /* zwzw */)});
        a.op(OP_MOV, dst(R_TEMP, 0, 0x4), {src(R_INPUT, 0)});
        a.op(OP_MOV, dst(R_TEMP, 0, 0x8), {src(R_CONST, 4)});
        a.op(OP_MAX, dst(R_TEMP, 1, 0x1), {rep(src(R_INPUT, 0), 3), rep(src(R_CONST, 5), 0)});
        a.op(OP_RCP, dst(R_TEMP, 1, 0x1), {rep(src(R_TEMP, 1), 0)});
        a.op(OP_MUL, dst(R_RASTOUT, 0), {src(R_TEMP, 0), rep(src(R_TEMP, 1), 0)});
    } else {
        float wv[16], wvp[16];
        mat_mul(pl.transform[24], pl.transform[2], wv); // WORLD (256 -> 24), VIEW
        mat_mul(wv, pl.transform[3], wvp);             // PROJECTION
        put_columns(wvp, &c[0]);
        a.op(OP_M4X4, dst(R_RASTOUT, 0), {src(R_INPUT, 0), src(R_CONST, 0)});
    }
    a.op(OP_MOV, dst(R_ATTROUT, 0), {src(in.color[0].present ? R_INPUT : R_CONST, in.color[0].present ? 1 : 4)});
    a.op(OP_MOV, dst(R_ATTROUT, 1),
         {in.color[1].present ? src(R_INPUT, 2) : rep(src(R_CONST, 5), 1)});
    for (uint32_t s = 0; s < stages; ++s) {
        const uint32_t index = pl.tss[s][11] & 7; // D3DTSS_TEXCOORDINDEX, without the generation mode
        if (pl.tss[s][11] >> 16)
            log_once("d3d9.ffp.texgen", "d3d9 ffp: texture coordinate generation is not modelled (stage %u, %08x)",
                     s, pl.tss[s][11]);
        if (!in.texcoord[index].present) {
            a.op(OP_MOV, dst(R_TEXCRDOUT, s), {rep(src(R_CONST, 5), 1)});
            continue;
        }
        const uint32_t flags = pl.tss[s][24] & 0xff; // D3DTSS_TEXTURETRANSFORMFLAGS count
        if (!flags) {
            a.op(OP_MOV, dst(R_TEXCRDOUT, s), {src(R_INPUT, 3 + index)});
            continue;
        }
        // A set with fewer components is padded as Direct3D does before the
        // matrix: (u, 1, 0, 0) or (u, v, 1, 0), so the translation sits in the
        // row after the last coordinate.
        const uint32_t n = components(in.texcoord[index].type);
        a.op(OP_MOV, dst(R_TEMP, 2), {rep(src(R_CONST, 5), 1)});
        a.op(OP_MOV, dst(R_TEMP, 2, (1u << n) - 1), {src(R_INPUT, 3 + index)});
        if (n < 4)
            a.op(OP_MOV, dst(R_TEMP, 2, 1u << n), {src(R_CONST, 4)});
        put_columns(pl.transform[16 + s], &c[6 + 4 * s]);
        a.op(OP_M4X4, dst(R_TEXCRDOUT, s), {src(R_TEMP, 2), src(R_CONST, 6 + 4 * s)});
    }
    a.op(OP_MOV, dst(R_RASTOUT, 1, 0x1), {src(R_CONST, 4)});
    return a.bytes();
}

// ----------------------------------------------------------------- pixel --
// Registers: r0 current, r1 temp, r2 the stage's texture, r3-r10 arguments
// and intermediates, r11 the stage's result before RESULTARG. Constants: c0
// the texture factor, c1-c8 the stage constants, c9 (0.5), c10 (1), c11 (0),
// c12 (4).
struct Pixel {
    Asm a{0xffff0200u};
    uint32_t scratch = 3;
    // An argument (D3DTA_*) as a source: complement needs an instruction.
    S arg(uint32_t ta, uint32_t stage, bool alpha) {
        S s;
        switch (ta & 0xf) {
        case 0: s = src(R_INPUT, 0); break;          // DIFFUSE
        case 1: s = src(R_TEMP, 0); break;           // CURRENT
        case 2: s = src(R_TEMP, 2); break;           // TEXTURE
        case 3: s = src(R_CONST, 0); break;          // TFACTOR
        case 4: s = src(R_INPUT, 1); break;          // SPECULAR
        case 5: s = src(R_TEMP, 1); break;           // TEMP
        case 6: s = src(R_CONST, 1 + stage); break;  // CONSTANT
        default: s = src(R_TEMP, 0); break;
        }
        if ((ta & 0x20) || alpha) // ALPHAREPLICATE, or an alpha operation's argument
            s = rep(s, 3);
        if (ta & 0x10) { // COMPLEMENT: 1 - x
            const uint32_t r = scratch++;
            a.op(OP_ADD, dst(R_TEMP, r), {src(R_CONST, 10), neg(s)});
            return src(R_TEMP, r);
        }
        return s;
    }
    // One operation (D3DTOP_*) into r11 under `mask`.
    void operation(uint32_t top, uint32_t stage, const uint32_t *tss, bool alpha, uint32_t mask) {
        scratch = 3;
        const uint32_t i1 = alpha ? 5 : 2, i2 = alpha ? 6 : 3, i0 = alpha ? 27 : 26;
        const D out = dst(R_TEMP, 11, mask, true), raw = dst(R_TEMP, 11, mask);
        const S r6 = src(R_TEMP, 11);
        auto a1 = [&] { return arg(tss[i1], stage, alpha); };
        auto a2 = [&] { return arg(tss[i2], stage, alpha); };
        auto blend = [&](S factor) {
            S x = a1(), y = a2();
            a.op(OP_LRP, out, {factor, x, y});
        };
        switch (top) {
        case 2: a.op(OP_MOV, out, {a1()}); break;                              // SELECTARG1
        case 3: a.op(OP_MOV, out, {a2()}); break;                              // SELECTARG2
        case 4: { S x = a1(), y = a2(); a.op(OP_MUL, out, {x, y}); break; }     // MODULATE
        case 5: case 6: {                                                       // MODULATE2X, 4X
            S x = a1(), y = a2();
            a.op(OP_MUL, raw, {x, y});
            a.op(OP_ADD, top == 5 ? out : raw, {r6, r6});
            if (top == 6)
                a.op(OP_ADD, out, {r6, r6});
            break;
        }
        case 7: { S x = a1(), y = a2(); a.op(OP_ADD, out, {x, y}); break; }     // ADD
        case 8: case 9: {                                                       // ADDSIGNED, 2X
            S x = a1(), y = a2();
            a.op(OP_ADD, raw, {x, y});
            a.op(OP_ADD, top == 8 ? out : raw, {r6, neg(src(R_CONST, 9))});
            if (top == 9)
                a.op(OP_ADD, out, {r6, r6});
            break;
        }
        case 10: { S x = a1(), y = a2(); a.op(OP_ADD, out, {x, neg(y)}); break; } // SUBTRACT
        case 11: {                                                              // ADDSMOOTH: x + y (1 - x)
            S x = a1(), y = a2();
            const uint32_t r = scratch++;
            a.op(OP_ADD, dst(R_TEMP, r), {src(R_CONST, 10), neg(x)});
            a.op(OP_MAD, out, {y, src(R_TEMP, r), x});
            break;
        }
        case 12: blend(rep(src(R_INPUT, 0), 3)); break;                         // BLENDDIFFUSEALPHA
        case 13: blend(rep(src(R_TEMP, 2), 3)); break;                          // BLENDTEXTUREALPHA
        case 14: blend(rep(src(R_CONST, 0), 3)); break;                         // BLENDFACTORALPHA
        case 16: blend(rep(src(R_TEMP, 0), 3)); break;                          // BLENDCURRENTALPHA
        case 15: {                                                              // BLENDTEXTUREALPHAPM
            S x = a1(), y = a2();
            const uint32_t r = scratch++;
            a.op(OP_ADD, dst(R_TEMP, r), {src(R_CONST, 10), neg(rep(src(R_TEMP, 2), 3))});
            a.op(OP_MAD, out, {y, src(R_TEMP, r), x});
            break;
        }
        case 18: { S x = a1(), y = a2(); a.op(OP_MAD, out, {rep(x, 3), y, x}); break; } // MODULATEALPHA_ADDCOLOR
        case 19: { S x = a1(), y = a2(); a.op(OP_MAD, out, {x, y, rep(x, 3)}); break; } // MODULATECOLOR_ADDALPHA
        case 20: case 21: {                                  // MODULATEINVALPHA_ADDCOLOR, MODULATEINVCOLOR_ADDALPHA
            S x = a1(), y = a2();
            const uint32_t r = scratch++;
            a.op(OP_ADD, dst(R_TEMP, r), {src(R_CONST, 10), neg(top == 20 ? rep(x, 3) : x)});
            a.op(OP_MAD, out, {src(R_TEMP, r), y, top == 20 ? x : rep(x, 3)});
            break;
        }
        case 24: {                                                              // DOTPRODUCT3
            S x = a1(), y = a2();
            const uint32_t r = scratch++, q = scratch++;
            a.op(OP_MAD, dst(R_TEMP, r), {x, src(R_CONST, 10, SWZ_XYZW), neg(src(R_CONST, 9))});
            a.op(OP_MAD, dst(R_TEMP, q), {y, src(R_CONST, 10, SWZ_XYZW), neg(src(R_CONST, 9))});
            a.op(OP_DP3, raw, {src(R_TEMP, r), src(R_TEMP, q)});
            a.op(OP_MUL, out, {r6, src(R_CONST, 12)}); // (2x - 1) . (2y - 1) = 4 ((x - .5) . (y - .5))
            break;
        }
        case 25: {                                                              // MULTIPLYADD
            S x = a1(), y = a2(), z = arg(tss[i0], stage, alpha);
            a.op(OP_MAD, out, {x, y, z});
            break;
        }
        case 26: {                                                              // LERP
            S x = a1(), y = a2(), z = arg(tss[i0], stage, alpha);
            a.op(OP_LRP, out, {z, x, y});
            break;
        }
        default:
            log_once(alpha ? "d3d9.ffp.aop" : "d3d9.ffp.cop",
                     "d3d9 ffp: texture operation %u (stage %u, %s) is not modelled; it selects its first argument",
                     top, stage, alpha ? "alpha" : "colour");
            a.op(OP_MOV, out, {a1()});
            break;
        }
    }
};

bool uses_texture(const uint32_t *tss) {
    auto t = [&](uint32_t i) { return (tss[i] & 0xf) == 2; };
    const uint32_t cop = tss[1], aop = tss[4];
    bool c = t(2) || (cop != 2 && t(3)) || ((cop == 25 || cop == 26) && t(26)) || cop == 13 || cop == 15;
    bool a = aop > 1 && (t(5) || (aop != 2 && t(6)) || ((aop == 25 || aop == 26) && t(27)) || aop == 13 || aop == 15);
    return c || a;
}

std::vector<uint8_t> pixel_program(const D9Pipeline &pl, uint32_t stages, float (*c)[4]) {
    Pixel p;
    Asm &a = p.a;
    put_argb(pl.rs[60], c[0]); // TEXTUREFACTOR
    for (uint32_t s = 0; s < 8; ++s)
        put_argb(pl.tss[s][32], c[1 + s]); // D3DTSS_CONSTANT
    for (int k = 0; k < 4; ++k) {
        c[9][k] = 0.5f;
        c[10][k] = 1.0f;
        c[12][k] = 4.0f;
    }
    a.dcl(0, dst(R_INPUT, 0));
    a.dcl(0, dst(R_INPUT, 1));
    uint32_t sampled = 0;
    for (uint32_t s = 0; s < stages; ++s)
        if (uses_texture(pl.tss[s]) && pl.sampler_tex[s]) {
            sampled |= 1u << s;
            a.dcl(0, dst(R_PS_TEXTURE, s));
            a.dcl(2u << 27, dst(R_SAMPLER, s)); // 2D
        }
    a.op(OP_MOV, dst(R_TEMP, 0), {src(R_INPUT, 0)});
    a.op(OP_MOV, dst(R_TEMP, 1), {src(R_CONST, 11)});
    for (uint32_t s = 0; s < stages; ++s) {
        const uint32_t *tss = pl.tss[s];
        if (sampled >> s & 1)
            a.op(OP_TEX, dst(R_TEMP, 2), {src(R_PS_TEXTURE, s), src(R_SAMPLER, s)});
        else
            a.op(OP_MOV, dst(R_TEMP, 2), {src(R_CONST, 10)}); // no texture: white, so MODULATE keeps the colour
        if (tss[1] == 22 || tss[1] == 23)
            log_once("d3d9.ffp.bump", "d3d9 ffp: bump mapping is not modelled (stage %u)", s);
        p.operation(tss[1], s, tss, false, 0x7);
        // An alpha operation of DISABLE under an enabled colour one keeps the current alpha.
        if (tss[4] > 1)
            p.operation(tss[4], s, tss, true, 0x8);
        else
            a.op(OP_MOV, dst(R_TEMP, 11, 0x8), {src(R_TEMP, 0)});
        const bool temp = (tss[28] & 0xf) == 5; // D3DTSS_RESULTARG
        a.op(OP_MOV, dst(R_TEMP, temp ? 1 : 0), {src(R_TEMP, 11)});
    }
    if (pl.rs[29]) // SPECULARENABLE
        a.op(OP_ADD, dst(R_TEMP, 0, 0x7, true), {src(R_TEMP, 0), src(R_INPUT, 1)});
    a.op(OP_MOV, dst(R_COLOROUT, 0), {src(R_TEMP, 0)});
    return a.bytes();
}

// Programs live as long as the process: the renderer caches them by key.
D9ShaderBytes intern(std::vector<uint8_t> bytes, uint64_t *key) {
    static std::mutex mutex;
    static auto *programs = new std::unordered_map<uint64_t, D9ShaderBytes>();
    *key = code_key(bytes.data(), bytes.size());
    std::lock_guard<std::mutex> lock(mutex);
    D9ShaderBytes &slot = (*programs)[*key];
    if (!slot.bytes)
        slot.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    return slot;
}

} // namespace

std::string d9_ffp_describe(const D9Pipeline &pl, const uint8_t *decl, size_t decl_size) {
    const Inputs in = read_declaration(decl, decl_size);
    char buf[256];
    std::string out = in.positiont.present ? "xyzrhw" : (in.position.present ? "xyz" : "no position");
    if (in.normal.present)
        out += "+normal";
    for (int i = 0; i < 2; ++i)
        if (in.color[i].present)
            out += i ? "+specular" : "+diffuse";
    for (int i = 0; i < 8; ++i)
        if (in.texcoord[i].present)
            out += "+tex" + std::to_string(i);
    snprintf(buf, sizeof buf, "; lighting %u, tfactor %08x, specular %u, alphatest %u ref %u func %u, blend %u %u/%u, "
             "z %u/%u func %u, stencil %u func %u ref %u mask %x pass %u, colorwrite %x, cull %u", pl.rs[137],
             pl.rs[60], pl.rs[29], pl.rs[15], pl.rs[24], pl.rs[25], pl.rs[27], pl.rs[19], pl.rs[20], pl.rs[7],
             pl.rs[14], pl.rs[23], pl.rs[52], pl.rs[56], pl.rs[57], pl.rs[58], pl.rs[55], pl.rs[168], pl.rs[22]);
    out += buf;
    if (!in.positiont.present) {
        static const char *names[3] = {"world", "view", "projection"};
        static const int slots[3] = {24, 2, 3};
        for (int m = 0; m < 3; ++m) {
            const float *t = pl.transform[slots[m]];
            snprintf(buf, sizeof buf, "; %s [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", names[m], t[0],
                     t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15]);
            out += buf;
        }
    }
    snprintf(buf, sizeof buf, "; viewport %u %u %u %u", pl.viewport[0], pl.viewport[1], pl.viewport[2], pl.viewport[3]);
    out += buf;
    for (uint32_t s = 0; s < 8 && pl.tss[s][1] > 1; ++s) {
        snprintf(buf, sizeof buf, "; stage %u: tex %u, colour %u(%x, %x) alpha %u(%x, %x), result %u, coords %x",
                 s, pl.sampler_tex[s], pl.tss[s][1], pl.tss[s][2], pl.tss[s][3], pl.tss[s][4], pl.tss[s][5],
                 pl.tss[s][6], pl.tss[s][28], pl.tss[s][11]);
        out += buf;
    }
    return out;
}

void d9_ffp_shaders(const D9Pipeline &pl, const uint8_t *decl, size_t decl_size, const int32_t viewport[4],
                    bool want_vs, bool want_ps, D9FfpShaders *out) {
    const Inputs in = read_declaration(decl, decl_size);
    uint32_t stages = 0;
    while (stages < 8 && pl.tss[stages][1] > 1) // up to the first COLOROP of DISABLE
        ++stages;
    if (want_vs) {
        if (pl.rs[137] && !in.positiont.present && in.normal.present)
            log_once("d3d9.ffp.light", "d3d9 ffp: lighting is not modelled; the vertex colour stands in");
        out->vs = intern(vertex_program(pl, in, viewport, stages, out->vconst), &out->vs_key);
    }
    if (want_ps)
        out->ps = intern(pixel_program(pl, stages, out->pconst), &out->ps_key);
}
