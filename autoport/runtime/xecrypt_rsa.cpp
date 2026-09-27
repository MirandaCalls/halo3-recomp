// Portable XeCryptBnQwNeRsaPubCrypt.
//
// The SDK only implements this export on Windows (via BCrypt); elsewhere it
// returns success without writing any output, so games that verify content
// signatures (e.g. Halo: Reach map/save validation) fail their check and
// assert. This computes the raw RSA public operation b = a^e mod n.
//
// Number format ("BnQwNe"): an array of `size` 64-bit words, each stored
// big-endian in guest memory, least-significant word first.

#include <rex/hook.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using Limbs = std::vector<uint32_t>;  // little-endian 32-bit limbs

uint64_t LoadBE64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

void StoreBE64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; i--, v >>= 8) p[i] = static_cast<uint8_t>(v);
}

Limbs LoadBn(const uint8_t* p, uint32_t qwords) {
  Limbs out(qwords * 2);
  for (uint32_t i = 0; i < qwords; i++) {
    uint64_t q = LoadBE64(p + i * 8);
    out[i * 2] = static_cast<uint32_t>(q);
    out[i * 2 + 1] = static_cast<uint32_t>(q >> 32);
  }
  return out;
}

void StoreBn(uint8_t* p, const Limbs& v, uint32_t qwords) {
  for (uint32_t i = 0; i < qwords; i++) {
    StoreBE64(p + i * 8, (uint64_t(v[i * 2 + 1]) << 32) | v[i * 2]);
  }
}

int Compare(const Limbs& a, const Limbs& b) {
  for (size_t i = a.size(); i-- > 0;) {
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

void SubInPlace(Limbs& a, const Limbs& b) {
  int64_t borrow = 0;
  for (size_t i = 0; i < a.size(); i++) {
    int64_t d = int64_t(a[i]) - b[i] - borrow;
    borrow = d < 0;
    a[i] = static_cast<uint32_t>(d);
  }
}

// (a * b) mod n, all of n's width. Schoolbook multiply, then bitwise reduction.
Limbs MulMod(const Limbs& a, const Limbs& b, const Limbs& n) {
  const size_t k = n.size();
  std::vector<uint32_t> prod(2 * k, 0);
  for (size_t i = 0; i < k; i++) {
    uint64_t carry = 0;
    for (size_t j = 0; j < k; j++) {
      uint64_t t = uint64_t(a[i]) * b[j] + prod[i + j] + carry;
      prod[i + j] = static_cast<uint32_t>(t);
      carry = t >> 32;
    }
    prod[i + k] = static_cast<uint32_t>(carry);
  }
  // r is one limb wider than n so the shift below never overflows.
  Limbs r(k + 1, 0);
  Limbs nn(n);
  nn.push_back(0);
  for (size_t bit = 2 * k * 32; bit-- > 0;) {
    uint32_t carry = (prod[bit / 32] >> (bit % 32)) & 1;
    for (size_t i = 0; i < r.size(); i++) {
      uint32_t next = r[i] >> 31;
      r[i] = (r[i] << 1) | carry;
      carry = next;
    }
    if (Compare(r, nn) >= 0) SubInPlace(r, nn);
  }
  r.pop_back();
  return r;
}

}  // namespace

// u32 XeCryptBnQwNeRsaPubCrypt(const u64* qw_a, u64* qw_b, const XECRYPT_RSA* rsa)
// XECRYPT_RSA: be32 size (qwords), be32 public_exponent, u64 reserved, modulus[size].
REX_HOOK_RAW(__imp__XeCryptBnQwNeRsaPubCrypt) {
  const uint8_t* rsa = base + ctx.r5.u32;
  const uint32_t size = (uint32_t(rsa[0]) << 24) | (rsa[1] << 16) | (rsa[2] << 8) | rsa[3];
  const uint32_t exponent = (uint32_t(rsa[4]) << 24) | (rsa[5] << 16) | (rsa[6] << 8) | rsa[7];
  if (size == 0 || size > 64) {  // up to 4096-bit keys
    ctx.r3.u64 = 0;
    return;
  }

  const Limbs n = LoadBn(rsa + 0x10, size);
  const Limbs a = LoadBn(base + ctx.r3.u32, size);
  if (Compare(a, n) >= 0) {
    ctx.r3.u64 = 0;
    return;
  }

  Limbs result(n.size(), 0);
  result[0] = 1;
  for (int bit = 31; bit >= 0; bit--) {
    result = MulMod(result, result, n);
    if ((exponent >> bit) & 1) result = MulMod(result, a, n);
  }

  StoreBn(base + ctx.r4.u32, result, size);
  ctx.r3.u64 = 1;
}
