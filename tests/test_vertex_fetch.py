"""decode_attr, the vertex attribute fetch every translated vertex shader has (VERTEX_FETCH in
runtime/gpu_shader.c), against the version before it (kept below): the same four words for every
size code (the packed 10-10-10-2 one and unknown ones included), every type, both component
orders and every byte offset, on random data.

Both are compiled from their HLSL text as C++, with HLSL's types and intrinsics written out there
(uint4, ByteAddressBuffer, asuint, f16tof32, ...): what is compared is the HLSL itself.

(A rewrite without the array, as a [loop] over Load2, matched it here bit for bit yet drew wrong
and slower on an Intel UHD 770 under DXVK: it was put back. A next attempt must pass this and be
tried on that GPU.)"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
CXX = shutil.which('clang++') or shutil.which('g++') or shutil.which('c++')

# VERTEX_FETCH before the rewrite: an unrolled loop over five words in an array indexed at run time.
OLD = r'''uint4 decode_attr(ByteAddressBuffer b, uint addr, uint fmt) {
  uint size = (fmt >> 21) & 0x3Fu, type = (fmt >> 27) & 7u, n = 4u, bits = 32u;
  bool packed = false;
  switch (size) {
  case 0x01u: n = 4u; bits = 32u; break; case 0x02u: n = 3u; bits = 32u; break;
  case 0x03u: n = 4u; bits = 16u; break; case 0x04u: n = 2u; bits = 32u; break;
  case 0x05u: n = 3u; bits = 16u; break; case 0x0Au: n = 4u; bits = 8u; break;
  case 0x0Fu: n = 2u; bits = 16u; break; case 0x12u: n = 1u; bits = 32u; break;
  case 0x13u: n = 3u; bits = 8u; break; case 0x18u: n = 2u; bits = 8u; break;
  case 0x1Bu: n = 1u; bits = 16u; break; case 0x1Du: n = 1u; bits = 8u; break;
  case 0x32u: n = 2u; bits = 8u; break; case 0x33u: n = 4u; bits = 8u; break;
  case 0x34u: n = 1u; bits = 8u; break; case 0x30u: n = 4u; bits = 10u; packed = true; break;
  default: return uint4(0u, 0u, 0u, 0x3F800000u);
  }
  uint base = addr & ~3u, sh = (addr & 3u) * 8u;
  uint4 w = b.Load4(base); uint w4 = b.Load(base + 16u);
  uint words[5] = {w.x, w.y, w.z, w.w, w4};
  bool is_float = type == 7u || type == 1u || type == 2u || type == 5u || type == 6u;
  uint4 o = uint4(0u, 0u, 0u, is_float ? 0x3F800000u : 1u);
  [unroll] for (uint i = 0u; i < 4u; i++) {
    if (i >= n) break;
    uint cb = packed && i == 3u ? 2u : bits;
    uint bo = sh + (packed ? i * 10u : i * bits), wi = bo >> 5, s = bo & 31u;
    uint v = words[wi] >> s;
    if (s + cb > 32u) v |= words[wi + 1u] << (32u - s);
    if (cb < 32u) v &= (1u << cb) - 1u;
    int sv = cb < 32u ? ((int)(v << (32u - cb))) >> (32u - cb) : asint(v);
    float scale = cb < 32u ? (float)((1u << cb) - 1u) : 4294967295.0f;
    uint r = v;
    switch (type) {
    case 1u: r = asuint(max((float)sv / (scale * 0.5f - 0.5f), -1.0f)); break;
    case 2u: r = asuint((float)v / scale); break;
    case 3u: r = asuint(sv); break;
    case 5u: r = asuint((float)v); break;
    case 6u: r = asuint((float)sv); break;
    case 7u: r = cb == 16u ? asuint(f16tof32(v)) : v; break;
    }
    o[i] = r;
  }
  if ((fmt >> 31) != 0u) o = o.zyxw;
  return o;
}
'''

PRELUDE = r'''#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
typedef uint32_t uint;
struct uint2 { uint x, y; };
struct uint4 {
    uint x, y, z, w;
    uint4(uint a, uint b, uint c, uint d) : x(a), y(b), z(c), w(d) {}
    uint &operator[](uint i) { return i == 0 ? x : i == 1 ? y : i == 2 ? z : w; }
    uint4 zyxw() const { return uint4(z, y, x, w); }
};
struct ByteAddressBuffer {   /* out of range reads 0, as Direct3D's raw buffers */
    const std::vector<uint> *words;
    uint Load(uint a) const { return a / 4 < words->size() ? (*words)[a / 4] : 0u; }
    uint2 Load2(uint a) const { return uint2{Load(a), Load(a + 4)}; }
    uint4 Load4(uint a) const { return uint4(Load(a), Load(a + 4), Load(a + 8), Load(a + 12)); }
};
static uint asuint(float f) { uint u; memcpy(&u, &f, 4); return u; }
static uint asuint(int i) { return (uint)i; }
static int asint(uint u) { return (int)u; }
static float max(float a, float b) { return a > b ? a : b; }
static float f16tof32(uint h) {   /* the low 16 bits, exactly (denormals, infinities, NaNs kept) */
    uint s = (h >> 15 & 1) << 31, e = h >> 10 & 31, m = h & 0x3FF, u;
    if (e == 31) u = s | 0x7F800000u | m << 13;
    else if (e) u = s | (e + 112) << 23 | m << 13;
    else if (!m) u = s;
    else { e = 113; while (!(m & 0x400)) { m <<= 1; e--; } u = s | e << 23 | (m & 0x3FF) << 13; }
    float f; memcpy(&f, &u, 4); return f;
}
'''

MAIN = r'''
int main() {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&]() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return (uint)(state >> 32); };
    std::vector<uint> data(16);
    ByteAddressBuffer b{&data};
    long cases = 0, differ = 0, unknown_ok = 0, unknown = 0;
    for (uint size = 0; size < 64; size++)
        for (uint type = 0; type < 8; type++)
            for (uint bgra = 0; bgra < 2; bgra++)
                for (uint offset = 0; offset < 4; offset++)
                    for (int k = 0; k < 64; k++) {
                        for (auto &w : data) w = next();
                        if (k < 8) for (auto &w : data) w = k & 1 ? 0xFFFFFFFFu : k & 2 ? 0x80000000u : k & 4 ? 0x7C00FC00u : 0u;
                        uint fmt = size << 21 | type << 27 | bgra << 31 | (next() & 0x1FFFFF);
                        uint addr = 4 * (next() % 4) + offset;
                        uint4 a = decode_old(b, addr, fmt), c = decode_new(b, addr, fmt);
                        cases++;
                        if (a.x != c.x || a.y != c.y || a.z != c.z || a.w != c.w) {
                            if (differ++ < 5)
                                printf("DIFFER size %#x type %u bgra %u addr %u: old %08x %08x %08x %08x, new %08x %08x %08x %08x\n", size, type, bgra, addr,
                                       a.x, a.y, a.z, a.w, c.x, c.y, c.z, c.w);
                        }
                        if (size == 0 || size == 0x3F) {
                            unknown++;
                            unknown_ok += c.x == 0 && c.y == 0 && c.z == 0 && c.w == 0x3F800000u;
                        }
                    }
    printf("%ld cases, %ld differ; unknown size codes give (0, 0, 0, 1.0f) in %ld of %ld\n", cases, differ, unknown_ok, unknown);
    return differ || unknown_ok != unknown;
}
'''


def current_hlsl():
    """VERTEX_FETCH as runtime/gpu_shader.c has it: the C string's text."""
    source = (ROOT / 'runtime/gpu_shader.c').read_text(encoding='utf-8')
    body = re.search(r'static const char VERTEX_FETCH\[\] =\n(.*?);\n', source, re.S).group(1)
    return ''.join(bytes(m, 'utf-8').decode('unicode_escape') for m in re.findall(r'"((?:[^"\\]|\\.)*)"', body))


def as_cpp(hlsl, name):
    return re.sub(r'\[(?:unroll|loop)\]\s*', '', hlsl).replace('decode_attr', name).replace('.zyxw', '.zyxw()')


class VertexFetchTest(unittest.TestCase):
    @unittest.skipUnless(CXX, 'no C++ compiler')
    def test_same_results_as_before(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'fetch.cpp', Path(tmp) / 'fetch'
            src.write_text(PRELUDE + as_cpp(OLD, 'decode_old') + as_cpp(current_hlsl(), 'decode_new') + MAIN)
            r = subprocess.run([CXX, '-std=c++17', '-O1', '-ffp-contract=off', str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn(', 0 differ;', r.stdout)


if __name__ == '__main__':
    unittest.main()
