"""Выпуск новой версии мода: python tools/release.py

1. Поднять версию в src/version.hpp (kMajor.kMinor.kPatch, например 0.41.0) и добавить раздел
   "## 0.41.0" в CHANGELOG.md - его текст станет описанием релиза на GitHub.
2. Запустить скрипт: соберёт мод, подпишет закрытым ключом из %USERPROFILE%/.tlou1-coop/signing.key
   и выложит релиз (например 0.41.0) на GitHub (winmm.dll, winmm.dll.sig, version.txt). У друзей мод
   скачает его сам при следующем запуске игры и включит со следующего.
"""
import glob, os, pathlib, re, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEY = pathlib.Path(os.environ["USERPROFILE"]) / ".tlou1-coop" / "signing.key"
REPO = "Marklox2282/tlou-together"


def run(*cmd, **kw):
    print(">", " ".join(map(str, cmd)))
    return subprocess.run(cmd, check=True, **kw)


def tool_env():
    env = dict(os.environ)
    extra = [r"C:\Program Files\CMake\bin", r"C:\Program Files\LLVM\bin"]
    extra += glob.glob(os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build*"))
    env["PATH"] = os.pathsep.join(extra + [env["PATH"]])
    return env


def changelog_notes(tag):
    """Раздел "## 0.41.0" из CHANGELOG.md (до следующего "## ")."""
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    m = re.search(rf"^## {re.escape(tag)}\s*$(.*?)(?=^## |\Z)", text, re.M | re.S)
    return m.group(1).strip() if m else ""


def main():
    m = re.search(r"kMajor = (\d+), kMinor = (\d+), kPatch = (\d+);", (ROOT / "src/version.hpp").read_text(encoding="utf-8"))
    major, minor, patch = map(int, m.groups())
    version = major * 10000 + minor * 100 + patch  # то же число, что version::kCode (version.txt, сеть)
    tag = f"{major}.{minor}.{patch}"
    notes = changelog_notes(tag)
    if not notes:
        sys.exit(f"нет раздела '## {tag}' в CHANGELOG.md: опиши, что изменилось")
    notes += ("\n\n---\nInstall once: copy winmm.dll into the game folder; later versions update themselves.\n"
              "Установка один раз: скопируйте winmm.dll в папку игры; дальше мод обновляется сам.")
    if not KEY.exists():
        sys.exit(f"нет ключа подписи {KEY}")
    if subprocess.run(["gh", "release", "view", tag, "--repo", REPO], capture_output=True).returncode == 0:
        sys.exit(f"релиз {tag} уже есть: подними версию в src/version.hpp")
    if subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout.strip():
        sys.exit("есть незакоммиченные изменения: сначала закоммить, чтобы релиз совпадал с кодом")
    env = tool_env()
    run("cmake", "--build", "build", cwd=ROOT, env=env)
    for test in ("netsim", "enginetest"):
        run(str(ROOT / f"build/{test}.exe"), stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        dll = tmp / "winmm.dll"
        dll.write_bytes((ROOT / "build/winmm.dll").read_bytes())
        run(str(ROOT / "build/signer.exe"), "sign", str(KEY), str(dll), str(version), str(tmp / "winmm.dll.sig"))
        (tmp / "version.txt").write_text(f"{version}\n")
        run("git", "push", "origin", "HEAD", cwd=ROOT)
        run("gh", "release", "create", tag, "--repo", REPO, "--title", f"tlou-together {tag}",
            "--notes", notes,
            str(dll), str(tmp / "winmm.dll.sig"), str(tmp / "version.txt"))
    print(f"готово: {tag}")


if __name__ == "__main__":
    main()
