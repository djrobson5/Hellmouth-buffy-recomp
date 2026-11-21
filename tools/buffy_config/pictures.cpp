// The achievement pictures and the game's icon, from the player's own disc, in C++ so that the player's PC needs neither Python nor ffmpeg
// (the Windows first start runs this through buffy_config --pictures). Two ports in one file:
//
//  - the achievement pictures are tools/make_achievement_icons.py, and write the same pixels: the same PICKS, the same texture
//    decoding (a DXT5 block's colours as the script works them out, not as a graphics card does) and the same crops. Only the PNG
//    encoder differs, so the files differ in their bytes and not in what they show (tools/check_config_tool.sh compares the pixels).
//  - the icon is tools/make_icon.py's composition (the purple sky, the moon and the name with a shadow, from the launch screen) as a
//    full square: Windows icons are not Apple's rounded shape on a 824 / 1024 grid. Its scaling and blurs are approximations of
//    ffmpeg's (bicubic and gaussian); nothing needs them to match.
//
// Both read the two disc files named in pictures.h and nothing else.
#include "pictures.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "gpu/stb/stb_image_write.h"

namespace fs = std::filesystem;

namespace pictures {

namespace {

using Bytes = std::vector<uint8_t>;

constexpr const char* kThumbnails = "interfc/levelThumbnails.mtx";
constexpr const char* kLaunch = "media/launch_buffy.xpr";
constexpr int kLaunchWidth = 1024, kLaunchHeight = 512, kLaunchData = 0x800;  // a swizzled X8R8G8B8 texture, its pixels after the file's header

bool ReadAll(const fs::path& path, Bytes& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return !f.bad();
}

// false for no bytes too: a picture whose encoding failed comes here empty, and an empty file must not be written and called made
bool WriteAll(const fs::path& path, const Bytes& data) {
    if (data.empty()) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    f.flush();
    return bool(f);
}

uint32_t U32(const Bytes& b, size_t at) { return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24; }
uint16_t U16(const Bytes& b, size_t at) { return uint16_t(b[at] | b[at + 1] << 8); }

// ---- the launch screen ----------------------------------------------------------------------------------------------------------

// Morton order over the square part, with the extra bits of the longer side (here x) above it (make_icon.py unswizzle). Rows of
// (r, g, b) bytes, 1024 x 512.
bool DecodeLaunch(const Bytes& data, Bytes& rgb, std::string& why) {
    if (data.size() < size_t(kLaunchData) + size_t(kLaunchWidth) * kLaunchHeight * 4) {
        why = std::string(kLaunch) + " is too short to be the launch screen";
        return false;
    }
    auto spread = [](uint32_t n) {
        uint32_t s = 0;
        for (int bit = 0; bit < 9; bit++) s += ((n >> bit) & 1) << (2 * bit);
        return s;
    };
    std::vector<uint32_t> xs(kLaunchWidth), ys(kLaunchHeight);
    for (uint32_t x = 0; x < uint32_t(kLaunchWidth); x++) xs[x] = spread(x & 511) | ((x >> 9) << 18);
    for (uint32_t y = 0; y < uint32_t(kLaunchHeight); y++) ys[y] = spread(y) << 1;
    rgb.assign(size_t(kLaunchWidth) * kLaunchHeight * 3, 0);
    size_t at = 0;
    for (int y = 0; y < kLaunchHeight; y++)
        for (int x = 0; x < kLaunchWidth; x++, at += 3) {
            const size_t px = size_t(kLaunchData) + size_t(xs[size_t(x)] + ys[size_t(y)]) * 4;  // blue, green, red, unused
            rgb[at] = data[px + 2];
            rgb[at + 1] = data[px + 1];
            rgb[at + 2] = data[px];
        }
    return true;
}

// ---- the level thumbnails -----------------------------------------------------------------------------------------------------

struct Picture {
    int w = 0, h = 0;
    Bytes rgb;  // w * h * 3
};

// A MULTITEX has a 0x80 byte record for each texture: its name at 0x3C, then 0x7C on its width, height, format, offset and size.
bool ThumbnailTable(const Bytes& mtx, std::map<std::string, uint32_t>& table, std::string& why) {
    if (mtx.size() < 0x18 || std::memcmp(mtx.data(), "MULTITEX", 8) != 0) {
        why = std::string(kThumbnails) + " is not a MULTITEX file";
        return false;
    }
    const uint32_t count = U32(mtx, 0x14);
    if (count > 4096 || 0x7C + size_t(count) * 0x80 + 0x28 > mtx.size()) {
        why = std::string(kThumbnails) + " has a table that does not fit in the file";
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const size_t named = 0x3C + size_t(i) * 0x80, fields = 0x7C + size_t(i) * 0x80;
        const char* name = reinterpret_cast<const char*>(&mtx[named]);
        std::string key(name, strnlen(name, 32));
        const uint32_t width = U32(mtx, fields), height = U32(mtx, fields + 4);
        const uint32_t format = U32(mtx, fields + 0x1C), offset = U32(mtx, fields + 0x20), size = U32(mtx, fields + 0x24);
        if (width != 256 || height != 128 || format != 5 || size != 0x8000) {
            why = key + " in " + kThumbnails + " is not the expected 256 x 128 DXT5";
            return false;
        }
        table[key] = offset;
    }
    return true;
}

// A 256 x 128 DXT5 as rows of (r, g, b). The alpha block is skipped: the thumbnails don't use it. The arithmetic is the script's:
// the five and six bit channels widened by * 255 / 31 and * 255 / 63, and always four colours, the two in between at a third.
void DecodeDxt5(const uint8_t* data, Picture& out) {
    out.w = 256;
    out.h = 128;
    out.rgb.assign(256 * 128 * 3, 0);
    auto rgb = [](uint32_t v, int c[3]) {
        c[0] = int((v >> 11) & 31) * 255 / 31;
        c[1] = int((v >> 5) & 63) * 255 / 63;
        c[2] = int(v & 31) * 255 / 31;
    };
    size_t at = 8;
    for (int y = 0; y < 128; y += 4)
        for (int x = 0; x < 256; x += 4) {
            const uint32_t c0 = uint32_t(data[at] | data[at + 1] << 8), c1 = uint32_t(data[at + 2] | data[at + 3] << 8);
            const uint32_t bits = uint32_t(data[at + 4]) | uint32_t(data[at + 5]) << 8 | uint32_t(data[at + 6]) << 16 | uint32_t(data[at + 7]) << 24;
            at += 16;
            int colours[4][3];
            rgb(c0, colours[0]);
            rgb(c1, colours[1]);
            for (int k = 0; k < 3; k++) {
                colours[2][k] = (2 * colours[0][k] + colours[1][k]) / 3;
                colours[3][k] = (colours[0][k] + 2 * colours[1][k]) / 3;
            }
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    const int* c = colours[(bits >> (2 * (i + 4 * j))) & 3];
                    uint8_t* p = &out.rgb[(size_t(y + j) * 256 + size_t(x + i)) * 3];
                    p[0] = uint8_t(c[0]);
                    p[1] = uint8_t(c[1]);
                    p[2] = uint8_t(c[2]);
                }
        }
}

// ---- what is picked -----------------------------------------------------------------------------------------------------------

constexpr const char* kLaunchSource = "launch";  // as a source in kPicks: the launch screen, 1024 x 512

struct Pick {
    const char* id;
    const char* source;
    int cx, cy, size;
};

// the pictures, in the order of make_achievement_icons.py's PICKS (to change a pick, change both): the thumbnails' pictures fill about
// 170 x 128 of each 256 x 128 texture, with a painted purple edge. Launchers and the game draw them in a circle, so what matters sits in
// the middle. Some show a boss, so nothing may show a picture before it is earned.
const Pick kPicks[] = {
    {"ch_training", "m00_spanishmission_3", 75, 50, 100},
    {"boss_malik", "m01_highschool_4", 120, 60, 100},
    {"boss_spike", "m02_bronze_4", 100, 70, 100},
    {"boss_scylla", "m03_cemetery_4", 120, 60, 100},
    {"boss_matereani", "m05_sunken_4", 60, 60, 100},
    {"boss_scylla_snake", "m06_mansion_2", 100, 60, 100},
    {"boss_urd", "m07_docks_5", 80, 70, 100},
    {"boss_malik_tiger", "m08_highschool_4", 85, 65, 100},
    {"boss_angel", "m09_foundry_5", 100, 60, 100},
    {"boss_dark_buffy", "m11_dreamrealm_3", 100, 60, 100},
    {"boss_dreamers", "m11_dreamrealm_4", 80, 60, 100},
    {"boss_master", "m12_apse", 95, 60, 100},
    {"game_finished", "m12_bronze-ending", 95, 60, 100},
    {"game_hard", kLaunchSource, 395, 86, 132},  // the moon
    {"kills_1", "m00_spanishmission_1", 88, 50, 90},
    {"kills_100", "m03_cemetery_1", 100, 70, 100},
    {"kills_250", "m10_sunken-church_1", 80, 70, 90},
    {"kills_500", kLaunchSource, 450, 140, 220},  // Buffy under the moon
    {"kills_pole_20", "m07_docks_3", 88, 64, 100},
    {"kill_soaker", "m05_sunken_2", 88, 64, 100},
    {"kills_crossbow_5", "m02_bronze_1", 88, 64, 100},
    {"secret_1", "hub01", 90, 60, 100},
    {"secret_level", "m04_mausoleum_2", 88, 60, 100},
    {"secret_30", "m04_mausoleum_1", 85, 55, 100},
    {"secret_all", "hub01", 136, 68, 70},  // Giles
    {"die_once", "m03_cemetery_2", 110, 50, 100},
    {"chapter_deathless", "m10_sunken-church_0", 88, 70, 100},
    {"history", "m01_highschool_1", 100, 70, 100},
};

// ---- PNG and ICO --------------------------------------------------------------------------------------------------------------

void Append(void* context, void* data, int size) {
    auto* out = static_cast<Bytes*>(context);
    out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

// empty when the encoder failed (it returns 0)
Bytes EncodePng(int w, int h, int channels, const uint8_t* pixels) {
    Bytes out;
    if (!stbi_write_png_to_func(Append, &out, w, h, channels, pixels, w * channels)) out.clear();
    return out;
}

// ---- the icon -----------------------------------------------------------------------------------------------------------------

// the launch screen's pieces, as (width, height, x, y) there
struct Piece {
    int w, h, x, y;
};
constexpr Piece kSky{170, 170, 45, 165}, kMoon{122, 122, 334, 26}, kLogo{216, 134, 28, 24};
// and where they go, in make_icon.py's 824 square (here the whole icon: its size is Scale * 824)
constexpr double kShape = 824.0, kSkyBrightness = 0.55, kLogoShadow = 16.0;
constexpr int kMoonSize = 520, kMoonX = 152, kMoonY = 50, kLogoWidth = 770, kLogoX = 27, kLogoY = 300;
constexpr int kCanvas = 512;  // composed here, then made smaller: the icon's largest size is 256

// a picture of floats, red, green, blue, alpha (0 to 255 each)
struct Canvas {
    int w = 0, h = 0;
    std::vector<float> p;
    Canvas() = default;
    Canvas(int width, int height) : w(width), h(height), p(size_t(width) * size_t(height) * 4, 0.0f) {}
    float* At(int x, int y) { return &p[(size_t(y) * size_t(w) + size_t(x)) * 4]; }
    const float* At(int x, int y) const { return &p[(size_t(y) * size_t(w) + size_t(x)) * 4]; }
};

float Clamp(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

// Catmull-Rom weights for the four samples around a position t (0 to 1) between the second and third
void Cubic(float t, float weights[4]) {
    weights[0] = ((-0.5f * t + 1.0f) * t - 0.5f) * t;
    weights[1] = (1.5f * t - 2.5f) * t * t + 1.0f;
    weights[2] = ((-1.5f * t + 2.0f) * t + 0.5f) * t;
    weights[3] = (0.5f * t - 0.5f) * t * t;
}

// a part of the launch screen (w x h at x, y) made dw x dh, bicubic; samples beyond the part's edge are the edge's, as a crop then scale does
Canvas Scale(const Bytes& rgb, const Piece& part, int dw, int dh) {
    Canvas out(dw, dh);
    for (int y = 0; y < dh; y++) {
        const float sy = (float(y) + 0.5f) * float(part.h) / float(dh) - 0.5f;
        const int iy = int(std::floor(sy));
        float wy[4];
        Cubic(sy - float(iy), wy);
        for (int x = 0; x < dw; x++) {
            const float sx = (float(x) + 0.5f) * float(part.w) / float(dw) - 0.5f;
            const int ix = int(std::floor(sx));
            float wx[4];
            Cubic(sx - float(ix), wx);
            float sum[3] = {0, 0, 0};
            for (int j = 0; j < 4; j++) {
                const int py = part.y + std::min(std::max(iy - 1 + j, 0), part.h - 1);
                for (int i = 0; i < 4; i++) {
                    const int px = part.x + std::min(std::max(ix - 1 + i, 0), part.w - 1);
                    const uint8_t* s = &rgb[(size_t(py) * kLaunchWidth + size_t(px)) * 3];
                    const float weight = wy[j] * wx[i];
                    for (int k = 0; k < 3; k++) sum[k] += weight * float(s[k]);
                }
            }
            float* d = out.At(x, y);
            for (int k = 0; k < 3; k++) d[k] = Clamp(sum[k], 0.0f, 255.0f);
            d[3] = 255.0f;
        }
    }
    return out;
}

// a gaussian blur of one channel (c) of a picture, edge samples repeated
void Blur(Canvas& image, int c, double sigma) {
    const int radius = std::max(1, int(std::ceil(sigma * 3.0)));
    std::vector<float> kernel(size_t(radius) * 2 + 1);
    float total = 0;
    for (int i = -radius; i <= radius; i++) total += kernel[size_t(i + radius)] = float(std::exp(-double(i) * double(i) / (2.0 * sigma * sigma)));
    for (float& k : kernel) k /= total;
    std::vector<float> line(size_t(std::max(image.w, image.h)));
    for (int pass = 0; pass < 2; pass++) {
        const int length = pass == 0 ? image.w : image.h, lines = pass == 0 ? image.h : image.w;
        for (int l = 0; l < lines; l++) {
            auto at = [&](int i) { return pass == 0 ? image.At(i, l) : image.At(l, i); };
            for (int i = 0; i < length; i++) {
                float sum = 0;
                for (int k = -radius; k <= radius; k++) sum += kernel[size_t(k + radius)] * at(std::min(std::max(i + k, 0), length - 1))[c];
                line[size_t(i)] = sum;
            }
            for (int i = 0; i < length; i++) at(i)[c] = line[size_t(i)];
        }
    }
}

// `colour` over the canvas at (x, y) wherever `alpha` (0 to 1) says
void Over(Canvas& canvas, int x0, int y0, int w, int h, const std::function<void(int, int, float rgb[3], float& alpha)>& pixel) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int cx = x0 + x, cy = y0 + y;
            if (cx < 0 || cy < 0 || cx >= canvas.w || cy >= canvas.h) continue;
            float rgb[3], alpha = 0;
            pixel(x, y, rgb, alpha);
            float* d = canvas.At(cx, cy);
            for (int k = 0; k < 3; k++) d[k] = rgb[k] * alpha + d[k] * (1.0f - alpha);
        }
}

// the composition of make_icon.py, over the whole square: the sky (a piece of the launch screen, blurred and darkened), the moon cut out as
// a circle, and the name (near white, found by its green) with a blurred shadow
Canvas ComposeIcon(const Bytes& launch) {
    const double s = double(kCanvas) / kShape;
    auto px = [&](double v) { return int(std::lround(v * s)); };
    Canvas icon = Scale(launch, kSky, kCanvas, kCanvas);
    for (int c = 0; c < 3; c++) Blur(icon, c, 8.0 * s);
    for (size_t i = 0; i < icon.p.size(); i += 4)
        for (int k = 0; k < 3; k++) icon.p[i + size_t(k)] *= float(kSkyBrightness);

    const int moon = px(kMoonSize);
    Canvas moon_picture = Scale(launch, kMoon, moon, moon);
    const float radius = float(moon) / 2.0f, edge = float(s);
    Over(icon, px(kMoonX), px(kMoonY), moon, moon, [&](int x, int y, float rgb[3], float& alpha) {
        const float* p = moon_picture.At(x, y);
        std::copy(p, p + 3, rgb);
        const float distance = std::hypot(float(x) + 0.5f - radius, float(y) + 0.5f - radius);
        alpha = Clamp((radius - 3.0f * edge - distance) / (4.0f * edge), 0.0f, 1.0f);
    });

    const int logo_w = px(kLogoWidth), logo_h = logo_w * kLogo.h / kLogo.w;
    Canvas name = Scale(launch, kLogo, logo_w, logo_h);
    auto name_alpha = [&](int x, int y) { return Clamp((name.At(x, y)[1] - 70.0f) / 50.0f, 0.0f, 1.0f); };
    // the shadow: black, as opaque as the name is, blurred
    const double sigma = kLogoShadow * s;
    const int pad = int(std::ceil(sigma * 3.0));
    Canvas shadow(logo_w + 2 * pad, logo_h + 2 * pad);
    for (int y = 0; y < logo_h; y++)
        for (int x = 0; x < logo_w; x++) shadow.At(x + pad, y + pad)[3] = name_alpha(x, y);
    Blur(shadow, 3, sigma);
    Over(icon, px(kLogoX) - pad, px(kLogoY) - pad, shadow.w, shadow.h, [&](int x, int y, float rgb[3], float& alpha) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        alpha = Clamp(shadow.At(x, y)[3], 0.0f, 1.0f);
    });
    Over(icon, px(kLogoX), px(kLogoY), logo_w, logo_h, [&](int x, int y, float rgb[3], float& alpha) {
        const float* p = name.At(x, y);
        std::copy(p, p + 3, rgb);
        alpha = name_alpha(x, y);
    });
    for (size_t i = 3; i < icon.p.size(); i += 4) icon.p[i] = 255.0f;
    return icon;
}

