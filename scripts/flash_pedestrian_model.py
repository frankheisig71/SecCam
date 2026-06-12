Import("env")

import glob
import json
import os
import subprocess


def _flash_model_partition(source, target, env):
    model_partition = env.GetProjectOption("custom_model_partition", "").strip()
    model_component_dir = env.GetProjectOption("custom_model_component_dir", "").strip()
    model_glob = env.GetProjectOption("custom_model_glob", "").strip()
    script_tag = f"flash_model:{model_partition or 'unknown'}"

    if not model_partition or not model_component_dir or not model_glob:
        print(f"[{script_tag}] model upload options incomplete, skipping model flash")
        return

    build_dir = env.subst("$BUILD_DIR")
    flasher_args_path = os.path.join(build_dir, "flasher_args.json")
    if not os.path.exists(flasher_args_path):
        print(f"[{script_tag}] flasher_args.json not found, skipping model flash")
        return

    with open(flasher_args_path, "r", encoding="utf-8") as flasher_args_file:
        flasher_args = json.load(flasher_args_file)

    model_entry = flasher_args.get(model_partition)
    if not model_entry:
        print(f"[{script_tag}] {model_partition} entry missing, skipping model flash")
        return

    model_path = os.path.join(build_dir, model_entry["file"])
    python_exe = env.subst("$PYTHONEXE")
    if not os.path.exists(model_path):
        project_dir = env.subst("$PROJECT_DIR")
        pack_script = os.path.join(
            project_dir, "managed_components", "espressif__esp-dl", "fbs_loader", "pack_espdl_models.py"
        )
        source_models = sorted(
            glob.glob(
                os.path.join(project_dir, "managed_components", model_component_dir, *model_glob.split("/"))
            )
        )
        if not os.path.exists(pack_script) or not source_models:
            print(f"[{script_tag}] source model files missing, skipping model flash")
            return

        os.makedirs(os.path.dirname(model_path), exist_ok=True)
        print(f"[{script_tag}] model artifact missing, packing model")
        subprocess.check_call(
            [python_exe, pack_script, "--model_path", *source_models, "--out_file", model_path],
            cwd=build_dir,
        )

    if not os.path.exists(model_path):
        print(f"[{script_tag}] model artifact still missing after build, skipping model flash")
        return

    upload_port = env.subst("$UPLOAD_PORT")
    upload_speed = env.subst("$UPLOAD_SPEED")
    uploader = env.subst("$UPLOADER")
    chip = env.BoardConfig().get("build.mcu", "esp32s3")

    command = [
        python_exe,
        uploader,
        "--chip",
        chip,
        "--port",
        upload_port,
        "--baud",
        upload_speed,
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        model_entry["offset"],
        model_path,
    ]

    print(f"[{script_tag}] flashing {model_partition} partition")
    subprocess.check_call(command, cwd=build_dir)


env.AddPostAction("upload", _flash_model_partition)