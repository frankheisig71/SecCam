from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
from configparser import ConfigParser
from pathlib import Path


def _read_ini_config(env):
    project_dir = Path(env["PROJECT_DIR"])
    ini_path = project_dir / "platformio.ini"
    parser = ConfigParser()
    parser.read(ini_path)
    return parser


def _get_config(env, key, default, aliases=None):
    parser = _read_ini_config(env)
    aliases = aliases or []
    candidates = [key, *aliases]

    for section_name in [f"env:{env.get('PIOENV', '')}", env.get("PIOENV", ""), "remote", "env"]:
        if not section_name or not parser.has_section(section_name):
            continue
        section = parser[section_name]
        for candidate in candidates:
            if candidate in section and section[candidate] not in (None, ""):
                value = section[candidate]
                return str(value).strip()

    return str(default).strip() if default is not None else default


def _remote_cfg(env):
    return {
        "remote_user": _get_config(env, "remote_user", "frank", ["user"]),
        "remote_password": _get_config(env, "remote_password", "voodoo", ["password"]),
        "remote_host": _get_config(env, "remote_host", "192.168.178.155", ["host"]),
        "remote_ssh_port": _get_config(env, "remote_ssh_port", "22", ["ssh_port"]),
        "remote_dir": _get_config(env, "remote_dir", "/home/frank/platformio/firmware", ["dir"]),
        "remote_esp_port": _get_config(env, "remote_esp_port", "/dev/ttyUSB0", ["esp_port"]),
        "remote_flash_baud": _get_config(env, "remote_flash_baud", "460800", ["flash_baud"]),
        "remote_monitor_baud": _get_config(env, "remote_monitor_baud", "115200", ["monitor_baud"]),
        "remote_chip": _get_config(env, "remote_chip", "esp32s3", ["chip"]),
    }


def _ssh_command_prefix(cfg, command_name):
    if cfg.get("remote_password") and cfg["remote_password"] not in ("", "None"):
        if shutil.which("sshpass") is None:
            raise RuntimeError(
                "sshpass is not installed on this machine. Install it with: sudo apt-get install sshpass"
            )
        return ["sshpass", "-p", cfg["remote_password"], command_name]
    return [command_name]


def _find_build_binary(env):
    build_dir = Path(env.subst("$BUILD_DIR"))
    progname = env.subst("$PROGNAME")
    candidates = [
        build_dir / f"{progname}.bin",
        build_dir / "firmware.bin",
        build_dir / f"{progname}.elf",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate

    binaries = sorted(build_dir.glob("*.bin"))
    if binaries:
        return binaries[0]

    raise FileNotFoundError(
        "No firmware binary found in build directory. Run 'pio run' first."
    )


def _find_upload_artifacts(env):
    build_dir = Path(env.subst("$BUILD_DIR"))
    artifacts = []

    for name in ["bootloader.bin", "partitions.bin", "firmware.bin"]:
        artifact = build_dir / name
        if artifact.exists():
            artifacts.append(artifact)

    boot_app0_candidates = [
        Path(env.subst("$PLATFORMIO_PACKAGES_DIR")) / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin",
        Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin",
        Path.home() / ".platformio" / "packages" / "framework-espidf" / "components" / "partition_table" / "partitions" / "boot_app0.bin",
    ]
    for boot_app0 in boot_app0_candidates:
        if boot_app0.exists():
            artifacts.append(boot_app0)
            break

    if not artifacts:
        raise FileNotFoundError(
            "No ESP32 flash artifacts found in build directory. Run 'pio run' first."
        )

    ordered = []
    for name in ["bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin"]:
        for artifact in artifacts:
            if artifact.name == name:
                ordered.append(artifact)
                break

    # Ensure the app binary is always included even if a different binary name was chosen.
    app_candidate = _find_build_binary(env)
    if app_candidate not in ordered:
        ordered.append(app_candidate)

    return ordered


def _dry_run(label, command):
    if os.getenv("REMOTE_PI_DRY_RUN") == "1":
        print(f"[remote_pi] DRY RUN: {label}")
        print(f"  {command}")
        return True
    return False


def _upload_artifacts_to_pi(env):
    cfg = _remote_cfg(env)
    artifacts = _find_upload_artifacts(env)

    if os.getenv("REMOTE_PI_DRY_RUN") == "1":
        for artifact in artifacts:
            remote_path = f"{cfg['remote_dir']}/{artifact.name}"
            scp_cmd = _ssh_command_prefix(cfg, "scp") + [
                "-P",
                str(cfg["remote_ssh_port"]),
                str(artifact),
                f"{cfg['remote_user']}@{cfg['remote_host']}:{remote_path}",
            ]
            print(f"[remote_pi] DRY RUN: would upload {artifact} to {cfg['remote_host']}:{remote_path}")
            print("  " + " ".join(shlex.quote(part) for part in scp_cmd))
        return 0

    mkdir_cmd = _ssh_command_prefix(cfg, "ssh") + [
        "-p",
        str(cfg["remote_ssh_port"]),
        f"{cfg['remote_user']}@{cfg['remote_host']}",
        "mkdir -p " + shlex.quote(cfg["remote_dir"]),
    ]

    print(f"[remote_pi] Creating remote directory {cfg['remote_dir']} on {cfg['remote_host']}")
    if _dry_run("mkdir remote", " ".join(shlex.quote(part) for part in mkdir_cmd)):
        pass
    else:
        if subprocess.run(mkdir_cmd, check=False).returncode != 0:
            raise RuntimeError(f"Failed to create remote directory {cfg['remote_dir']} on {cfg['remote_host']}")

    for artifact in artifacts:
        remote_path = f"{cfg['remote_dir']}/{artifact.name}"
        scp_cmd = _ssh_command_prefix(cfg, "scp") + [
            "-P",
            str(cfg["remote_ssh_port"]),
            str(artifact),
            f"{cfg['remote_user']}@{cfg['remote_host']}:{remote_path}",
        ]

        print(f"[remote_pi] Uploading {artifact} to {cfg['remote_host']}:{remote_path}")
        if _dry_run("scp transfer", " ".join(shlex.quote(part) for part in scp_cmd)):
            continue
        if subprocess.run(scp_cmd, check=False).returncode != 0:
            raise RuntimeError(f"Failed to copy {artifact.name} to {cfg['remote_host']}")

    return 0


def pi_upload_action(target, source, env):
    print("[remote_pi] Upload-only mode: using existing local build artifacts")
    return _upload_artifacts_to_pi(env)


def pi_build_upload_action(target, source, env):
    project_dir = env["PROJECT_DIR"]
    env_name = env.get("PIOENV") or env.get("ENV")
    build_cmd = [sys.executable, "-m", "platformio", "run"]
    if env_name:
        build_cmd.extend(["--environment", env_name])
    print(f"[remote_pi] Building firmware for environment '{env_name or 'default'}' before remote upload")
    if _dry_run("local build", " ".join(shlex.quote(part) for part in build_cmd)):
        return _upload_artifacts_to_pi(env)

    result = subprocess.run(build_cmd, cwd=project_dir, check=False)
    if result.returncode != 0:
        raise RuntimeError("Local PlatformIO build failed")

    return _upload_artifacts_to_pi(env)


def generate(env):
    env.AddCustomTarget(
        "pi_upload",
        None,
        pi_upload_action,
        "Upload to Pi",
    )
    env.AddCustomTarget(
        "pi_build_upload",
        None,
        pi_build_upload_action,
        "Build + Upload to Pi",
    )


generate(DefaultEnvironment())