// the picture made d x d, each pixel the average of the part of the original it covers
Canvas Smaller(const Canvas& src, int d) {
    const double step = double(src.w) / double(d);
    Canvas across(d, src.h);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < d; x++) {
            const double from = x * step, to = (x + 1) * step;
            double sum[4] = {0, 0, 0, 0}, weight = 0;
            for (int i = int(from); i < src.w && double(i) < to; i++) {
                const double w = std::min(to, double(i + 1)) - std::max(from, double(i));
                const float* p = src.At(i, y);
                for (int k = 0; k < 4; k++) sum[k] += w * double(p[k]);
                weight += w;
            }
            for (int k = 0; k < 4; k++) across.At(x, y)[k] = float(sum[k] / weight);
        }
    Canvas out(d, d);
    for (int y = 0; y < d; y++)
        for (int x = 0; x < d; x++) {
            const double from = y * step, to = (y + 1) * step;
            double sum[4] = {0, 0, 0, 0}, weight = 0;
            for (int i = int(from); i < across.h && double(i) < to; i++) {
                const double w = std::min(to, double(i + 1)) - std::max(from, double(i));
                const float* p = across.At(x, i);
                for (int k = 0; k < 4; k++) sum[k] += w * double(p[k]);
                weight += w;
            }
            for (int k = 0; k < 4; k++) out.At(x, y)[k] = float(sum[k] / weight);
        }
    return out;
}

