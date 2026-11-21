// The C library calls the game code DLL leaves undefined, answered by this executable: the DLL is built without a C library
// (-nostdinc, no default libraries), so these are the executable's own, handed on under their usual names by
// host_exports.def.in. Only for a build with the code in a DLL.
#ifdef XBE_GAME_CODE_DLL
#include <cmath>
#include <cstring>

extern "C" {
void* x86_memcpy(void* d, const void* s, size_t n) { return std::memcpy(d, s, n); }
void* x86_memmove(void* d, const void* s, size_t n) { return std::memmove(d, s, n); }
void* x86_memset(void* d, int v, size_t n) { return std::memset(d, v, n); }
double x86_sin(double v) { return std::sin(v); }
double x86_cos(double v) { return std::cos(v); }
double x86_tan(double v) { return std::tan(v); }
double x86_atan2(double y, double x) { return std::atan2(y, x); }
double x86_fmod(double a, double b) { return std::fmod(a, b); }
double x86_remainder(double a, double b) { return std::remainder(a, b); }
double x86_sqrt(double v) { return std::sqrt(v); }
float x86_sqrtf(float v) { return std::sqrt(v); }
double x86_log2(double v) { return std::log2(v); }
double x86_exp2(double v) { return std::exp2(v); }
double x86_ldexp(double v, int e) { return std::ldexp(v, e); }
double x86_frexp(double v, int* e) { return std::frexp(v, e); }
}
#endif
