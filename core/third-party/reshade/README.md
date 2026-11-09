# ReShade add-on headers

`include/` is ReShade's add-on API, the headers of https://github.com/crosire/reshade at tag v6.8.0 (`include/`),
unchanged, under its BSD 3-Clause licence (`LICENSE.md`, from the same tag). The mod includes `reshade.hpp` without
`imgui.h` (`core/src/ReShade.cpp`, built without the precompiled header), so `reshade_overlay.hpp` compiles to nothing and the
ImGui check of `register_addon` is skipped; see `docs/t5-visuals-plan.md` 1.