Bytes Rgba8(const Canvas& c) {
    Bytes out(c.p.size());
    for (size_t i = 0; i < out.size(); i++) out[i] = uint8_t(std::lround(Clamp(c.p[i], 0.0f, 255.0f)));
    return out;
}

void Put16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void Put32(Bytes& b, uint32_t v) { Put16(b, uint16_t(v)); Put16(b, uint16_t(v >> 16)); }

// an icon file with the sizes: a size of 256 or more is a PNG (Windows reads that since Vista), the others a 32 bit DIB with its
// (empty) mask, as every Windows reads
Bytes MakeIco(const Canvas& master, const std::vector<int>& sizes) {
    std::vector<Bytes> images;
    for (int d : sizes) {
        Canvas c = d == master.w ? master : Smaller(master, d);
        Bytes rgba = Rgba8(c);
        if (d >= 256) {
            images.push_back(EncodePng(d, d, 4, rgba.data()));
            if (images.back().empty()) return {};  // the caller sees no bytes
            continue;
        }
        Bytes dib;
        Put32(dib, 40);
        Put32(dib, uint32_t(d));
        Put32(dib, uint32_t(d * 2));  // the height counts the mask
        Put16(dib, 1);
        Put16(dib, 32);
        for (int i = 0; i < 6; i++) Put32(dib, 0);
        for (int y = d - 1; y >= 0; y--)  // bottom up, blue green red alpha
            for (int x = 0; x < d; x++) {
                const uint8_t* p = &rgba[(size_t(y) * size_t(d) + size_t(x)) * 4];
                dib.push_back(p[2]);
                dib.push_back(p[1]);
                dib.push_back(p[0]);
                dib.push_back(p[3]);
            }
        dib.insert(dib.end(), size_t((d + 31) / 32 * 4) * size_t(d), 0);
        images.push_back(std::move(dib));
    }
    Bytes ico;
    Put16(ico, 0);
    Put16(ico, 1);
    Put16(ico, uint16_t(sizes.size()));
    uint32_t offset = uint32_t(6 + 16 * sizes.size());
    for (size_t i = 0; i < sizes.size(); i++) {
        ico.push_back(uint8_t(sizes[i] >= 256 ? 0 : sizes[i]));  // 0 means 256
        ico.push_back(uint8_t(sizes[i] >= 256 ? 0 : sizes[i]));
        ico.push_back(0);
        ico.push_back(0);
        Put16(ico, 1);
        Put16(ico, 32);
        Put32(ico, uint32_t(images[i].size()));
        Put32(ico, offset);
        offset += uint32_t(images[i].size());
    }
    for (const Bytes& image : images) ico.insert(ico.end(), image.begin(), image.end());
    return ico;
}

}  // namespace

