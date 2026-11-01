# Phase 1 shader trial

Run from `work/experiments/shader-trial`, with these files copied there and XenosRecomp cloned at commit `4906992` (PR 22). Translated shaders derive from the game, so they stay out of this repo.

1. `extract.py <game folder> extracted`: one file per shipped shader.
2. `run-trial.sh`: applies `xenosrecomp-crackdown.patch`, builds the translator without its HLSL compiler, translates and compiles. It compiles and runs third-party code, so it's a step for Bruce to run or approve. A line starting `UNSUPPORTED` in `translated/*.translate.txt` marks a feature that shader uses and the output doesn't implement.
3. `join.py`: lines each shipped shader up with the emulator's Metal. It needs a MoltenVK dump (`MVK_CONFIG_SHADER_DUMP_DIR`) and SPIR-V saved by ucode hash, which took a temporary change to `VulkanTranslation::GetOrCreateShaderModule`.
4. `time-all.py`: times both forms with `harness_xenos.swift` and `experiments/menu-bench/harness`.
5. `pull_vertices.py extracted translated "<Crackdown Data>/user_data/native-shaders"`: writes the vertex shaders shadow mode loads, with `manifest.txt`. It needs `translated/` from step 2 and only Apple's compiler.
6. `build_pixels.py translated "<Crackdown Data>/user_data/native-shaders"`: compiles the pixel shaders and writes `pixel-manifest.txt`.
