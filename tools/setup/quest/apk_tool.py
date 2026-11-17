"""apk_tool.py -- the APK steps of the player's Quest build without Java or the Android build tools (ac0oculus installer, 2026-11-17):
an aligned zip and an APK Signature Scheme v2 signature, in the Python standard library only.

  python -I apk_tool.py pack BASE.apk OUT.apk ARCNAME=FILE ...   BASE's entries as they are, then each FILE STORED as ARCNAME; every
                                                                 STORED entry aligned (.so to 16 KB, the rest to 4 bytes), as zipalign
                                                                 -P 16 -f 4 does; nothing compressed again
  python -I apk_tool.py keygen KEY.json [NAME]                   a 2048-bit RSA key and its self-signed certificate (CN=NAME), once
  python -I apk_tool.py sign IN.apk OUT.apk KEY.json             a v2 signature block (RSASSA-PKCS1-v1_5, SHA-256; minSdk 32 needs
                                                                 neither v1 nor v3)
  python -I apk_tool.py verify APK [KEY.json]                    the v2 block checked: digest, signature, certificate's key; with KEY,
                                                                 also that it is that key's
  python -I apk_tool.py libs APK                                 the lib/ entries: method, size, data offset, offset % 16384

The format: source.android.com/docs/security/features/apksigning/v2 (the signing block between the entries and the central directory;
1 MB chunk digests of the entries, the central directory and the end record with the directory's offset as before the block).
The key file holds a private key: it stays on the player's PC, outside the install folder, so that later builds can be installed over
earlier ones (Android refuses an update signed by another key)."""
import base64
import datetime
import hashlib
import json
import math
import os
import secrets
import struct
import sys
import zipfile

ALIGN_SO = 16384
ALIGN_OTHER = 4
ALG_RSA_PKCS1_SHA256 = 0x0103
V2_BLOCK_ID = 0x7109871A
MAGIC = b'APK Sig Block 42'
DIGEST_INFO_SHA256 = bytes.fromhex('3031300d060960864801650304020105000420')


# ---------------------------------------------------------------- zip

