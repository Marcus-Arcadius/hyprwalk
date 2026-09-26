# A CS2 sun with no baked shadow channel

Written from the report of the research agent (session 085051d7, 2026-09-26). Data, decompiled shaders and scripts
are in this folder: survey_table.md, survey_raw.json, light_environment_keys.json, world_lighting.json, ents/,
lists/, world/, lm/, shaders/combos/, lmview/, web/, tools/, vcsdump/.

## CS2's shaders (decompiled SPIR-V of game/csgo*/shaders_vulkan_dir.vpk)
- csgo_environment, lightmap combo (shaders/combos/csgo_environment_vulkan_60_ps_s16_d4.txt): the sun's cascaded
  shadow (942-1092, fading to 1 past the last cascade, 1 without cascades), then
  `:1094 sunVis = csm * (1.0 - dot(directLightShadows, sunMask))`; diffuse and GGX times sunColor * sunVis.
- Probe combo (..._s16_d2.txt): the _dlshd atlas blended like irradiance (shadow 0 outside all volumes);
  `:1233` the same formula. No baked lighting / vertex-lit: CSM only.
- csgo_complex and csgo_lightmappedgeneric: the same formula (complex s0_d2:638, s0_d4:399; lightmappedgeneric
  s0_d2:615, s0_d4:374).
- So: one formula on every surface; no channel = a zero mask = baked factor 1: the sun is shadowed by the CSM alone.
- The engine's own table (world.vwrld_c m_worldLightingInfo.m_bakedShadows by light_path_uniqueid) agrees with
  bakedshadowindex for all 519 lights checked; m_bBakedShadowsGamma20 false everywhere (1 - r is right).

## Keys (Source2 wiki's FGD dump, VDC HL:A lighting page, VRF)
- directlight: None 0, Baked 1, Dynamic 2, Stationary 3 (VDC's light_omni2 page has 2 and 3 swapped; the data agree
  with the FGD: every directlight 3 light has an index, no directlight 2 one does).
- baked_light_indexing ("Stationary Light Shadows"): mixes the CSM with baked shadows. Baked with it off is "fully
  baked": "Direct component gets stored in lightmaps and light probe volumes" (VDC, HL:A).
- VRF (src d757df4): SceneLight.cs:276-277 index defaults to -1; :324-327 baked_light_indexing 0 forces -1 for
  light_environment; :860-867 one-hot mask 0..3 else zero; lighting.slang:227-234 the same formula.

## Survey of the install (132 map/prefab VPKs)
- Every playable map with a lit sun: bakedshadowindex 0 (so do their 3D skyboxes). de_mirage: directlight 1,
  index 0, baked_light_indexing true, brightness 3.
- No index (the key absent, never -1): cs_office and its skybox (directlight 1, baked_light_indexing false,
  brightness 0: off); de_train's sun has index 0 but brightness 0; prefabs 3dskybox_mirage and inferno s2_3d_skybox
  (directlight 2, referenced by no map); templates and ui maps (directlight 2).
- de_mirage's lightmaps and probes don't hold the sun's direct light (lit sides of baked shadow edges only 1.15x
  brighter in irradiance); nor do icon_generation's (directlight 1 and 2 suns).

## What hypr3d does now (after this session)
- Plugin: a lighting set without a `shadows` lightmap has no baked shadow (uBakedShadow 0: baked = 1), as the probes'
  alpha already said: both kinds of surface get sun x realtime shadow.
- cs2map: sun_channel() (-1 when the key is missing or the sun is fully baked), sun_runtime() (Dynamic/Stationary,
  or Baked with a channel; enabled and bright), no probe shadow atlas without a channel (not max(channel, 0)), a
  black sun when CS2 doesn't light with it at run time (the plugin would otherwise use its own default sun), and a
  brightness of 0 stays 0 (it was read as 1: de_train's and cs_office's dark suns were lit).

## Inferred, not confirmed
- That the engine sets no runtime sun for a fully baked one (docs; the only such CS2 sun has brightness 0).
- castshadows 2 probably means no sun CSM ("Baked (2)"); not used.
