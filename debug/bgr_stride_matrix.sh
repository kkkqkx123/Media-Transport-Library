#!/bin/bash
# BGR stride/format matrix: which MPP input setting encodes BGR correctly?
# Board-only (native gcc + rockchip_mpp + mpi_dec_test). No repo build needed.
#
# Cells (8 frames each, CBR): nv12/hs1920 (REF, ground truth),
# bgr3/hs1920 (current production semantics), bgr3/hs5760 (byte-stride
# hypothesis), rgb3/hs1920 (channel-order hypothesis).
# Verdict per cell: CLEAN (matches REF) or TILED (640-periodic).
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/tmp/bgr_matrix}"
PROBE="$OUT/probe"
W=1920; H=1080; NFR=8

mkdir -p "$OUT"
command -v gcc >/dev/null 2>&1 || { echo "缺 gcc（板上直编探针用）"; exit 1; }
command -v mpi_dec_test >/dev/null 2>&1 || { echo "缺 mpi_dec_test（回放解码用）"; exit 1; }
if [ ! -x "$PROBE" ]; then
  echo "build probe (native)..."
  gcc "$SRC_DIR/mpp_enc_frame_latency.c" -o "$PROBE" -lrockchip_mpp
fi

echo "gen pattern..."
python3 - "$OUT/pat.bgr3" "$OUT/pat.nv12" <<'PY'
import sys
W,H=1920,1080
pb,pc=open(sys.argv[1],'wb'),open(sys.argv[2],'wb')
yb=bytearray(W*H); uv=bytearray(W*H//2)
for r in range(H):
    band=(r//120)%2
    for c in range(W):
        if c<640:   b,g,rr=(200,40,40)
        elif c<1280: b,g,rr=(40,200,40)
        else:       b,g,rr=(40,40,200)
        if c%160<2: b,g,rr=255,255,255
        k=40 if band else 0
        b,g,rr=min(255,b+k),min(255,g+k),min(255,rr+k)
        pb.write(bytes((b,g,rr)))
        yv=(66*rr+129*g+25*b+128)//256
        yb[r*W+c]=max(0,min(255,yv))
        if r%2==0 and c%2==0:
            cb=(-38*rr-74*g+112*b+128)//256+128
            cr=(112*rr-94*g-18*b+128)//256+128
            uv[(r//2)*(W)+c]=max(0,min(255,cb))
            uv[(r//2)*(W)+c+1]=max(0,min(255,cr))
for r in range(H):
    pc.write(yb[r*W:(r+1)*W])
for r in range(H//2):
    pc.write(uv[r*W:(r+1)*W])
pb.close(); pc.close()
print('pattern ok')
PY

run_cell() { # name infmt hs filestride
  local name="$1" fmt="$2" hs="$3" fs="$4" pat="$OUT/pat.bgr3"
  [ "$fmt" = nv12 ] && pat="$OUT/pat.nv12"
  "$PROBE" --input "$pat" --w $W --h $H --frames $NFR --fps 60 \
    --bps 8000000 --infmt "$fmt" --hor-stride "$hs" --file-stride "$fs" \
    --out "$OUT/$name.h264" >"$OUT/$name.enc.log" 2>&1
  mpi_dec_test -t 7 -i "$OUT/$name.h264" -n $NFR -o "$OUT/$name.yuv" \
    >>"$OUT/$name.enc.log" 2>&1 || true
}

run_cell ref    nv12 1920 0
run_cell bgr1920 bgr3 1920 0
run_cell bgr5760 bgr3 5760 0
run_cell rgb1920 rgb3 1920 0

python3 - "$OUT" "$NFR" <<'PY'
import sys
W,H=1920,1080
fs=W*H*3//2
out,nfr=sys.argv[1],int(sys.argv[2])
def load(n):
    d=open(f'{out}/{n}.yuv','rb').read()
    k=len(d)//fs
    return d[(k//2)*fs:(k//2+1)*fs][:W*H]
ref=load('ref')
def stats(Y):
    prof=[0.0]*W
    for r in range(200,800):
        row=Y[r*W:(r+1)*W]
        for c in range(W): prof[c]+=row[c]
    prof=[p/600 for p in prof]
    mx=sum(prof)/W
    def ac(p): return sum((prof[c]-mx)*(prof[c+p]-mx) for c in range(W-p))/(W-p)
    b=ac(1) or 1e-9
    e640=ac(640)/abs(b)
    def mean(x0,x1):
        s=0;n=0
        for r in range(200,800,5):
            for c in range(x0,x1,7): s+=Y[r*W+c]; n+=1
        return s/n
    m0,m1,m2=mean(0,640),mean(640,1280),mean(1280,1920)
    return e640,min(abs(m0-m1),abs(m1-m2),abs(m0-m2))
def mad(a,b):
    return sum(abs(x-y) for x,y in zip(a[::37],b[::37]))/len(a[::37])
print(f"{'cell':<10}{'E640':>8}  {'thirds':>8}  {'MADvsREF':>9}  verdict")
for n in ['ref','bgr1920','bgr5760','rgb1920']:
    try: Y=load(n)
    except Exception as e: print(f'{n:<10} MISSING {e}'); continue
    e640,td=stats(Y)
    m=0.0 if n=='ref' else mad(Y,ref)
    v='TILED' if e640>0.8 else ('CLEAN' if m<12 else 'DIFF')
    print(f'{n:<10}{e640:>8.2f}{td:>8.1f}{m:>9.1f}  {v}')
PY