def dos_time(dt):
    y, mo, d, h, mi, s = dt
    return ((h << 11) | (mi << 5) | (s // 2)), (((y - 1980) << 9) | (mo << 5) | d)


def raw_entries(path):
    """(ZipInfo, raw compressed bytes) of every entry of an existing zip, in its order"""
    out = []
    with zipfile.ZipFile(path) as z, open(path, 'rb') as f:
        for zi in z.infolist():
            f.seek(zi.header_offset)
            h = f.read(30)
            if h[:4] != b'PK\x03\x04':
                raise SystemExit('apk_tool: bad local header for %s in %s' % (zi.filename, path))
            n, e = struct.unpack('<HH', h[26:30])
            f.seek(zi.header_offset + 30 + n + e)
            out.append((zi, f.read(zi.compress_size)))
    return out


def write_zip(out_path, items):
    """items: (name, method 0|8, crc, usize, raw bytes, date_time, external_attr). STORED data aligned; no data descriptors."""
    cd = []
    with open(out_path, 'wb') as f:
        for name, method, crc, usize, raw, dt, ext_attr in items:
            nb = name.encode('utf-8')
            off = f.tell()
            extra = b''
            if method == 0:
                align = ALIGN_SO if name.endswith('.so') else ALIGN_OTHER
                base = off + 30 + len(nb) + 6                    # (the 0xd935 record: id, size, alignment, then the padding)
                pad = (-base) % align
                extra = struct.pack('<HHH', 0xD935, 2 + pad, align) + b'\0' * pad
            t, d = dos_time(dt)
            flags = 0x0800 if any(c > 127 for c in nb) else 0
            f.write(struct.pack('<IHHHHHIIIHH', 0x04034B50, 20, flags, method, t, d, crc, len(raw), usize, len(nb), len(extra)))
            f.write(nb)
            f.write(extra)
            f.write(raw)
            cd.append(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014B50, 20, 20, flags, method, t, d, crc, len(raw), usize, len(nb), 0, 0,
                                  0, 0, ext_attr, off) + nb)
        cd_off = f.tell()
        cdb = b''.join(cd)
        f.write(cdb)
        f.write(struct.pack('<IHHHHIIH', 0x06054B50, 0, 0, len(items), len(items), len(cdb), cd_off, 0))


def cmd_pack(base, out, adds):
    items = []
    names = set()
    for zi, raw in raw_entries(base):
        items.append((zi.filename, zi.compress_type, zi.CRC, zi.file_size, raw, zi.date_time, zi.external_attr))
        names.add(zi.filename)
    for kv in adds:
        arc, path = kv.split('=', 1)
        if arc in names:
            raise SystemExit('apk_tool: %s is already in %s' % (arc, base))
        with open(path, 'rb') as fp:
            data = fp.read()
        import zlib
        items.append((arc, 0, zlib.crc32(data) & 0xFFFFFFFF, len(data), data, (2008, 1, 1, 0, 0, 0), 0o644 << 16))
        names.add(arc)
    write_zip(out, items)
    return 0


def cmd_libs(apk):
    bad = 0
    with zipfile.ZipFile(apk) as z, open(apk, 'rb') as f:
        for zi in z.infolist():
            f.seek(zi.header_offset)
            h = f.read(30)
            n, e = struct.unpack('<HH', h[26:30])
            off = zi.header_offset + 30 + n + e
            stored = zi.compress_type == 0
            if zi.filename.startswith('lib/'):
                ok = stored and off % ALIGN_SO == 0
                bad += not ok
                print('%s %s %d %d %d' % (zi.filename, 'stored' if stored else 'deflated', zi.file_size, off, off % ALIGN_SO))
            elif stored and off % ALIGN_OTHER:
                bad += 1
                print('%s: stored, data at %d is not 4-byte aligned' % (zi.filename, off))
    return 1 if bad else 0


# ---------------------------------------------------------------- RSA and DER

def is_probable_prime(n, rounds=40):
    if n < 4:
        return n in (2, 3)
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(rounds):
        a = secrets.randbelow(n - 3) + 2
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def gen_prime(bits, e):
    while True:
        c = secrets.randbits(bits) | (1 << (bits - 1)) | (1 << (bits - 2)) | 1
        if math.gcd(c - 1, e) == 1 and is_probable_prime(c):
            return c


def der(tag, body):
    n = len(body)
    if n < 0x80:
        ln = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
        ln = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + ln + body


def der_int(v):
    b = v.to_bytes(max(1, (v.bit_length() + 8) // 8), 'big')       # (one more byte than needed: the sign bit stays 0)
    while len(b) > 1 and b[0] == 0 and b[1] < 0x80:
        b = b[1:]
    return der(0x02, b)


def der_oid(dotted):
    p = [int(x) for x in dotted.split('.')]
    out = bytes([40 * p[0] + p[1]])
    for v in p[2:]:
        enc = [v & 0x7F]
        v >>= 7
        while v:
            enc.insert(0, 0x80 | (v & 0x7F))
            v >>= 7
        out += bytes(enc)
    return der(0x06, out)


def seq(*parts):
    return der(0x30, b''.join(parts))


OID_RSA = '1.2.840.113549.1.1.1'
OID_SHA256_RSA = '1.2.840.113549.1.1.11'
OID_CN = '2.5.4.3'


def spki(n, e):
    return seq(seq(der_oid(OID_RSA), b'\x05\x00'), der(0x03, b'\x00' + seq(der_int(n), der_int(e))))


def pkcs1_sign(msg, n, d):
    k = (n.bit_length() + 7) // 8
    t = DIGEST_INFO_SHA256 + hashlib.sha256(msg).digest()
    em = b'\x00\x01' + b'\xff' * (k - len(t) - 3) + b'\x00' + t
    return pow(int.from_bytes(em, 'big'), d, n).to_bytes(k, 'big')


def pkcs1_verify(msg, sig, n, e):
    k = (n.bit_length() + 7) // 8
    t = DIGEST_INFO_SHA256 + hashlib.sha256(msg).digest()
    em = b'\x00\x01' + b'\xff' * (k - len(t) - 3) + b'\x00' + t
    return len(sig) == k and pow(int.from_bytes(sig, 'big'), e, n).to_bytes(k, 'big') == em


def make_cert(n, e, d, name):
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    until = now.replace(year=now.year + 30)
    nm = seq(der(0x31, seq(der_oid(OID_CN), der(0x0C, name.encode('utf-8')))))
    alg = seq(der_oid(OID_SHA256_RSA), b'\x05\x00')
    validity = seq(der(0x17, now.strftime('%y%m%d%H%M%SZ').encode()), der(0x18, until.strftime('%Y%m%d%H%M%SZ').encode()))
    tbs = seq(der(0xA0, der_int(2)), der_int(secrets.randbits(63) | 1), alg, nm, validity, nm, spki(n, e))
    return seq(tbs, alg, der(0x03, b'\x00' + pkcs1_sign(tbs, n, d)))


def cmd_keygen(path, name='ac0oculus debug'):
    if os.path.exists(path):
        print('apk_tool: %s exists: kept (a new key could not update the app already on the headset)' % path)
        return 0
    e = 65537
    while True:
        p, q = gen_prime(1024, e), gen_prime(1024, e)
        n = p * q
        if p != q and n.bit_length() == 2048:
            break
    d = pow(e, -1, math.lcm(p - 1, q - 1))
    cert = make_cert(n, e, d, name)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='ascii') as fp:
        json.dump({'format': 'ac0oculus-apk-key-1', 'n': '%x' % n, 'e': e, 'd': '%x' % d, 'cert': base64.b64encode(cert).decode()}, fp)
    os.replace(tmp, path)
    print('apk_tool: new signing key %s (certificate sha256 %s)' % (path, hashlib.sha256(cert).hexdigest()))
    return 0


def load_key(path):
    with open(path, encoding='ascii') as fp:
        k = json.load(fp)
    return int(k['n'], 16), int(k['e']), int(k['d'], 16), base64.b64decode(k['cert'])


# ---------------------------------------------------------------- APK Signature Scheme v2

def lp(b):
    return struct.pack('<I', len(b)) + b


def zip_sections(data):
    """(entries end = central directory offset, central directory size, EOCD offset); refuses a zip with a comment or zip64"""
    eocd = len(data) - 22
    if data[eocd:eocd + 4] != b'PK\x05\x06':
        raise SystemExit('apk_tool: no end record at the end (a zip comment is not supported)')
    cd_size, cd_off = struct.unpack('<II', data[eocd + 12:eocd + 20])
    if cd_off + cd_size != eocd:
        raise SystemExit('apk_tool: the central directory does not end at the end record')
    return cd_off, cd_size, eocd


def v2_digest(sections):
    chunks = []
    for s in sections:
        for i in range(0, len(s), 1 << 20):
            c = s[i:i + (1 << 20)]
            chunks.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(chunks)) + b''.join(chunks)).digest()


