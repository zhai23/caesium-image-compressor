# stb

Source: https://github.com/nothings/stb

Pinned revision: `2c980bb59875b0d32144a71867fbdebb2f77cd20`.
Vendored files are unmodified. This project uses the MIT license option in
`LICENSE` and in each header. Only the TGA decoder is enabled in stb_image.
stb_image_write supplies the RLE TGA encoder. Qt handles filesystem paths.

The adapter validates packet bounds, normalizes palette origins, handles the
horizontal image origin, and restores 16-bit TGA's one-bit alpha independently
of stb. Writer globals remain at their defaults for concurrent compression.
