// The level's static lights. The engine never instantiates the editor's Light / DirLight / ObjectLight objects
// (CF_NORUNTIME): they exist only in the world FILE, baked into lightmaps and the model light grid. For ray-traced
// shadows the host reads them back from the game's own files, the way the engine finds them: through the -rez
// sources (archives or folders), a later source overriding an earlier one.
// Formats: blood2_recon (rezmgr / S_Object.cpp LoadObjects) and d_ren's world_file.d.
#include "renderer.h"
#include <stdio.h>
#include <algorithm>
#include <map>

namespace {

struct RezEntry { uint64_t pos, size; };
std::map<std::string, std::map<std::string, RezEntry>> g_rezIndex;

std::string upper(std::string s) { for (char &c : s) c = (char)toupper((unsigned char)c); return s; }

void walkRez(FILE *f, uint32_t pos, uint32_t size, const std::string &prefix, int depth, std::map<std::string, RezEntry> &index) {
    if (depth > 16 || !size || size > (64u << 20)) return;
    std::vector<uint8_t> data(size);
    _fseeki64(f, pos, SEEK_SET);
    if (fread(data.data(), 1, size, f) != size) return;
    size_t at = 0;
    auto dword = [&]() -> uint32_t { if (at + 4 > data.size()) { at = data.size() + 1; return 0; } uint32_t v; memcpy(&v, &data[at], 4); at += 4; return v; };
    auto cstr = [&]() { size_t s = at; while (at < data.size() && data[at]) at++; std::string r((const char *)&data[s], at - s); at++; return r; };
    while (at < data.size()) {
        uint32_t type = dword();
        if (type == 1) {
            uint32_t dpos = dword(), dsize = dword(); dword();
            std::string name = cstr();
            if (at > data.size()) break;
            walkRez(f, dpos, dsize, prefix + upper(name) + "\\", depth + 1, index);
        } else if (type == 0) {
            uint32_t rpos = dword(), rsize = dword(); dword(); dword();
            uint32_t fourcc = dword(), keys = dword();
            std::string name = cstr(); cstr();
            at += 4 * (size_t)keys;
            if (at > data.size() + 1) break;
            std::string ext;
            for (int s = 24; s >= 0; s -= 8) { char c = (char)(fourcc >> s); if (c > ' ') ext += c; }
            index[prefix + upper(name) + (ext.empty() ? "" : "." + upper(ext))] = RezEntry{rpos, rsize};
        } else break;
    }
}

const std::map<std::string, RezEntry> &rezIndex(const std::string &archive) {
    auto it = g_rezIndex.find(archive);
    if (it != g_rezIndex.end()) return it->second;
    auto &index = g_rezIndex[archive];
    if (FILE *f = fopen(archive.c_str(), "rb")) {
        uint8_t header[40] = {};
        fseek(f, 127, SEEK_SET);
        if (fread(header, 1, 40, f) == 40) {
            uint32_t rootPos, rootSize; memcpy(&rootPos, header + 4, 4); memcpy(&rootSize, header + 8, 4);
            walkRez(f, rootPos, rootSize, "", 0, index);
        }
        fclose(f);
    }
    return index;
}

bool readGameFile(const std::string &name, const std::string &rezSources, std::vector<uint8_t> &out) {
    std::vector<std::string> sources;
    size_t start = 0;
    while (start <= rezSources.size()) {
        size_t end = rezSources.find(';', start);
        if (end == std::string::npos) end = rezSources.size();
        if (end > start) sources.push_back(rezSources.substr(start, end - start));
        start = end + 1;
    }
    std::string key = upper(name);
    std::replace(key.begin(), key.end(), '/', '\\');
    for (size_t i = sources.size(); i-- > 0;) {
        const std::string &src = sources[i];
        DWORD attr = GetFileAttributesA(src.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (FILE *f = fopen((src + "\\" + name).c_str(), "rb")) {
                fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                out.resize(n); fread(out.data(), 1, n, f); fclose(f);
                return true;
            }
            continue;
        }
        const auto &index = rezIndex(src);
        auto it = index.find(key);
        if (it == index.end()) continue;
        FILE *f = fopen(src.c_str(), "rb");
        if (!f) continue;
        out.resize((size_t)it->second.size);
        _fseeki64(f, (long long)it->second.pos, SEEK_SET);
        bool ok = fread(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        if (ok) return true;
    }
    return false;
}

} // namespace

void Renderer::loadLamps(const char *worldFile, const char *rezSources) {
    lamps.clear(); fileLampCount = 0;
    if (!worldFile[0]) { Log("lamps: the world file name is unknown (no static lamp shadows)"); return; }
    std::vector<uint8_t> file;
    if (!readGameFile(worldFile, rezSources, file) || file.size() < 8) { Log("lamps: cannot read %s from [%s]", worldFile, rezSources); return; }
    uint32_t offset; memcpy(&offset, &file[4], 4);
    if (offset >= file.size()) return;
    const uint8_t *d = file.data() + offset; size_t n = file.size() - offset, at = 0;
    bool bad = false;
    auto take = [&](size_t bytes) -> const uint8_t * { if (at + bytes > n) { bad = true; return nullptr; } const uint8_t *p = d + at; at += bytes; return p; };
    auto u16 = [&]() { const uint8_t *p = take(2); uint16_t v = 0; if (p) memcpy(&v, p, 2); return v; };
    auto u32 = [&]() { const uint8_t *p = take(4); uint32_t v = 0; if (p) memcpy(&v, p, 4); return v; };
    auto str = [&]() { uint16_t len = u16(); const uint8_t *p = take(len); return p ? std::string((const char *)p, len) : std::string(); };

    uint32_t count = u32(), nPoint = 0, nSpot = 0, nObject = 0, nDir = 0;
    for (uint32_t o = 0; o < count && !bad; o++) {
        uint16_t length = u16();
        size_t start = at;
        std::string cls = str();
        int kind = cls == "Light" ? 0 : cls == "DirLight" ? 1 : cls == "ObjectLight" ? 2 : cls == "GlobalDirLight" ? 3 : -1;
        float pos[3] = {}, color[3] = {255, 255, 255}, rot[3] = {}, radius = 300, bright = 1, fov = 90;
        bool clip = true, lightObjects = true;
        uint32_t props = u32();
        for (uint32_t p = 0; p < props && !bad; p++) {
            std::string name = str();
            const uint8_t *tp = take(1); uint8_t type = tp ? *tp : 0;
            u32();
            uint16_t vlen = u16();
            if (type == 0) { str(); continue; }        // PT_STRING
            const uint8_t *v = take(vlen);
            if (!v || kind < 0) continue;
            auto F = [&](size_t i) { float f = 0; if (vlen >= (i + 1) * 4) memcpy(&f, v + i * 4, 4); return f; };
            if (name == "Pos") { pos[0] = F(0); pos[1] = F(1); pos[2] = F(2); }
            else if (name == "LightColor" || name == "InnerColor") { color[0] = F(0); color[1] = F(1); color[2] = F(2); }
            else if (name == "LightRadius") radius = F(0);
            else if (name == "BrightScale") bright = F(0);
            else if (name == "Rotation") { rot[0] = F(0); rot[1] = F(1); rot[2] = F(2); }
            else if (name == "FOV") fov = F(0);
            else if (name == "ClipLight") clip = vlen && v[0];
            else if (name == "LightObjects") lightObjects = vlen && v[0];
        }
        at = start + length;        // the stored length is authoritative
        if (kind < 0 || bad) continue;
        if (kind == 3) { nDir++; continue; }            // GlobalDirLight: not handled yet (docs/STATUS.md)
        GLamp l = {};
        l.posRadius[0] = pos[0]; l.posRadius[1] = pos[1]; l.posRadius[2] = pos[2]; l.posRadius[3] = radius;
        for (int i = 0; i < 3; i++) l.color[i] = color[i] * bright / 255.0f;
        // flags in color.w: 1 = lit the world (lightmaps), 2 = lit models, 4 = its light was clipped by geometry in the bake
        l.color[3] = (float)((kind != 2 ? 1 : 0) | (lightObjects ? 2 : 0) | (clip ? 4 : 0));
        l.spot[3] = -2.0f;
        if (kind == 1) {
            // Euler pitch (x), yaw (y) as the engine's GetPropRotation reads them: the light looks down its +z
            float cp = cosf(rot[0]), sp = sinf(rot[0]), cy = cosf(rot[1]), sy = sinf(rot[1]);
            l.spot[0] = sy * cp; l.spot[1] = -sp; l.spot[2] = cy * cp;
            l.spot[3] = cosf(fov * 0.5f * 3.14159265f / 180.0f);
            nSpot++;
        } else if (kind == 2) nObject++; else nPoint++;
        if (radius > 1 && lamps.size() < kMaxLamps) lamps.push_back(l);
    }

    fileLampCount = lamps.size();
    Log("lamps: %u point, %u spot, %u object-only, %u directional (ignored) from %s", nPoint, nSpot, nObject, nDir, worldFile);
}

void Renderer::buildLampGrid() {
    lampCells.clear(); lampIndices.clear();
    memset(&lampGrid, 0, sizeof(lampGrid));
    // ---- uniform grid over the lamps' reach, so a pixel only walks the lamps that can touch it
    if (!lamps.empty()) {
        V3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
        for (auto &l : lamps) {
            V3 c{l.posRadius[0], l.posRadius[1], l.posRadius[2]}, r{l.posRadius[3], l.posRadius[3], l.posRadius[3]};
            mn = vmin(mn, c - r); mx = vmax(mx, c + r);
        }
        float ext[3] = {mx.x - mn.x, mx.y - mn.y, mx.z - mn.z};
        float cell = std::max({ext[0], ext[1], ext[2]}) / 48.0f;
        cell = std::max(cell, 128.0f);
        int dims[3];
        for (int i = 0; i < 3; i++) dims[i] = std::clamp((int)ceilf(ext[i] / cell), 1, 48);
        lampGrid.origin[0] = mn.x; lampGrid.origin[1] = mn.y; lampGrid.origin[2] = mn.z; lampGrid.origin[3] = cell;
        lampGrid.dims[0] = dims[0]; lampGrid.dims[1] = dims[1]; lampGrid.dims[2] = dims[2];
        std::vector<std::vector<uint32_t>> cells((size_t)dims[0] * dims[1] * dims[2]);
        for (uint32_t i = 0; i < lamps.size(); i++) {
            const float *pr = lamps[i].posRadius;
            int lo[3], hi[3];
            for (int a = 0; a < 3; a++) {
                lo[a] = std::clamp((int)floorf((pr[a] - pr[3] - (&mn.x)[a]) / cell), 0, dims[a] - 1);
                hi[a] = std::clamp((int)floorf((pr[a] + pr[3] - (&mn.x)[a]) / cell), 0, dims[a] - 1);
            }
            for (int z = lo[2]; z <= hi[2]; z++) for (int y = lo[1]; y <= hi[1]; y++) for (int x = lo[0]; x <= hi[0]; x++)
                cells[((size_t)z * dims[1] + y) * dims[0] + x].push_back(i);
        }
        lampCells.resize(cells.size() + 1);
        for (size_t c = 0; c < cells.size(); c++) {
            lampCells[c] = (uint32_t)lampIndices.size();
            lampIndices.insert(lampIndices.end(), cells[c].begin(), cells[c].end());
        }
        lampCells[cells.size()] = (uint32_t)lampIndices.size();
        lampGrid.count = (uint32_t)lamps.size();
    }
    if (lamps.empty()) return;
    memcpy(lampBuf.mapped, lamps.data(), lamps.size() * sizeof(GLamp));
    uint32_t base = (uint32_t)lampCells.size();
    uint32_t *ld = (uint32_t *)lampDataBuf.mapped;
    if (lampCells.size() + lampIndices.size() <= lampDataBuf.size / 4) {
        for (size_t i = 0; i < lampCells.size(); i++) ld[i] = lampCells[i] + base;
        memcpy(ld + base, lampIndices.data(), lampIndices.size() * 4);
    } else { Log("lamp grid too large: lamps off"); lampGrid.count = 0; }
    Log("lamp grid: %zu lamps (%zu from the level file, %zu emitters), %dx%dx%d cells, %zu entries", lamps.size(), fileLampCount,
        lamps.size() - fileLampCount, lampGrid.dims[0], lampGrid.dims[1], lampGrid.dims[2], lampIndices.size());
}

// Surfaces the level marks as bright -- the editor's "bright" surface tag, which the preprocessor resolves into a fully
// saturated lightmap, and fullbright texels (palette entries 246..255 of DTX_FULLBRITE textures) -- are self-lit in the
// original but light nothing. Here each one becomes a light: a cosine-lobed lamp in front of the polygon, coloured by
// what the texture shows, sized by the polygon. They then go through the same shadow-ray and bounce code as the lamps.
void Renderer::buildEmitters() {
    lamps.resize(fileLampCount);
    float strength = settings.giEmissive > 0 ? settings.giEmissive : 0.0f;
    struct Cand { float weight; GLamp lamp; };
    std::vector<Cand> cands;
    size_t skipped = 0;
    for (const EmitPoly &e : emitPolys) {
        uint32_t tex = e.surface < surfaces.size() ? surfaces[e.surface].tex : 0;
        if (!tex || tex >= texStats.size()) continue;
        const TexStats &ts = texStats[tex];
        float col[3];
        if (e.bright) for (int i = 0; i < 3; i++) col[i] = ts.avg[i];
        else if (ts.fbCoverage > 0.02f) for (int i = 0; i < 3; i++) col[i] = ts.fbAvg[i] * std::min(ts.fbCoverage * 3.0f, 1.0f);
        else continue;
        float lum = col[0] * 0.299f + col[1] * 0.587f + col[2] * 0.114f;
        if (lum < 0.06f || e.area < 48.0f) continue;
        // A glowing panel the level designer already gave a lamp (ceiling lights) must not shine twice.
        bool hasLamp = false;
        float reach = sqrtf(e.area) + 96.0f;
        for (size_t i = 0; i < fileLampCount && !hasLamp; i++) {
            V3 d = V3{lamps[i].posRadius[0], lamps[i].posRadius[1], lamps[i].posRadius[2]} - e.center;
            hasLamp = dot(d, d) < reach * reach && dot(d, e.normal) > -8.0f;
        }
        if (hasLamp) { skipped++; continue; }
        GLamp l = {};
        V3 p = e.center + e.normal * 4.0f;
        float size = sqrtf(e.area);
        l.posRadius[0] = p.x; l.posRadius[1] = p.y; l.posRadius[2] = p.z;
        l.posRadius[3] = std::clamp(size * 5.0f, 140.0f, 640.0f);
        float gain = strength * std::clamp(size / 48.0f, 0.5f, 1.6f);
        for (int i = 0; i < 3; i++) l.color[i] = col[i] * gain;
        l.color[3] = 3.0f;                              // lights the world and models
        l.spot[0] = e.normal.x; l.spot[1] = e.normal.y; l.spot[2] = e.normal.z;       // lampAt: cone axis = direction the lamp shines in
        l.spot[3] = 0.0f;                               // cosine lobe over the front hemisphere
        cands.push_back(Cand{lum * e.area, l});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.weight > b.weight; });
    size_t room = kMaxLamps > lamps.size() ? kMaxLamps - lamps.size() : 0;
    for (size_t i = 0; i < cands.size() && i < room && i < 900; i++) lamps.push_back(cands[i].lamp);
    emittersBuilt = true; emittersStrength = settings.giEmissive;
    Log("emitters: %zu candidates from %zu polygons, %zu used, %zu skipped (a level lamp is already there)", cands.size(), emitPolys.size(),
        lamps.size() - fileLampCount, skipped);
    buildLampGrid();
}