bool Make(const fs::path& thumbnails, const fs::path& launch, const fs::path& out, std::string& why) {
    Bytes mtx, xpr, screen;
    if (!ReadAll(thumbnails, mtx)) {
        why = "cannot read " + thumbnails.string();
        return false;
    }
    if (!ReadAll(launch, xpr)) {
        why = "cannot read " + launch.string();
        return false;
    }
    std::map<std::string, uint32_t> offsets;
    if (!ThumbnailTable(mtx, offsets, why) || !DecodeLaunch(xpr, screen, why)) return false;

    // each picture's source decoded once
    std::map<std::string, Picture> decoded;
    for (const Pick& pick : kPicks) {
        const std::string source = pick.source;
        if (decoded.count(source)) continue;
        Picture& picture = decoded[source];
        if (source == kLaunchSource) {
            picture.w = kLaunchWidth;
            picture.h = kLaunchHeight;
            picture.rgb = screen;
        } else {
            auto it = offsets.find(source);
            if (it == offsets.end()) {
                why = source + " is not in " + kThumbnails;
                return false;
            }
            if (size_t(it->second) + 0x8000 > mtx.size()) {
                why = source + " in " + kThumbnails + " lies beyond the end of the file";
                return false;
            }
            DecodeDxt5(&mtx[it->second], picture);
        }
    }

    std::error_code ec;
    fs::create_directories(out / "achievements", ec);
    if (ec) {
        why = "cannot make " + (out / "achievements").string() + ": " + ec.message();
        return false;
    }
    for (const Pick& pick : kPicks) {
        const Picture& picture = decoded[pick.source];
        const int left = pick.cx - pick.size / 2, top = pick.cy - pick.size / 2;
        if (left < 0 || top < 0 || top + pick.size > picture.h || left + pick.size > picture.w) {
            why = std::string(pick.id) + "'s crop does not fit in " + pick.source;
            return false;
        }
        Bytes crop(size_t(pick.size) * size_t(pick.size) * 3);
        for (int y = 0; y < pick.size; y++)
            std::memcpy(&crop[size_t(y) * size_t(pick.size) * 3], &picture.rgb[(size_t(top + y) * size_t(picture.w) + size_t(left)) * 3],
                        size_t(pick.size) * 3);
        const fs::path file = out / "achievements" / (std::string(pick.id) + ".png");
        if (!WriteAll(file, EncodePng(pick.size, pick.size, 3, crop.data()))) {
            why = "cannot write or encode " + file.string();
            return false;
        }
    }

    Canvas master = Smaller(ComposeIcon(screen), 256);
    if (!WriteAll(out / "Buffy.png", EncodePng(256, 256, 4, Rgba8(master).data()))) {
        why = "cannot write or encode " + (out / "Buffy.png").string();
        return false;
    }
    if (!WriteAll(out / "Buffy.ico", MakeIco(master, {256, 48, 32, 16}))) {
        why = "cannot write or encode " + (out / "Buffy.ico").string();
        return false;
    }
    return true;
}

}  // namespace pictures
