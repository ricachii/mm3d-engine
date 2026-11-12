# Origin of the MM3D port files in this tree

These files come from **Volpe34/mm3d-pc** (https://github.com/Volpe34/mm3d-pc, v0.1.9, commit `fc4d8f6`),
a native PC port of Majora's Mask 3D on the TriAevum runtime, licensed **GPL-3.0-or-later**. All credit for
the port, `thumb_to_a32.py`, the MM3D host code, the setup and the original recipe goes to Volpe34.

Added on top (branch `eshop-1.1.0` of the local mm3d-pc clone, imported unchanged here):
`relocate_recipe.py` (relocates the recipe to another build of the game code), `make_cci.py` (base + update CIA ->
.cci with the update RomFS as partition 7), `package/recipe_eshop110/` (address-only recipe for the USA v1.1.0 eShop
build), per-build setup (`BUILDS`), `mm3d_build.h` (per-build host addresses) and the host support for the
update RomFS (fs:USER OpenFileDirectly, file path type 5).

Imported only as files: nothing here is wired into the build of this tree yet (Phase B, docs/plan-switch.md).
No game data, ROM, key or asset is part of these files (recipes are addresses only).
