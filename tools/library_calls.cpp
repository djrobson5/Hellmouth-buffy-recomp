// A synthetic stand-in for recompiled code that uses every library call the recompiler (sdk/tools/recomp/codegen*.cpp) and
// cpu/x86_ops.h can make, whether or not this game's code does. tools/check_game_code_dll.sh compiles it as the game code DLL
// is compiled (the flags of game_code_flags.h, the freestanding headers, no C or C++ library), links it against the
// executable's import library, and checks that the link succeeds and that the calls the compiler left undefined are the ones
// the executable exports for them (sdk/runtime/cpu/host_exports.def.in). No game data is involved.
//
// Operands are read from the context, as in generated code, and have the types the generator writes (x87 values are double,
// SSE lanes float), as the overload picks sqrt or sqrtf. The script checks that every std:: call the generator writes, every
// builtin it uses and every inline helper of x86_ops.h is named here.
#include "cpu/x86_ops.h"

#define SYNTH(name) extern "C" __declspec(dllexport) void name(X86Context& __restrict ctx, uint8_t* base)

// x87: codegen_fp.cpp
SYNTH(synth_x87) {
    X87_ST(0) = std::fabs(X87_ST(0));
    X87_ST(0) = x87_prec(ctx, std::sqrt(X87_ST(0)));
    X87_ST(0) = std::sin(X87_ST(0)); ctx.fsw &= ~0x400;
    X87_ST(0) = std::cos(X87_ST(0)); ctx.fsw &= ~0x400;
    { double v = X87_ST(0); X87_ST(0) = std::sin(v); x87_push(ctx, std::cos(v)); ctx.fsw &= ~0x400; }
    X87_ST(0) = std::tan(X87_ST(0)); x87_push(ctx, 1.0); ctx.fsw &= ~0x400;
    { double v = std::atan2(X87_ST(1), X87_ST(0)); x87_pop(ctx); X87_ST(0) = v; }
    X87_ST(0) = std::exp2(X87_ST(0)) - 1.0;
    { double v = X87_ST(1) * std::log2(X87_ST(0)); x87_pop(ctx); X87_ST(0) = v; }
    { double v = X87_ST(1) * std::log2(X87_ST(0) + 1.0); x87_pop(ctx); X87_ST(0) = v; }
    X87_ST(0) = std::ldexp(X87_ST(0), (int)std::trunc(X87_ST(1)));
    X87_ST(0) = x87_round(ctx, X87_ST(0));
    X87_ST(0) = std::fmod(X87_ST(0), X87_ST(1)); ctx.fsw &= ~0x400;
    X87_ST(0) = std::remainder(X87_ST(0), X87_ST(1)); ctx.fsw &= ~0x400;
    { int e; double v = X87_ST(0); double mnt = std::frexp(v, &e); X87_ST(0) = (double)(e - 1); x87_push(ctx, mnt * 2.0); }
    // the rounding helpers and the compare and examine ones, with the helpers' own calls
    X87_ST(1) = std::floor(X87_ST(1)) + std::ceil(X87_ST(2)) + std::nearbyint(X87_ST(3));
    x87_compare(ctx, X87_ST(0), X87_ST(1));
    x87_examine(ctx);
    ctx.fsw = x87_status(ctx);
    ctx.eax = (uint32_t)x87_to_i32(ctx, X87_ST(0));
    ctx.ecx = (uint32_t)x87_to_i16(ctx, X87_ST(1));
    { int64_t q = x87_to_i64(ctx, X87_ST(2)); ctx.edx = (uint32_t)q; ctx.ebx = (uint32_t)(q >> 32); }
    { double v = X87_ST(0) / X87_ST(1); double a = X87_ST(2); bool u = a != a || v != v; ctx.zf = u || a == v; ctx.pf = u; }
    x87_pop(ctx);
}

// 80 bit loads and stores (FLD and FSTP of a tword)
SYNTH(synth_x87_ext) {
    x87_push(ctx, x87_load80(X86_PTR(uint8_t, ctx.esi)));
    x87_store80(X86_PTR(uint8_t, ctx.edi), X87_ST(0));
    x87_pop(ctx);
}

