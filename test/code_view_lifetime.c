/* Texture views whose last reference goes while the command buffer that samples them is
 * still being recorded: each frame draws 64 quads, each with its own new 1x1 texture that
 * is unbound and released right after its draw (the game streams and drops textures like
 * this when a UI window opens or closes). Metal 4 command buffers don't retain resources,
 * so a view freed at once lets the next texture's view take its resource ID: a quad then
 * showed another quad's colour, or the GPU faulted on the freed view. Every quad must show
 * its own texture's colour. */
#include "t_xfer.h"

static IDirect3DTexture9 *solid(DWORD c) {
  IDirect3DTexture9 *t;
  D3DLOCKED_RECT lr;
  X_CHECK(IDirect3DDevice9_CreateTexture(x_dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL));
  X_CHECK(IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0));
  *(DWORD *)lr.pBits = c;
  IDirect3DTexture9_UnlockRect(t, 0);
  return t;
}

static DWORD color(int n, int k) {
  return 0xff000080u | (DWORD)(k * 4) << 16 | (DWORD)((n * 7) & 0xff) << 8;
}

static void frame(int n) {
  IDirect3DDevice9_SetRenderTarget(x_dev, 0, x_backBuffer);
  IDirect3DDevice9_Clear(x_dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0);
  IDirect3DDevice9_BeginScene(x_dev);
  for (int k = 0; k < 64; k++) {
    IDirect3DTexture9 *t = solid(color(n, k));
    const float x0 = -1.0f + (k % 8) * 0.25f, y0 = -1.0f + (k / 8) * 0.25f;
    x_draw((IDirect3DBaseTexture9 *)t, x0 + 0.02f, y0 + 0.02f, x0 + 0.23f, y0 + 0.23f);
    IDirect3DDevice9_SetTexture(x_dev, 0, NULL);
    IDirect3DTexture9_Release(t);
  }
  IDirect3DDevice9_EndScene(x_dev);
  if (n <= 4) {
    XReadback r = x_read(x_backBuffer);
    for (int k = 0; k < 64; k++) {
      char what[48];
      snprintf(what, sizeof(what), "frame %d quad %d", n, k);
      /* cell centre; D3D's y axis points up, the surface's rows down */
      x_expect(what, &r, (UINT)((k % 8) * 80 + 40), (UINT)(480 - ((k / 8) * 60 + 30)), color(n, k), 0x00ffffffu);
    }
    x_done(&r);
    if (n == 4)
      x_report("code_view_lifetime");
  }
}

int main(void) {
  x_init("d9mt_view_lifetime");
  IDirect3DDevice9_SetRenderState(x_dev, D3DRS_ALPHABLENDENABLE, FALSE);
  return x_loop(frame);
}
