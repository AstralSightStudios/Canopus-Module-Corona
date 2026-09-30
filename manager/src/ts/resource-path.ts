/** TSV destinations are relative to QuickApp internal://files/, never native paths. */
export const THEME_DESTINATION_ROOT = "themes/";
// Band 11 has the longest supported native files root (35 UTF-8 bytes).
// Budget for expansion without depending on the device's actual native path.
export const MAX_EXPANDED_PATH_BYTES = 255;
export const WORST_CASE_FILES_ROOT_BYTES = 35;

export function utf8PathLength(value: string): number {
  let bytes = 0;
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i);
    if (code <= 0x7f) bytes++;
    else if (code <= 0x7ff) bytes += 2;
    else if (code >= 0xd800 && code <= 0xdbff && i + 1 < value.length &&
        value.charCodeAt(i + 1) >= 0xdc00 && value.charCodeAt(i + 1) <= 0xdfff) {
      bytes += 4;
      i++;
    } else bytes += 3;
  }
  return bytes;
}

/** A trailing slash denotes a directory; all other empty segments are forbidden. */
export function safeRelativeResourcePath(value: string): boolean {
  if (!value || value[0] === "/" || /[\\:\u0000-\u001f\u007f]/.test(value)) return false;
  const segments = value.split("/");
  return segments.every((segment, index) =>
    (segment !== "" || index === segments.length - 1) && segment !== "." && segment !== "..");
}

export function safeThemeDestination(value: string): boolean {
  return value.startsWith(THEME_DESTINATION_ROOT) &&
    value.length > THEME_DESTINATION_ROOT.length && safeRelativeResourcePath(value) &&
    utf8PathLength(value) + WORST_CASE_FILES_ROOT_BYTES <= MAX_EXPANDED_PATH_BYTES;
}
