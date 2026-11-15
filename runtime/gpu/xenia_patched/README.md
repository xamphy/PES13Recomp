# Patched Xenia sources

Copies of files from `thirdparty/xenia` (xenia-project/xenia @ 95a5c3e, BSD-3, see
`thirdparty/xenia/LICENSE`) with PES13 changes. `cmake/xenia_shader.cmake` builds these instead of
the originals; the submodule itself stays unmodified. Every change is marked `PES13`.

- `spirv_shader_translator_fetch.cc` - texture fetches in pixel shaders that compute their LOD from
  derivatives sample with an implicit LOD and the fetch constant / instruction LOD as a bias, instead
  of explicit gradients (coarse derivatives scaled by 2^lod). Same LOD; explicit-gradient sampling is
  much slower on Apple GPUs. 3D / stacked textures and register gradients keep Xenia's path.
  `PES_GRAD_LOD=1` = Xenia's original behaviour (A/B).
