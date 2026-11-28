"""Package the exact tested build without rebuilding any executable."""
from pathlib import Path
import hashlib
import zipfile
import copy
import zlib

root = Path(".")
raw = root / "dist/Twitch-1.33-Classic.vpk"
out = root / "dist/Twitch-1.33.vpk"
def digest(b): return hashlib.sha256(b).hexdigest()
assert digest(raw.read_bytes()) == "9e555d10f5f218498db82f580fecb1202e21ec9194348452998f4256db6e22a1", "unexpected CI VPK"
art = {
    "sce_sys/icon0.png": "d5f4b3ee55c88307734df9a14f994df5ab7d385adfef76c83a6c1e3567c19415",
    "sce_sys/livearea/contents/bg.png": "96ee98839f8290313439b8bd7ef19d0233d0692a13727e030f5c058d53f4729a",
    "sce_sys/livearea/contents/startup.png": "2265d4173538abd8bfc463105740c0d1ed6fe92b2a7954ad9f7b62b9aa583688",
    "sce_sys/livearea/contents/template.xml": "574cfaa320985431ce569e3a79acfb78f07cd9a142a8a94e6b69ebe7b3b996e4",
}
for name, expected in art.items():
    assert digest((root / name).read_bytes()) == expected, name
with zipfile.ZipFile(raw) as src, zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as dest:
    assert src.testzip() is None
    for entry in src.infolist():
        name = entry.filename
        data = (root / name).read_bytes() if name in art else src.read(name)
        dest.writestr(copy.copy(entry), data)
with zipfile.ZipFile(raw) as src, zipfile.ZipFile(out) as final:
    assert final.testzip() is None
    assert final.namelist() == src.namelist()
    for name in final.namelist():
        assert digest(final.read(name)) == (art[name] if name in art else digest(src.read(name))), name
    assert b"01.33" in final.read("sce_sys/param.sfo") and b"VTWI00001" in final.read("sce_sys/param.sfo")
expected = "3e8c413297746028c0dd761b52dcab90a192f04181ba1cd386fef5b8ac5232be"
actual = digest(out.read_bytes())
print("zlib", zlib.ZLIB_RUNTIME_VERSION, "release SHA256", actual)
assert actual == expected, "release must match the exact hardware-approved package byte for byte"
(root / "dist/SHA256SUMS-release.txt").write_text(f"{actual}  {out.name}\n")
