/* A programmable pixel shader samples a DEFAULT texture that nothing has written yet (#55).
 * Since #105 such a texture is bound as null until its first write, but the shader's null check
 * (the sampler type) still sees a texture, so it samples. With an argument buffer (the default
 * for programmable shaders) that word used to be 0, a read through resource ID 0, which is
 * undefined on the GPU; now it is the null texture, which reads zero, as Windows does.
 * Three quads with the texquad shaders, every frame:
 *   left    a texture created this frame and never written (churn, as SWTOR's previews do);
 *   middle  a texture created at start, written by UpdateTexture from frame 3 on;
 *   right   a texture written at start (the control).
 * Checks in frames 1 and 3 (readback before Present): unwritten quads are black, written ones
 * show their colour, the background stays. FAIL lines count under the hash harness. */
#include "t_xfer.h"

#define BG       0xff404040u
#define LATE     0xff20c040u
#define CONTROL  0xffc04020u

static IDirect3DTexture9 *late_tex, *control_tex;

static IDirect3DTexture9 *make_default(void) {
  IDirect3DTexture9 *t = NULL;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL));
  return t;
}

static void write_solid(IDirect3DTexture9 *dst, DWORD colour) {
  IDirect3DTexture9 *src = NULL;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, NULL));
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DTexture9_LockRect(src, 0, &lr, NULL, 0));
  for (int y = 0; y < 64; y++) {
    DWORD *row = (DWORD *)((BYTE *)lr.pBits + (size_t)y * lr.Pitch);
    for (int x = 0; x < 64; x++)
      row[x] = colour;
  }
  IDirect3DTexture9_UnlockRect(src, 0);
  X_CHECK(IDirect3DDevice9_UpdateTexture(x_dev, (IDirect3DBaseTexture9 *)src, (IDirect3DBaseTexture9 *)dst));
  IDirect3DTexture9_Release(src);
}

static void frame(int n) {
  if (n == 3)
    write_solid(late_tex, LATE);
  IDirect3DTexture9 *fresh = make_default();

  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, BG, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  x_draw((IDirect3DBaseTexture9 *)fresh, -0.9f, -0.5f, -0.4f, 0.5f);
  x_draw((IDirect3DBaseTexture9 *)late_tex, -0.25f, -0.5f, 0.25f, 0.5f);
  x_draw((IDirect3DBaseTexture9 *)control_tex, 0.4f, -0.5f, 0.9f, 0.5f);
  IDirect3DDevice9_EndScene(x_dev);
  IDirect3DTexture9_Release(fresh);

  if (n == 1 || n == 3) {
    XReadback r = x_read(x_backBuffer);
    x_expect("unwritten texture created this frame", &r, 112, 240, 0u, 0x00ffffffu);
    x_expect(n == 1 ? "texture not written yet" : "texture written in this frame", &r, 320, 240,
             n == 1 ? 0u : LATE, 0x00ffffffu);
    x_expect("written texture (control)", &r, 528, 240, CONTROL, 0x00ffffffu);
    x_expect("background", &r, 320, 40, BG, 0x00ffffffu);
    x_done(&r);
    if (n == 3)
      x_report("code_unwritten_ab");
  }
}

int main(void) {
  x_init("d9mt_unwritten_ab");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  late_tex = make_default();
  control_tex = make_default();
  write_solid(control_tex, CONTROL);
  return x_loop(frame);
}
