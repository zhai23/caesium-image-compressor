# TGA support

TGA files can be imported through the file picker, drag and drop, directory
scanning, advanced import, and the CLI. Both preview panes, resizing, original
caching, restoration, and Cocos Creator `.meta` synchronization use the same
image pipeline as the other supported formats.

Choose **Same as input** to recompress a TGA, or **TGA** to convert another
supported format to TGA. The **TGA** section after TIFF in the **Compression**
tab offers **RLE (lossless)** (default) and **Uncompressed**. This section remains
available in both quality and target-size modes. The choice is saved and changing
it refreshes the automatic preview when enabled. Output is 24-bit RGB or 32-bit RGBA.
Alpha values, including fully transparent pixels' RGB values, are
retained when no resizing occurs. Quality sliders do not affect TGA output.
Resizing changes pixels and is performed only when explicitly enabled.

Examples:

```text
caesium-cli compress ./textures --format same --wait
caesium-cli compress ./textures --format tga --wait
caesium-cli compress ./textures --format tga --tga-compression none --wait
caesium-cli settings set tga_compression rle
caesium-cli compress ./textures --format png --lossless --wait
```

RLE does not always reduce file size. The existing **Skip if bigger** option
still applies, including when selecting Uncompressed. In target-size mode, TGA output that exceeds the requested limit
fails with an explanation and leaves existing output/source files untouched;
the program does not silently reduce quality or dimensions.

The reader accepts uncompressed and RLE truecolor, grayscale, and indexed TGA,
including legacy files without the TGA 2.0 footer. Supported source depths are
15/16/24/32-bit truecolor, 8/16-bit grayscale, and 8/16-bit palette indices with
15/16/24/32-bit palette entries. Both origin directions, nonzero palette origins,
and 16-bit truecolor's declared one-bit alpha are handled. Interleaved images,
invalid palette indices, truncated/overlong RLE packets, and excessive sizes are
rejected. Input files are limited to 500 MiB and decoded RGBA data to 512 MiB.

stb does not preserve TGA Image ID or TGA 2.0 extension/developer metadata.
Re-encoding a TGA containing that metadata while **Keep metadata** is enabled
fails explicitly; disable the option to allow pixel-only TGA output. Conversion
to other formats does not transfer TGA-specific metadata. Original caching and
restoration preserve the original file bytes, including metadata.

The stb headers are pinned and vendored in `third_party/stb`, under their MIT
license option. No additional runtime DLL or Qt TGA plugin is required.
