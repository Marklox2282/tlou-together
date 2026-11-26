"""Выпуск новой версии мода: python tools/release.py

1. Поднять kModVersion в src/main.cpp (это и есть номер версии).
2. Запустить скрипт: соберёт мод, подпишет закрытым ключом из %USERPROFILE%/.tlou1-coop/signing.key
   и выложит релиз vN на GitHub (winmm.dll, winmm.dll.sig, version.txt). У друзей мод
   скачает его сам при следующем запуске игры и включит со следующего.
"""
import glob, os, pathlib, re, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEY = pathlib.Path(os.environ["USERPROFILE"]) / ".tlou1-coop" / "signing.key"
REPO = "marklox228lox/tlou1-coop"


def run(*cmd, **kw):
    print(">", " ".join(map(str, cmd)))
    return subprocess.run(cmd, check=True, **kw)


def tool_env():
    env = dict(os.environ)
    extra = [r"C:\Program Files\CMake\bin", r"C:\Program Files\LLVM\bin"]
    extra += glob.glob(os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build*"))
    env["PATH"] = os.pathsep.join(extra + [env["PATH"]])
    return env


def main():
    version = int(re.search(r"kModVersion = (\d+);", (ROOT / "src/main.cpp").read_text(encoding="utf-8")).group(1))
    tag = f"v{version}"
    if not KEY.exists():
        sys.exit(f"нет ключа подписи {KEY}")
    if subprocess.run(["gh", "release", "view", tag, "--repo", REPO], capture_output=True).returncode == 0:
        sys.exit(f"релиз {tag} уже есть: подними kModVersion в src/main.cpp")
    if subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout.strip():
        sys.exit("есть незакоммиченные изменения: сначала закоммить, чтобы релиз совпадал с кодом")
    env = tool_env()
    run("cmake", "--build", "build", cwd=ROOT, env=env)
    run(str(ROOT / "build/netsim.exe"), stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        dll = tmp / "winmm.dll"
        dll.write_bytes((ROOT / "build/winmm.dll").read_bytes())
        run(str(ROOT / "build/signer.exe"), "sign", str(KEY), str(dll), str(version), str(tmp / "winmm.dll.sig"))
        (tmp / "version.txt").write_text(f"{version}\n")
        run("git", "push", "origin", "HEAD", cwd=ROOT)
        run("gh", "release", "create", tag, "--repo", REPO, "--title", f"tlou1-coop {tag}",
            "--notes", "Test build. Install once: copy winmm.dll into the game folder; later versions update themselves.",
            str(dll), str(tmp / "winmm.dll.sig"), str(tmp / "version.txt"))
    print(f"готово: {tag}")


if __name__ == "__main__":
    main()
