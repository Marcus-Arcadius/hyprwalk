# Credits

Back to the [README](../README.md).

- [cgltf](https://github.com/jkuhlmann/cgltf) (MIT) and [stb_image, stb_dxt and stb_vorbis](https://github.com/nothings/stb)
  (public domain or MIT), vendored in `src/third_party/`.
- The bone-name table in `tools/unity2hyprwalk.py` is from [Modular Avatar](https://github.com/bdunderscore/modular-avatar)
  (MIT, © 2022 bd_), which took it from HhotateA's AvatarModifyTools (MIT, © 2021 @HhotateA_xR) and Azukimochi's
  BoneRenamer (MIT, © 2023 Azukimochi). The rest of the converter's Modular Avatar support reimplements MA's
  behaviour in Python, written from reading MA's source.
- The humanoid muscle maths follow lox9973's [ShaderMotion](https://gitlab.com/lox9973/ShaderMotion) (MIT,
  © 2020-2021 lox9973) and [uvw.js](https://gitlab.com/lox9973/uvw.js) (Apache 2.0, © 2022-2023 lox9973).
- The converter knows UnlitWF's and lilToon's shaders by their GUIDs (from
  [Unlit_WF_ShaderSuite](https://github.com/whiteflare/Unlit_WF_ShaderSuite), zlib, and
  [lilToon](https://github.com/lilxyzw/lilToon), MIT, © 2020-2024 lilxyzw). Their settings, and
  [Poiyomi Toon](https://github.com/poiyomi/PoiyomiToonShader)'s (MIT), were read from those shaders' sources. None of
  their code is in this repo.
- The VRCFury support reimplements [VRCFury](https://github.com/VRCFury/VRCFury)'s build behaviour (© 2022 Senky,
  under its own license), written from reading its source. It contains none of VRCFury's code.
- The vowel recordings in `extras/speech` are from Wikimedia Commons, Lingua Libre and Tofugu/WaniKani (public
  domain, CC0, CC BY and CC BY-SA 4.0), and the formant data in `extras/speech/ref` is Kakeru Yazawa's (Zenodo
  15227304, CC BY 4.0); `extras/speech/LICENSES.md` credits each. The patches in `extras/` change Hyprland's and
  aquamarine's code (BSD 3-Clause).
- License texts are in [THIRD_PARTY.md](../THIRD_PARTY.md). VRChat, Modular Avatar, Counter-Strike 2 and Blender belong
  to their owners. This project has no connection with any of them and ships none of their assets.
