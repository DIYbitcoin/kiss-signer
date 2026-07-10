#!/usr/bin/env python3
# Render the on-screen QR test page from mk_qr_parts output (one part per line).
# usage: mk_test_qrs.py <out-dir>  — expects <out-dir>/<name>.parts files written
# by mk_test_qrs.sh; writes <out-dir>/index.html with all frames inlined as data
# URIs (no network, no external files — open it on any screen and scan).
import base64
import io
import sys
from pathlib import Path

import qrcode
from qrcode.constants import ERROR_CORRECT_L, ERROR_CORRECT_M

# (file stem, heading, what the device should do)
BLOCKS = [
    ("static-native", "1 · Single QR (base64 PSBT)",
     "expect: READY - 1 Native SegWit input - TESTNET - 60,000 sats out"),
    ("pmofn-nested", "2 · Animated pMofN (Specter/Krux legacy format)",
     "expect: parts counter while reading, then READY - Nested SegWit - TESTNET"),
    ("ur-legacy", "3 · Animated BC-UR fountain (ur:crypto-psbt)",
     "expect: segmented bar fills part by part, then READY - Legacy - TESTNET"),
    ("static-wrongnet", "4 · Wrong-network PSBT (mainnet coin type)",
     "expect: STOP - wrong network (this one must NOT be signable on testnet)"),
]
FRAME_MS = 500          # sender cadence; scanner reads at ~30fps, plenty


def qr_png_uri(text, ec):
    q = qrcode.QRCode(error_correction=ec, box_size=8, border=4)
    q.add_data(text)
    q.make(fit=True)
    buf = io.BytesIO()
    q.make_image(fill_color="black", back_color="white").save(buf, format="PNG")
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


def main(outdir):
    out = Path(outdir)
    sections = []
    for stem, title, expect in BLOCKS:
        parts = (out / f"{stem}.parts").read_text().splitlines()
        parts = [p for p in parts if p]
        ec = ERROR_CORRECT_M if len(parts) == 1 else ERROR_CORRECT_L
        uris = [qr_png_uri(p, ec) for p in parts]
        imgs = ",".join(f'"{u}"' for u in uris)
        anim = "" if len(parts) == 1 else f"""
<script>(function() {{
  var f = [{imgs}], i = 0, el = document.getElementById("qr_{stem}");
  setInterval(function() {{ i = (i + 1) % f.length; el.src = f[i]; }}, {FRAME_MS});
}})();</script>"""
        sections.append(f"""
<section>
  <h2>{title}</h2>
  <p class="expect">{expect}</p>
  <p class="meta">{len(parts)} part(s)</p>
  <img id="qr_{stem}" src="{uris[0]}" alt="{stem}">
  {anim}
</section>""")
    html = f"""<!doctype html>
<meta charset="utf-8">
<title>KISS wallet - QR scan test fixtures (dev seed 73C5DA0A, TESTNET)</title>
<style>
  body {{ background:#fff; color:#111; font:16px/1.5 -apple-system, sans-serif;
         max-width:560px; margin:2rem auto; text-align:center; }}
  section {{ margin-bottom:4rem; }}
  img {{ width:440px; max-width:95vw; image-rendering:pixelated; }}
  h2 {{ margin-bottom:.2rem; }}
  .expect {{ color:#444; margin:.2rem 0; }}
  .meta {{ color:#999; font-size:13px; margin:.2rem 0 .8rem; }}
</style>
<h1>KISS scan test QRs</h1>
<p>device: TESTNET network, wallet restored from the dev words
(abandon &times; 11 + about, no passphrase &rarr; fingerprint 73C5DA0A).<br>
Sign &rarr; SCAN QR, then point the camera at each block in turn.</p>
{"".join(sections)}
"""
    (out / "index.html").write_text(html)
    print(f"wrote {out}/index.html")


if __name__ == "__main__":
    main(sys.argv[1])