// SSE: codegen_fp.cpp, floats as the lanes hold them
SYNTH(synth_sse) {
    for (int i = 0; i < 4; i++) {
        float a = ctx.xmm[0].f[i], b = ctx.xmm[1].f[i];
        ctx.xmm[0].f[i] = std::sqrt(b);
        ctx.xmm[1].f[i] = 1.0f / std::sqrt(b);
        ctx.xmm[2].f[i] = sse_min(a, b);
        ctx.xmm[3].f[i] = sse_max(a, b);
        ctx.xmm[4].u[i] = sse_cmp(a, b, int(ctx.eax & 7));
        ctx.xmm[5].u[i] = (uint32_t)sse_cvt(a) + (uint32_t)sse_cvtt(b);
    }
    { float a = ctx.xmm[0].f[0], b = X86_MEMF32(ctx.esi); ctx.xmm[0].f[0] = std::sqrt(b); ctx.xmm[1].f[0] = a / b; }
    ctx.mm[0] = (uint64_t)(uint32_t)sse_cvt(ctx.xmm[2].f[0]) | ((uint64_t)(uint32_t)sse_cvtt(ctx.xmm[2].f[1]) << 32);
}

// MMX: codegen_fp.cpp, lanes of 8, 16 and 32 bits and the saturating forms
SYNTH(synth_mmx) {
    ctx.mm[0] = mmx_map2<uint8_t>(ctx.mm[0], ctx.mm[1], [](uint8_t a, uint8_t b) { return a + b; });
    ctx.mm[1] = mmx_map2<int8_t>(ctx.mm[1], ctx.mm[2], [](int8_t a, int8_t b) { return sat<int8_t>((int)a + b); });
    ctx.mm[2] = mmx_map2<int16_t>(ctx.mm[2], ctx.mm[3], [](int16_t a, int16_t b) { return sat<int16_t>((int)a + b); });
    ctx.mm[3] = mmx_map2<uint8_t>(ctx.mm[3], ctx.mm[4], [](uint8_t a, uint8_t b) { return sat<uint8_t>((int)a - b); });
    ctx.mm[4] = mmx_map2<uint16_t>(ctx.mm[4], ctx.mm[5], [](uint16_t a, uint16_t b) { return sat<uint16_t>((int)a + b); });
    ctx.mm[5] = mmx_map2<int16_t>(ctx.mm[5], ctx.mm[6], [](int16_t a, int16_t b) { return (int16_t)(((int)a * b) >> 16); });
    ctx.mm[6] = mmx_map2<uint32_t>(ctx.mm[6], ctx.mm[7], [](uint32_t a, uint32_t b) { return a == b ? 0xFFFFFFFFu : 0; });
    { uint64_t n = ctx.mm[7]; if (n > 15) n = 15; ctx.mm[0] = mmx_map2<int16_t>(ctx.mm[0], 0, [n](int16_t a, int16_t) { return (int16_t)(a >> n); }); }
    { int16_t a[4], b[4]; uint64_t x = ctx.mm[1], y = ctx.mm[2]; std::memcpy(a, &x, 8); std::memcpy(b, &y, 8); ctx.mm[3] = (uint64_t)a[0] + b[3]; }
    { uint8_t r[8]; uint64_t x = ctx.mm[4]; std::memcpy(r, &x, 8); r[ctx.eax & 7] = 1; std::memcpy(&ctx.mm[5], r, 8); }
}

// rep movs and rep stos, whose fast paths call memmove and memset with a run time length
SYNTH(synth_rep) {
    x86_rep_movs(ctx, base, 1);
    x86_rep_movs(ctx, base, 2);
    x86_rep_movs(ctx, base, 4);
    x86_rep_stos(ctx, base, 1);
    x86_rep_stos(ctx, base, 2);
    x86_rep_stos(ctx, base, 4);
}

// the C library calls a compiler makes for a copy, a move or a fill of a length it does not know
SYNTH(synth_mem) {
    std::memcpy(base + ctx.edi, base + ctx.esi, ctx.ecx);
    std::memmove(base + ctx.esi, base + ctx.edi, ctx.edx);
    std::memset(base + ctx.edi, int(ctx.eax), ctx.ebx);
}