// The world file gives each lamp a colour, BrightScale and radius, but how the original light preprocessor turned
// those into lightmap values is not known exactly (an open item in the reconstruction). The level's own lightmaps are
// the ground truth: over all lightmapped polygons of the main world, compare the bake's result with the unshadowed sum
// of the lamps reaching the polygon (the bake's formula: one-sided, linear falloff, cone) and scale every file lamp by
// the ratio found where the bake is least occluded (an upper percentile).
void Renderer::calibrateLamps() {
    lampCalibration = 1.0f;
    if (lamps.empty() || emitPolys.empty()) return;
    std::vector<float> ratios;
    for (size_t pi = 0; pi < emitPolys.size(); pi += 1 + emitPolys.size() / 6000) {
        const EmitPoly &e = emitPolys[pi];
        if (!e.hasLm || e.area < 256.0f) continue;
        float sum = 0;
        for (size_t i = 0; i < fileLampCount && i < lamps.size(); i++) {
            const GLamp &l = lamps[i];
            if (!((uint32_t)l.color[3] & 1u)) continue;
            V3 to = V3{l.posRadius[0], l.posRadius[1], l.posRadius[2]} - e.center;
            float d = length(to);
            if (d >= l.posRadius[3] || dot(to, e.normal) <= 0) continue;
            float f = 1.0f - d / l.posRadius[3];
            if (l.spot[3] > -1.5f) {
                float c = d > 0.001f ? -(l.spot[0] * to.x + l.spot[1] * to.y + l.spot[2] * to.z) / d : 1.0f;
                f *= std::max((c - l.spot[3]) / (1.0f - l.spot[3]), 0.0f);
            }
            sum += (l.color[0] * 0.299f + l.color[1] * 0.587f + l.color[2] * 0.114f) * f;
        }
        float baked = std::max(e.lm[0] - levelAmbient[0], 0.0f) * 0.299f + std::max(e.lm[1] - levelAmbient[1], 0.0f) * 0.587f +
                      std::max(e.lm[2] - levelAmbient[2], 0.0f) * 0.114f;
        // saturated lightmaps say nothing about the ratio (the bake clamped there)
        if (sum > 0.08f && e.lm[1] < 0.95f) ratios.push_back(baked / sum);
    }
    if (ratios.size() < 30) { Log("lamp calibration: only %zu samples, lamps left as they are", ratios.size()); return; }
    std::sort(ratios.begin(), ratios.end());
    float med = ratios[ratios.size() / 2], hi = ratios[ratios.size() * 8 / 10];
    // The median: occluded samples pull the ratio down, samples the bake lit by bounce-less overlap pull it up. A value
    // near 1 (0.93 in 01_SubwayA) also confirms the bake formula itself, cosine-free.
    lampCalibration = std::clamp(med, 0.05f, 2.0f);
    for (size_t i = 0; i < fileLampCount && i < lamps.size(); i++)
        for (int k = 0; k < 3; k++) lamps[i].color[k] *= lampCalibration;
    Log("lamp calibration against the lightmaps: %zu samples, ratio median %.3f (80th percentile %.3f) -> lamps x %.3f", ratios.size(), med, hi,
        lampCalibration);
}
