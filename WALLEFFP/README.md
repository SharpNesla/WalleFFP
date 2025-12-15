README - D3D9 proxy for RTX Remix

Overview:
- This project implements a Direct3D9 proxy DLL that loads an external RTX Remix d3d9 implementation and spoofs GPU capability caps.
- The proxy DLL exports Direct3DCreate9 and optionally Direct3DCreate9Ex and forwards to a renamed RTX Remix DLL (e.g. d3d9_remix.dll).
- The game loads this proxy (place it alongside the game executable). The proxy locates and loads the RTX Remix DLL (renamed) from the same directory and forwards all D3D9 calls to it.

Usage:
1. Rename the original RTX Remix d3d9.dll to the configured name (default: d3d9_remix.dll).
2. Build this project as the d3d9.dll proxy and put the built d3d9.dll next to the game executable, alongside d3d9_remix.dll.
3. Launch the game; our proxy ensures the app perceives a legacy GPU (Pixel Shader 1.4, Vertex Shader 1.1) and restricted caps.

Notes:
- If a local d3d9_remix.dll is not found, the proxy attempts to fall back to the system d3d9.dll (from the system directory).
- All strings and comments are in English only.
- This proxy always attempts to call the renamed RTX Remix d3d9 DLL first when present; otherwise it falls back to the system d3d9 implementation.

Disclaimer:
- This code is provided for educational and integration purposes. Ensure you have the rights to modify and redistribute the RTX Remix DLL per its license.