// integer instructions: codegen_int.cpp
SYNTH(synth_int) {
    ctx.eax = __builtin_bswap32(ctx.eax);
    { uint32_t s = ctx.ecx; ctx.eax = (uint32_t)__builtin_ctz(s); ctx.edx = 31 - (uint32_t)__builtin_clz(s); }
    ctx.eax = X86_PARITY(ctx.ebx);
    { uint64_t n = ((uint64_t)ctx.edx << 32) | ctx.eax; uint32_t d = ctx.ecx; uint64_t q = n / d; ctx.eax = (uint32_t)q; ctx.edx = (uint32_t)(n % d); }
    { int64_t n = (int64_t)(((uint64_t)ctx.edx << 32) | ctx.eax); int32_t d = (int32_t)ctx.ecx; int64_t q = n / d; ctx.eax = (uint32_t)q; ctx.edx = (uint32_t)(n % d); }
    X86_BARRIER();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// locked and exchanging instructions on 8, 16, 32 and 64 bits
SYNTH(synth_atomic) {
    { uint8_t* p = X86_PTR(uint8_t, ctx.esi); uint8_t a = __atomic_load_n(p, __ATOMIC_SEQ_CST); uint8_t r = uint8_t(a + 1); __atomic_compare_exchange_n(p, &a, r, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); ctx.eax = __atomic_exchange_n(p, uint8_t(ctx.eax), __ATOMIC_SEQ_CST); }
    { uint16_t* p = X86_PTR(uint16_t, ctx.esi); uint16_t a = __atomic_load_n(p, __ATOMIC_SEQ_CST); uint16_t r = uint16_t(a + 1); __atomic_compare_exchange_n(p, &a, r, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); ctx.eax = __atomic_exchange_n(p, uint16_t(ctx.eax), __ATOMIC_SEQ_CST); }
    { uint32_t* p = X86_PTR(uint32_t, ctx.esi); uint32_t a = __atomic_load_n(p, __ATOMIC_SEQ_CST); uint32_t r = a + 1; __atomic_compare_exchange_n(p, &a, r, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); ctx.eax = __atomic_exchange_n(p, ctx.eax, __ATOMIC_SEQ_CST); ctx.ebx = __atomic_fetch_add(p, ctx.ebx, __ATOMIC_SEQ_CST); }
    { uint32_t* p = X86_PTR(uint32_t, ctx.edi); uint32_t bit = 1u << (ctx.ecx & 31); ctx.eax = __atomic_fetch_or(p, bit, __ATOMIC_SEQ_CST) & bit; ctx.eax |= __atomic_fetch_and(p, ~bit, __ATOMIC_SEQ_CST); ctx.eax |= __atomic_fetch_xor(p, bit, __ATOMIC_SEQ_CST); }
    { uint64_t x = ((uint64_t)ctx.edx << 32) | ctx.eax; uint64_t n = ((uint64_t)ctx.ecx << 32) | ctx.ebx; bool ok = __atomic_compare_exchange_n(X86_PTR(uint64_t, ctx.esi), &x, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); ctx.zf = ok; ctx.eax = (uint32_t)x; ctx.edx = (uint32_t)(x >> 32); }
}

// a frame of more than 4 KB, which the prologue probes through ___chkstk_ms
SYNTH(synth_big_frame) {
    volatile uint8_t buf[16384];
    buf[ctx.eax & 16383] = uint8_t(ctx.ebx);
    ctx.ecx = buf[ctx.edx & 16383];
}

// the runtime API, and the shape of generated functions: a function the executable may replace (X86_WEAK_FUNC, an
// /alternatename here), a traced one, an absent one that traps, and the catch that resumes a continuation
X86_EXTERN_FUNC(sub_FFFE0001);
X86_FUNC(__imp__sub_FFFE0001) { x86_trap(ctx, 0xFFFE0001u, "not in this image"); }
X86_WEAK_FUNC(sub_FFFE0001, __imp__sub_FFFE0001)
X86_EXTERN_FUNC(sub_FFFE0002);
X86_FUNC(__imp__sub_FFFE0002) { x86_priv(ctx, 0xFFFE0002u, "cli"); ctx.eax = 1; }
X86_TRACED_FUNC(sub_FFFE0002, __imp__sub_FFFE0002, 0xFFFE0002u)

SYNTH(synth_api) {
    const uint32_t x86_frame = ctx.esp;
    try {
        sub_FFFE0001(ctx, base);
        sub_FFFE0002(ctx, base);
        { uint64_t t = x86_rdtsc(); ctx.eax = (uint32_t)t; ctx.edx = (uint32_t)(t >> 32); }
        x86_cpuid(ctx);
        X86_PUSH32(0x1000u);
        x86_icall(ctx, base, ctx.eax);
        x86_kernel(ctx, base, ctx.ecx & 0xFF);
    } X86_CATCH_RESUME(x86_frame)
}

// the square root as the x87 register cache writes it, in a local: the compiler keeps a call to sqrt for the argument it must
// set errno for
SYNTH(synth_sqrt) {
    double x87_r4 = ctx.st[ctx.eax & 7];
    x87_r4 = x87_prec(ctx, std::sqrt(x87_r4));
    ctx.st[ctx.ecx & 7] = x87_r4;
}
