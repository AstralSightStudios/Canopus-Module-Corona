# Resource image editing

`resource_image.py` extracts regular files from ROMFS and converts a restricted
LVGL v9 I8 format to/from transparent RGBA PNG. Install Pillow in your Python
environment (`python3 -m pip install Pillow`). Run commands from the repository root.

## Extract and decode

```sh
mkdir -p build/my-icon
python3 tools/resource_image.py extract \
  ~/develop/temp/extract155/vela_resource.bin \
  /resource/app/common/icon/confirm.bin build/my-icon/confirm.bin
python3 tools/resource_image.py decode \
  build/my-icon/confirm.bin build/my-icon/confirm.png
```

The extractor accepts `/resource/...` or a ROMFS-internal `/app/...` path.
It reads but never modifies firmware, does not follow links, and does not
validate ROMFS checksums or firmware authenticity. All commands refuse to
overwrite an existing output file. Output parent directories must already exist.

## Edit and encode

Edit the PNG in an image editor; keep its dimensions and transparency. Save it
as `confirm-edited.png`, keeping the original BIN as the encoding template.

```sh
python3 tools/resource_image.py encode \
  build/my-icon/confirm-edited.png build/my-icon/confirm-edited.bin \
  --template build/my-icon/confirm.bin
python3 tools/resource_image.py decode \
  build/my-icon/confirm-edited.bin build/my-icon/confirm-preview.png
```

Inspect the preview before uploading. Images with more than 256 distinct RGBA
colors are rejected unless `--quantize` is supplied to `encode`. Quantization is
lossy and may affect colors and transparency. Other supported edits are lossless.
The original header and row padding are preserved. Unchanged decoded pixels
produce a byte-identical original BIN, including duplicate/unused palette entries.

Upload the edited BIN as:

```text
/data/canopus/themes/current/app/common/icon/confirm.bin
```

Use the following directory mapping (generate a literal TAB):

```sh
printf '/resource/app/common/icon/\t/data/canopus/themes/current/app/common/icon/\n' > mappings.tsv
```

Follow [INSTALL.md](../docs/INSTALL.md) for configuration validation, installation
and activation.
Do not change theme files while an active resource transaction is running.

## Scope and verification

Supported header: magic `0x19`, color format `0x0A` (I8), zero flags/reserved,
12-byte header, 256 BGRA palette entries, one index byte per pixel. Width, height,
stride and exact file size are checked. Compressed formats, other color formats,
watchface bundles and resizing are intentionally unsupported. BGRA interpretation
follows LVGL's `lv_color32_t` layout. The user reports that a custom Settings
launcher icon was visibly displayed on `.155` after installing the current signed
module with the temporary settings-icon installer; see the narrowly scoped
[device observation](../docs/INSTALL.md#155-用户报告的单项实机记录).
This does not validate all palette colors/alpha values, every converted image,
other resource owners, or `.139` hardware.

The local `.155` resource image's `confirm.bin` (48x48), `cancel.bin` (48x48),
and `prompt.bin` (80x80) passed byte-exact BIN -> saved PNG -> BIN checks.
These round trips demonstrate conversion consistency, not hardware acceptance
of those three images. The separate Settings-icon observation must not be
extended to the example `confirm.bin` workflow or arbitrary edited images.

Run synthetic tests without firmware:

```sh
python3 tests/test_resource_image.py
```
