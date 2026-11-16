// Minimal math for the host. Conventions follow LithTech: left-handed, x right, y up, z forward; matrices are
// row-major with column vectors (v' = M v), translation in m[i][3].
#pragma once
#include <math.h>
#include <string.h>

struct V3 { float x = 0, y = 0, z = 0; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return sqrtf(dot(a, a)); }
inline V3 normalize(V3 a) { float l = length(a); return l > 1e-20f ? a * (1.0f / l) : V3{0, 1, 0}; }
inline V3 vmin(V3 a, V3 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z}; }
inline V3 vmax(V3 a, V3 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z}; }

struct M4 {
    float m[4][4];
    static M4 identity() { M4 r{}; r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1; return r; }
    V3 point(V3 p) const {
        return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
                m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
    }
    V3 dir(V3 p) const {
        return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z, m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z,
                m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z};
    }
};
inline M4 operator*(const M4 &a, const M4 &b) {
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}
inline M4 transpose(const M4 &a) { M4 r; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = a.m[j][i]; return r; }
inline M4 quatToMatrix(float x, float y, float z, float w) {
    M4 r = M4::identity();
    r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w); r.m[0][2] = 2 * (x * z + y * w);
    r.m[1][0] = 2 * (x * y + z * w); r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);
    r.m[2][0] = 2 * (x * z - y * w); r.m[2][1] = 2 * (y * z + x * w); r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}
// From a 3x4 row-major block (protocol node matrices).
inline M4 fromM34(const float *p) { M4 r = M4::identity(); memcpy(r.m, p, 48); return r; }

// General 4x4 inverse (cofactors).
inline M4 inverse(const M4 &a) {
    const float *m = &a.m[0][0];
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    M4 r = M4::identity();
    if (fabsf(det) < 1e-30f) return r;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) (&r.m[0][0])[i] = inv[i] * det;
    return r;
}