def cmd_sign(src, out, key_path):
    n, e, d, cert = load_key(key_path)
    with open(src, 'rb') as fp:
        data = fp.read()
    cd_off, cd_size, eocd = zip_sections(data)
    if data[cd_off - 16:cd_off] == MAGIC:
        raise SystemExit('apk_tool: %s is signed already' % src)
    entries, cdir, end = data[:cd_off], data[cd_off:eocd], data[eocd:]
    digest = v2_digest((entries, cdir, end))                       # (the end record names the directory where it is now: cd_off)
    signed_data = lp(lp(struct.pack('<I', ALG_RSA_PKCS1_SHA256) + lp(digest))) + lp(lp(cert)) + lp(b'')
    sig = pkcs1_sign(signed_data, n, d)
    signer = lp(signed_data) + lp(lp(struct.pack('<I', ALG_RSA_PKCS1_SHA256) + lp(sig))) + lp(spki(n, e))
    value = lp(lp(signer))
    pair = struct.pack('<QI', 4 + len(value), V2_BLOCK_ID) + value
    size = len(pair) + 8 + 16
    block = struct.pack('<Q', size) + pair + struct.pack('<Q', size) + MAGIC
    new_end = end[:16] + struct.pack('<I', cd_off + len(block)) + end[20:]
    tmp = out + '.tmp'
    with open(tmp, 'wb') as fp:
        fp.write(entries)
        fp.write(block)
        fp.write(cdir)
        fp.write(new_end)
    os.replace(tmp, out)
    print('apk_tool: signed %s (v2, certificate sha256 %s)' % (out, hashlib.sha256(cert).hexdigest()))
    return 0


