#!/usr/bin/env python3
"""Build the single-user X2 engine using verified upstream shape helpers.

Run inside the isolated voice environment. Docker uses NVIDIA CDI on NixOS;
no daemon configuration or system packages are modified.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--root", type=Path, required=True)
p.add_argument("--cp-precision", choices=("bf16", "fp32"), default="bf16")
p.add_argument("--image", default="nvcr.io/nvidia/tensorrt@sha256:94aa793e6db548940168186172b5acbe413cae9e56f7a656d171cd0dd9b6a869")
args = p.parse_args()
root = args.root.resolve()
sys.path.insert(0, str(root / "scripts/python"))
from trt_fused_io_formats import (fused_input_output_io_format_strings,
    fused_precision_args, fused_layer_precisions, fused_precision_constraints)
from trt_fused_talk_c2w_profiles import compute_fused_profiles, compute_fused_decode_profiles
variant = root / "workspace/exported/custom-1.7b"
building = variant / f"nala-building-{uuid.uuid4().hex}.engine"
manifest_path = variant / "triton_manifest.json"
manifest = json.loads(manifest_path.read_text())
manifest["cp_precision"] = args.cp_precision
arch = manifest["architecture"]
inputs, outputs = fused_input_output_io_format_strings(manifest)
params = [arch["hidden_size"], arch["kv_heads"], arch["head_dim"], arch["num_layers"], 1]
profiles = [compute_fused_profiles(*params, 256, 512, arch["n_c2w_layers"], arch["cp_num_stages"]),
            compute_fused_decode_profiles(*params, 512, arch["n_c2w_layers"], arch["cp_num_stages"])]
command = ["docker", "run", "--rm", "--device", "nvidia.com/gpu=0", "--shm-size", "2g",
           "--user", f"{os.getuid()}:{os.getgid()}", "-v", f"{variant}:/work", "-w", "/work",
           args.image, "/opt/tensorrt/bin/trtexec", "--onnx=talker_code2wav_fused.onnx",
           f"--saveEngine={building.name}", "--skipInference", "--memPoolSize=workspace:2048", "--builderOptimizationLevel=0", "--maxAuxStreams=0",
           f"--inputIOFormats={inputs}", f"--outputIOFormats={outputs}", *fused_precision_args(manifest)]
layer = fused_layer_precisions(manifest)
if layer:
    command += [f"--layerPrecisions={layer}", f"--precisionConstraints={fused_precision_constraints(manifest)}"]
for index, shapes in enumerate(profiles):
    command += [f"--profile={index}"] + [f"--{kind}Shapes={shape}" for kind, shape in zip(("min", "opt", "max"), shapes)]
result = subprocess.run(command)
if result.returncode and (result.returncode != 137 or not building.exists()):
    raise subprocess.CalledProcessError(result.returncode, command)
# trtexec may exhaust host RAM in its post-save deserialize check. Validate
# in a fresh process with the exact deployment runtime before adopting a plan.
validation = """import tensorrt as trt, sys
from pathlib import Path
runtime = trt.Runtime(trt.Logger(trt.Logger.ERROR))
engine = runtime.deserialize_cuda_engine(Path(sys.argv[1]).read_bytes())
assert engine is not None and engine.num_io_tensors == 62
assert engine.num_optimization_profiles == 2
print('Deployment-runtime engine validation passed')
"""
subprocess.run([str(root.parent / "x2-env/bin/python"), "-c", validation,
                str(building)], check=True)
building.replace(variant / "talker_code2wav_fused.engine")
manifest["engine_profile"].update(max_batch_size=1, max_input_len=256, max_seq_len=512,
    cp_precision=args.cp_precision, builder_image=args.image, tensorrt_version="10.16.1.11")
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
