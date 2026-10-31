# Phase 1 shader trial

Run from `work/experiments/shader-trial`, with these files copied there and XenosRecomp cloned at commit `4906992` (PR 22). Translated shaders derive from the game, so they stay out of this repo.

1. `extract.py <game folder> extracted`: one file per shipped shader.
2. `run-trial.sh`: builds the translator without its HLSL compiler, translates and compiles. It compiles and runs third-party code, so it's a step for Bruce to run or approve. The final script hasn't been run in this form; the pieces ran separately.
3. `fix_text.py`, then `compile-fixed.sh` (`compile-all.sh` pointed at `fixed/`): estimates what small translator fixes buy by patching the generated text.
4. `join.py`: lines each shipped shader up with the emulator's Metal. It needs a MoltenVK dump (`MVK_CONFIG_SHADER_DUMP_DIR`) and SPIR-V saved by ucode hash, which took a temporary change to `VulkanTranslation::GetOrCreateShaderModule`.
5. `time-all.py`: times both forms with `harness_xenos.swift` and `experiments/menu-bench/harness`.
