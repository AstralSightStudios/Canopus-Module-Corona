import { THEME_DESTINATION_ROOT } from "./resource-path";
import { readProtectedThemeIds } from "./resource-activation";
import { removeThemeFileInventory, removeThemeFromResourceOrder } from "./resource-order";
import { getResourceFileApi, invalidateResourceCatalog, removeThemeFromResourceOverrides, withResourceOperation } from "./resource-overrides";

export interface ResourceStorageFileApi {
  readOptionalText(uri: string): Promise<string | null>;
  writeText(uri: string, text: string): Promise<void>;
  removeDirectory(uri: string): Promise<void>;
  isFileNotFound(error: unknown): boolean;
}

export interface ResourceStorageError extends Error {
  resourceStorageReason: "invalid-theme-id" | "invalid-index" | "active-theme";
}

const INSTALLED_THEMES_URI = "internal://files/interconnect-themes.json";
const MAPPINGS_URI = "internal://files/mappings.tsv";
const THEME_ROOT_URI = "internal://files/themes/";
const VALID_THEME_ID = /^[a-z0-9_-]{1,64}$/;

function storageError(
  reason: ResourceStorageError["resourceStorageReason"],
  message: string,
): ResourceStorageError {
  const error = new Error(message) as ResourceStorageError;
  error.resourceStorageReason = reason;
  return error;
}

function isThemeId(value: unknown): value is string {
  return typeof value === "string" && VALID_THEME_ID.test(value);
}

function mappingsUseTheme(mappings: string, themeId: string): boolean {
  const destinationPrefix = `${THEME_DESTINATION_ROOT}${themeId}/`;
  return mappings.split(/\r?\n/).some((line) => {
    if (!line || line[0] === "#") return false;
    const separator = line.indexOf("\t");
    return separator > 0 && line.slice(separator + 1).startsWith(destinationPrefix);
  });
}

/** Removes an installed resource pack and its index entry unless it is active. */
export function removeInstalledTheme(
  themeId: string,
  file: ResourceStorageFileApi,
): Promise<void> {
  const storageFile = getResourceFileApi(file);
  return withResourceOperation(() => removeInstalledThemeInOperation(themeId, storageFile));
}

async function removeInstalledThemeInOperation(
  themeId: string,
  file: ResourceStorageFileApi,
): Promise<void> {
  if (!isThemeId(themeId))
    throw storageError("invalid-theme-id", "资源包标识无效");

  const indexText = await file.readOptionalText(INSTALLED_THEMES_URI);
  let installedThemeIds: string[] = [];
  if (indexText !== null) {
    let parsed: unknown;
    try {
      parsed = JSON.parse(indexText);
    } catch (_error) {
      throw storageError("invalid-index", "资源包索引损坏");
    }
    if (!Array.isArray(parsed) || parsed.some((item) => !isThemeId(item)))
      throw storageError("invalid-index", "资源包索引格式无效");
    installedThemeIds = parsed as string[];
  }

  const mappings = await file.readOptionalText(MAPPINGS_URI);
  const protectedThemeIds = await readProtectedThemeIds(file);
  if ((mappings !== null && mappingsUseTheme(mappings, themeId)) ||
      protectedThemeIds.includes(themeId))
    throw storageError("active-theme", "正在使用的资源包不能删除");

  invalidateResourceCatalog();
  try {
    await removeThemeFromResourceOverrides(themeId, file);
    try {
      await file.removeDirectory(`${THEME_ROOT_URI}${themeId}/`);
    } catch (error) {
      if (!file.isFileNotFound(error)) throw error;
    }

    const remainingThemeIds = installedThemeIds.filter((item) => item !== themeId);
    if (indexText !== null && remainingThemeIds.length !== installedThemeIds.length)
      await file.writeText(INSTALLED_THEMES_URI, JSON.stringify(remainingThemeIds));
    await removeThemeFromResourceOrder(themeId, file);
    await removeThemeFileInventory(themeId, file);
  } finally {
    // Partial failures must not leave a catalog referring to deleted files.
    invalidateResourceCatalog();
  }
}
