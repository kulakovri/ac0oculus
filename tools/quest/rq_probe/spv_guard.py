"""spv_guard.py SHADER_DIR [ALLOW_FILE]: the R-45 guard (outside note 6 item 5, 2026-11-10).

On the Quest 3's Adreno, inside a pass foveated by the runtime's density map, per-fragment position reads are misplaced in
reduced bins: an input-attachment read (subpassLoad) always (R-45c: the sun's glare through a DATE read, in a stage
WITHOUT specialization constants), and a gl_FragCoord-based read in a stage WITH specialization constants (R-45b: the
lean wet canopy's world read; the same read without constants is clean). This scans every fragment-stage SPIR-V of the
build and fails when a stage
  - reads a subpass input (OpImageRead of a SubpassData image), or
  - holds a specialization constant (OpSpecConstant*) and loads the FragCoord built-in,
unless the stage is listed in ALLOW_FILE ("name.frag.spv: reason", one a line, # comments) with the reason it never runs
in the foveated eye pass (another pass, a test switch, a path the Quest default does not take). Exit 1 on a failure,
2 on a stale allow entry (a listed stage that no longer trips the rule or no longer exists), 0 when clean.
Prints one line per tripping stage and a summary."""
import os, struct, sys

OP_TYPE_IMAGE, OP_VARIABLE, OP_LOAD, OP_ACCESS_CHAIN, OP_DECORATE, OP_IMAGE_READ = 25, 59, 61, 65, 71, 98
OP_SPEC = (48, 49, 50, 51, 52)            # OpSpecConstantTrue / False / OpSpecConstant / Composite / Op
DEC_BUILTIN, BI_FRAGCOORD, DIM_SUBPASS = 11, 15, 6

def scan(path):
    b = open(path, 'rb').read()
    if len(b) < 20 or len(b) % 4:
        raise ValueError('not SPIR-V')
    w = struct.unpack('<%dI' % (len(b) // 4), b)
    if w[0] != 0x07230203:
        raise ValueError('bad magic')
    spec = 0
    fragcoord_vars, subpass_types, ptr_to = set(), set(), {}
    var_type, loads = {}, []
    reads = []
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if n == 0:
            raise ValueError('zero-length instruction at word %d' % i)
        a = w[i + 1:i + n]
        if op in OP_SPEC:
            spec += 1
        elif op == OP_DECORATE and len(a) >= 3 and a[1] == DEC_BUILTIN and a[2] == BI_FRAGCOORD:
            fragcoord_vars.add(a[0])
        elif op == OP_TYPE_IMAGE and len(a) >= 3 and a[2] == DIM_SUBPASS:
            subpass_types.add(a[0])
        elif op == 32 and len(a) >= 3:          # OpTypePointer result, storage class, type
            ptr_to[a[0]] = a[2]
        elif op == OP_VARIABLE and len(a) >= 2:
            var_type[a[1]] = a[0]
        elif op in (OP_LOAD, OP_ACCESS_CHAIN) and len(a) >= 3:
            loads.append((a[0], a[1], a[2]))     # result type, result, pointer / base
        elif op == OP_IMAGE_READ and len(a) >= 3:
            reads.append(a[2])                   # the image operand
        i += n
    fragcoord = any(p in fragcoord_vars for _, _, p in loads)
    sub_vars = {v for v, t in var_type.items() if ptr_to.get(t) in subpass_types}
    sub_loads = {r for t, r, p in loads if p in sub_vars or t in subpass_types}
    subpass = any(img in sub_loads for img in reads)
    return spec, fragcoord, subpass

def main():
    d = sys.argv[1]
    allow = {}
    if len(sys.argv) > 2 and os.path.exists(sys.argv[2]):
        for line in open(sys.argv[2], encoding='utf-8'):
            line = line.strip()
            if not line or line.startswith('#') or ':' not in line:
                continue
            k, v = line.split(':', 1)
            allow[k.strip()] = v.strip()
    names = sorted(f for f in os.listdir(d) if f.endswith('.frag.spv'))
    bad, allowed, tripped = [], [], set()
    for f in names:
        try:
            spec, fc, sp = scan(os.path.join(d, f))
        except ValueError as e:
            bad.append('%s: unreadable (%s)' % (f, e)); continue
        why = []
        if sp: why.append('subpass input read')
        if spec and fc: why.append('specialization constants (%d) + FragCoord' % spec)
        if not why:
            continue
        tripped.add(f)
        if f in allow:
            allowed.append('allowed  %-34s %s -- %s' % (f, ', '.join(why), allow[f]))
        else:
            bad.append('FAIL     %-34s %s' % (f, ', '.join(why)))
    stale = [k for k in allow if k not in tripped]
    for l in allowed: print(l)
    for l in bad: print(l)
    for k in stale: print('STALE    %-34s in the allow list but %s' % (k, 'missing' if k not in names else 'no longer trips the rule'))
    print('spv_guard: %d fragment stages, %d trip the rule, %d allowed, %d failing, %d stale allow entries'
          % (len(names), len(tripped), len(allowed), len(bad), len(stale)))
    sys.exit(1 if bad else 2 if stale else 0)

if __name__ == '__main__':
    main()
