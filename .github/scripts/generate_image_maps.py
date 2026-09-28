import itertools
import json
import os
import sys

source_tag = os.getenv("SOURCE_TAG")
target_tag = os.getenv("TARGET_TAG")
branch = os.getenv("BRANCH")
# Upstream pushes from ghcr.io/neondatabase to Docker Hub and to Neon's ECR/ACR registries (dev and prod).
# This fork only has its own GHCR namespace, e.g. ghcr.io/<owner>.
registry = os.getenv("REGISTRY", "ghcr.io/neondatabase")

components = {
    "neon": ["neon"],
    "compute": [
        "compute-node-v14",
        "compute-node-v15",
        "compute-node-v16",
        "compute-node-v17",
        "vm-compute-node-v14",
        "vm-compute-node-v15",
        "vm-compute-node-v16",
        "vm-compute-node-v17",
    ],
}

registries = {
    "dev": [
        registry,
    ],
}

release_branches = ["release", "release-proxy", "release-compute"]

outputs: dict[str, dict[str, list[str]]] = {}

target_tags = (
    [target_tag, "latest"]
    if branch == "main"
    else [target_tag, "released"]
    if branch in release_branches
    else [target_tag]
)
target_stages = ["dev"]

for component_name, component_images in components.items():
    for stage in target_stages:
        outputs[f"{component_name}-{stage}"] = {
            f"{registry}/{component_image}:{source_tag}": [
                f"{target_registry}/{component_image}:{tag}"
                for target_registry, tag in itertools.product(registries[stage], target_tags)
                if not (target_registry == registry and tag == source_tag)
            ]
            for component_image in component_images
        }

with open(os.getenv("GITHUB_OUTPUT", "/dev/null"), "a") as f:
    for key, value in outputs.items():
        f.write(f"{key}={json.dumps(value)}\n")
        print(f"Image map for {key}:\n{json.dumps(value, indent=2)}\n\n", file=sys.stderr)
