/* code_resz.c with a 4x multisampled D24S8 depth surface: RESZ resolves sample 0 of the depth
 * into the INTZ texture (d9mt: D3D9DeviceEx::ResolveZ; on GPUs before Apple10, or with
 * D9MT_TEX_PFV=resolve, through the source's resolve image and a copy, D9MT_TEX_PFV). The INTZ
 * texture is created with D3DUSAGE_DEPTHSTENCIL, as AMD documents it: the resolve renders into it.
 * (A sampled-only INTZ, code_resz's, isn't resolved into by d9mt, in any mode.) The quads cover
 * whole pixels, so every sample has the same depth and the expected greys are code_resz's. */
#define RESZ_SAMPLES D3DMULTISAMPLE_4_SAMPLES
#define RESZ_NAME "d9mt_resz_msaa"
#define RESZ_INTZ_USAGE D3DUSAGE_DEPTHSTENCIL
#include "code_resz.c"
