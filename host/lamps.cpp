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
    lamps.clear();
    lampCells.clear(); lampIndices.clear();
    memset(&lampGrid, 0, sizeof(lampGrid));
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
    Log("lamps: %u point, %u spot, %u object-only, %u directional (ignored) from %s; grid %dx%dx%d, %zu cell entries", nPoint, nSpot, nObject, nDir,
        worldFile, lampGrid.dims[0], lampGrid.dims[1], lampGrid.dims[2], lampIndices.size());
}
