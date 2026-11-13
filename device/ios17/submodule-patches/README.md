# UniXNU submodule changes (fuxi iOS 17.7 21H16)

Source-only patches against the submodule revisions pinned by UniXNU branch `fuxi/ios17-arm64e-checkpoint1`
(SadNutWall/UniXNU). Apply inside each submodule with `git apply`:

| Patch | Submodule | Base |
| --- | --- | --- |
| AdrenoMetal.patch | drivers/AdrenoMetal | official-51b7003-import (5d56587): GPU scanout scaling (scanout_scale AIR kernel) for the logical display mode |
| GopFramebuffer.patch | drivers/GopFramebuffer | NativeLegacyDisplay logical mode, surface probe, GPU offer condition |
| Adreno740.patch | drivers/Adreno740 | NativeAdreno memory/publish/user-client changes |
| XnuPrelink.patch | kits/XnuPrelink | fuxi-ios17-21h16: fileset emit/move changes (the 21H16 symbol fixture, derived from Apple's kernelcache, is not included) |
