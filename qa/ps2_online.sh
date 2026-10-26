#!/bin/bash
# PS2 online (LRPS2 input-delay lockstep): two headless retrorun peers against a LOCAL wrangler (never production).
# usage: qa/ps2_online.sh <tag> <iso> <frames> [direct|relay] [port]   (run from the repo root)
# Needs scratch/cores/pcsx2_libretro.dll (LRPS2 nightly, SHA-256 of the zip 730112622345f0ca...), scratch/sys0/pcsx2/bios/<BIOS> and a local
# `npx wrangler dev --ip 127.0.0.1 --port <port>` (WRANGLER_REGISTRY_PATH isolated). Host card = blank card + marker; the guest starts with a
# DIFFERENT card that must be replaced by the host's. Exit codes of both peers go to scratch/net_<tag>.rc; logs scratch/net_<tag>_<slot>.log.
# Passes when both exit 0, no DESYNC, the EE RAM sync checks match, and the guest booted from the host's card hash.
T="$1"; ROM="$2"; F="$3"; MODE="${4:-direct}"; PORT="${5:-11400}"
case "$PORT" in 8761|8787) echo "refusing port $PORT"; exit 9;; esac
export RETRORUN_MUTE=1 RETROHUB_QA=1
if [ "$MODE" = relay ]; then export HUBNET_FORCE_RELAY=1; else export HUBNET_LOOPBACK=1; fi
BIOS="${PS2_BIOS:-SCPH-70012_BIOS_V12_USA_200.BIN}"
ROOM=$(head -c 400 /dev/urandom | tr -dc 'A-HJ-NP-Z2-9' | head -c 6)-1
rm -f scratch/net_${T}.rc
python - "$T" <<'PY'
import sys,os,random
t=sys.argv[1]
src='scratch/o_a/sys/pcsx2/memcards/Mcd001.ps2'
blank=open(src,'rb').read() if os.path.exists(src) else b'\xff'*8650752
for s in (0,1):
    d='scratch/net_%s_%d'%(t,s)
    os.system('rm -rf "%s"'%d)
    os.makedirs(d+'/sv'); os.makedirs(d+'/st'); os.makedirs(d+'/sys/pcsx2/memcards',exist_ok=True)
    os.system('cp -r scratch/sys0/pcsx2/bios "%s/sys/pcsx2/"'%d)
    b=bytearray(blank)
    if s==0: b[-70000:-69000]=bytes(random.Random(7).randrange(256) for _ in range(1000))   # host marker
    else: b[-90000:-89000]=bytes(random.Random(9).randrange(256) for _ in range(1000))      # guest junk: must be replaced
    open(d+'/sys/pcsx2/memcards/Mcd001.ps2','wb').write(b)
PY
for s in 0 1; do
  D=scratch/net_${T}_$s
  B=$BIOS; TAG=QA-DISC; if [ $s = 1 ]; then B=${PS2_BIOS1:-$BIOS}; TAG=${PS2_TAG1:-QA-DISC}; fi   # PS2_BIOS1 / PS2_TAG1: make the guest differ (mismatch test)
  ( build/bin/Release/retrorun.exe --core scratch/cores/pcsx2_libretro.dll --rom "$ROM" --save-dir $D/sv --state-dir $D/st --system-dir $D/sys --system ps2 --headless \
   --opt pcsx2_bios=$B --setup-tag $TAG --share-host-save --frames $F --fuzz-input --net ws://127.0.0.1:$PORT --room $ROOM --slot $s --players 2 --ports 0,1,-1,-1 --delay auto \
   --log scratch/net_${T}_$s.log >/dev/null 2>&1; echo "rc$s=$?" >> scratch/net_${T}.rc ) &
done
wait
cat scratch/net_${T}.rc
for s in 0 1; do grep -h "hostsave\|fingerprint\|FAIL\|DESYNC\|sync check\|input delay\|^done\|RESULT\|final:" scratch/net_${T}_$s.log | tail -12 | sed "s/^/[$s] /"; done
H0=$(sha256sum scratch/net_${T}_0/sys/pcsx2/memcards/Mcd001.ps2 | cut -c1-16); H1=$(sha256sum scratch/net_${T}_1/sys/pcsx2/memcards/Mcd001.ps2 | cut -c1-16)
echo "card on disk (after the run): host $H0 guest $H1"