def read_lp(b, i):
    (n,) = struct.unpack('<I', b[i:i + 4])
    return b[i + 4:i + 4 + n], i + 4 + n


def cmd_verify(apk, key_path=None):
    with open(apk, 'rb') as fp:
        data = fp.read()
    cd_off, cd_size, eocd = zip_sections(data)
    if data[cd_off - 16:cd_off] != MAGIC:
        print('verify: no APK signing block')
        return 1
    (size2,) = struct.unpack('<Q', data[cd_off - 24:cd_off - 16])
    start = cd_off - size2 - 8
    pairs = data[start + 8:cd_off - 24]
    i, v2 = 0, None
    while i < len(pairs):
        ln, pid = struct.unpack('<QI', pairs[i:i + 12])
        if pid == V2_BLOCK_ID:
            v2 = pairs[i + 12:i + 8 + ln]
        i += 8 + ln
    if v2 is None:
        print('verify: no v2 block')
        return 1
    signers, _ = read_lp(v2, 0)
    signer, _ = read_lp(signers, 0)
    signed_data, j = read_lp(signer, 0)
    sigs, j = read_lp(signer, j)
    pub, j = read_lp(signer, j)
    digests, k = read_lp(signed_data, 0)
    certs, k = read_lp(signed_data, k)
    d0, _ = read_lp(digests, 0)
    (alg,) = struct.unpack('<I', d0[:4])
    want, _ = read_lp(d0, 4)
    cert, _ = read_lp(certs, 0)
    s0, _ = read_lp(sigs, 0)
    sig, _ = read_lp(s0, 4)
    # the public key: the modulus and exponent inside the SubjectPublicKeyInfo (our own encoding: SEQ(SEQ(alg), BITSTRING(SEQ(n, e))))
    def ints(b):
        out, i = [], 0
        while i < len(b):
            tag, ln = b[i], b[i + 1]
            i += 2
            if ln & 0x80:
                nb = ln & 0x7F
                ln = int.from_bytes(b[i:i + nb], 'big')
                i += nb
            if tag == 0x02:
                out.append(int.from_bytes(b[i:i + ln], 'big'))
                i += ln
            elif tag in (0x30, 0x03):
                i += 1 if tag == 0x03 else 0
                continue
            else:
                i += ln
        return out
    n, e = ints(pub)[-2:]
    end = bytearray(data[eocd:])
    end[16:20] = struct.pack('<I', start)
    got = v2_digest((data[:start], data[cd_off:eocd], bytes(end)))
    ok = alg == ALG_RSA_PKCS1_SHA256 and got == want and pkcs1_verify(signed_data, sig, n, e) and spki(n, e) in cert
    msg = 'v2 digest %s, signature %s, certificate sha256 %s' % ('ok' if got == want else 'WRONG',
                                                               'ok' if pkcs1_verify(signed_data, sig, n, e) else 'WRONG',
                                                               hashlib.sha256(cert).hexdigest())
    if key_path:
        kn, ke, _, kcert = load_key(key_path)
        same = kn == n and ke == e and kcert == cert
        ok = ok and same
        msg += ', %s key' % ('the given' if same else 'ANOTHER')
    print('verify: %s: %s' % ('ok' if ok else 'FAILED', msg))
    return 0 if ok else 1


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    c, a = argv[1], argv[2:]
    if c == 'pack' and len(a) >= 2:
        return cmd_pack(a[0], a[1], a[2:])
    if c == 'keygen' and len(a) in (1, 2):
        return cmd_keygen(*a)
    if c == 'sign' and len(a) == 3:
        return cmd_sign(*a)
    if c == 'verify' and len(a) in (1, 2):
        return cmd_verify(*a)
    if c == 'libs' and len(a) == 1:
        return cmd_libs(a[0])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
