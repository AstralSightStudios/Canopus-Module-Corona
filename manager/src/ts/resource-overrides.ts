import { parseResourcePackManifest } from "./resource-pack";
import type { ResourcePackMapping } from "./resource-pack";
import {
  enumerateThemeFiles,
  mappingMatchesRelativeFile,
  readThemeFileInventory
} from "./resource-order";
import type { ResourceAssetFileApi, ThemeAssetFile } from "./resource-order";

export const RESOURCE_OVERRIDES_URI = "internal://files/resource-overrides.json";
export const DEFAULT_RESOURCE_CHOICE = "@default";
export const SYSTEM_RESOURCE_CHOICE = "@system";

const THEME_ROOT_URI = "internal://files/themes/";
const MAX_OVERRIDE_BYTES = 64 * 1024;
const MAX_CATALOG_PATHS = 8192;
const VALID_THEME_ID = /^[a-z0-9_-]{1,12}$/;

export interface ResourcePackOption {
  themeId: string;
  name: string;
  previewUri: string;
}

export interface RegisteredResourcePath {
  sourcePath: string;
  themes: ResourcePackOption[];
}

export interface ResourceOverrides {
  [sourcePath: string]: string;
}

function utf8Length(value: string): number {
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

function validSourcePath(value: unknown): value is string {
  if (typeof value !== "string" || !value || value[0] !== "/" ||
      value.endsWith("/") || utf8Length(value) >= 256) return false;
  const segments = value.slice(1).split("/");
  return segments.every(segment => segment !== "" && segment !== "." && segment !== ".." &&
    !/[\\:\u0000-\u001f\u007f]/.test(segment));
}

function validChoice(value: unknown): value is string {
  return value === DEFAULT_RESOURCE_CHOICE || value === SYSTEM_RESOURCE_CHOICE ||
    (typeof value === "string" && VALID_THEME_ID.test(value));
}

function parseOverrides(text: string | null): ResourceOverrides {
  if (text === null) return Object.create(null) as ResourceOverrides;
  if (utf8Length(text) > MAX_OVERRIDE_BYTES) throw new Error("混搭微调配置超过 64 KiB");
  let value: unknown;
  try { value = JSON.parse(text); }
  catch (_error) { throw new Error("混搭微调配置损坏"); }
  if (typeof value !== "object" || value === null || Array.isArray(value) ||
      (value as { version?: unknown }).version !== 1 ||
      typeof (value as { overrides?: unknown }).overrides !== "object" ||
      (value as { overrides?: unknown }).overrides === null ||
      Array.isArray((value as { overrides?: unknown }).overrides))
    throw new Error("混搭微调配置格式无效");

  const source = (value as { overrides: { [key: string]: unknown } }).overrides;
  const overrides = Object.create(null) as ResourceOverrides;
  for (const sourcePath of Object.keys(source)) {
    if (!validSourcePath(sourcePath) || !validChoice(source[sourcePath]))
      throw new Error("混搭微调配置包含无效路径或选项");
    overrides[sourcePath] = source[sourcePath] as string;
  }
  return overrides;
}

function serializeOverrides(overrides: ResourceOverrides): string {
  const ordered: ResourceOverrides = Object.create(null) as ResourceOverrides;
  Object.keys(overrides).sort().forEach(sourcePath => {
    const choice = overrides[sourcePath];
    if (!validSourcePath(sourcePath) || !validChoice(choice))
      throw new Error("混搭微调路径或选项无效");
    ordered[sourcePath] = choice;
  });
  const text = JSON.stringify({ version: 1, overrides: ordered });
  if (utf8Length(text) > MAX_OVERRIDE_BYTES) throw new Error("混搭微调配置超过 64 KiB");
  return text;
}

function sourceRuleMatches(source: string, resourcePath: string): boolean {
  return source.endsWith("/") ? resourcePath.startsWith(source) : resourcePath === source;
}

function concreteSourcePath(mapping: ResourcePackMapping, relativePath: string): string | null {
  if (!mappingMatchesRelativeFile(mapping, relativePath)) return null;
  if (!mapping.destination.endsWith("/")) return mapping.source;
  const suffix = relativePath.slice(mapping.destination.length);
  if (!suffix) return null;
  const sourcePath = `${mapping.source}${suffix}`;
  return validSourcePath(sourcePath) ? sourcePath : null;
}

interface RegisteredThemeFile {
  sourcePath: string;
  relativePath: string;
}

function registeredFilesForTheme(
  mappings: ResourcePackMapping[],
  inventory: ThemeAssetFile[],
): RegisteredThemeFile[] {
  const files = new Map<string, string>();
  for (const mapping of mappings) {
    for (const asset of inventory) {
      const sourcePath = concreteSourcePath(mapping, asset.relativePath);
      if (!sourcePath || mappings.some(other => other !== mapping &&
          other.source.length > mapping.source.length &&
          sourceRuleMatches(other.source, sourcePath))) continue;
      files.set(sourcePath, asset.relativePath);
    }
  }
  return Array.from(files.keys()).map(sourcePath => ({
    sourcePath,
    relativePath: files.get(sourcePath) as string
  }));
}

/** Lists concrete replacement files registered by every installed resource pack. */
export async function loadRegisteredResourcePaths(
  installedThemeIds: string[],
  file: ResourceAssetFileApi,
): Promise<RegisteredResourcePath[]> {
  const byPath = new Map<string, Map<string, ResourcePackOption>>();
  for (const themeId of installedThemeIds) {
    const text = await file.readOptionalText(`${THEME_ROOT_URI}${themeId}/canora.json`);
    if (text === null) throw new Error(`资源包 ${themeId} 缺少 canora.json，无法读取混搭路径`);
    let manifest;
    try { manifest = parseResourcePackManifest(text, themeId); }
    catch (error) {
      throw new Error(`资源包 ${themeId} 的 manifest 无效：${String((error as Error).message || error)}`);
    }
    if (!manifest.mappings.length) continue;
    let registeredFiles: RegisteredThemeFile[] = [];
    let inventoryCoversMappings = false;
    try {
      const inventory = await readThemeFileInventory(themeId, file);
      if (inventory) {
        inventoryCoversMappings = manifest.mappings.some(mapping =>
          inventory.some(asset => mappingMatchesRelativeFile(mapping, asset.relativePath)));
        registeredFiles = registeredFilesForTheme(manifest.mappings, inventory);
      }
    } catch (_error) {
      // A stale index is not authoritative; check the installed files below.
    }
    if (!inventoryCoversMappings) {
      const inventory = await enumerateThemeFiles(themeId, file);
      registeredFiles = registeredFilesForTheme(manifest.mappings, inventory);
    }
    for (const registration of registeredFiles) {
      const sourcePath = registration.sourcePath;
      let themes = byPath.get(sourcePath);
      if (!themes) {
        themes = new Map<string, ResourcePackOption>();
        byPath.set(sourcePath, themes);
      }
      const previewUri = registration.relativePath.toLowerCase().endsWith(".bin")
        ? `${THEME_ROOT_URI}${themeId}/${registration.relativePath}` : "";
      themes.set(themeId, { themeId, name: manifest.name, previewUri });
      if (byPath.size > MAX_CATALOG_PATHS)
        throw new Error("注册的替换路径超过 Manager 列表上限");
    }
  }
  return Array.from(byPath.keys()).sort().map(sourcePath => ({
    sourcePath,
    themes: Array.from((byPath.get(sourcePath) as Map<string, ResourcePackOption>).values())
  }));
}

export async function loadResourceOverrides(
  file: Pick<ResourceAssetFileApi, "readOptionalText">,
): Promise<ResourceOverrides> {
  return parseOverrides(await file.readOptionalText(RESOURCE_OVERRIDES_URI));
}

export async function saveResourceOverride(
  sourcePath: string,
  choice: string,
  file: Pick<ResourceAssetFileApi, "readOptionalText" | "writeText">,
): Promise<void> {
  if (!validSourcePath(sourcePath) || !validChoice(choice))
    throw new Error("混搭微调路径或选项无效");
  const overrides = await loadResourceOverrides(file);
  overrides[sourcePath] = choice;
  await file.writeText(RESOURCE_OVERRIDES_URI, serializeOverrides(overrides));
}

/** Removes stale paths and falls back to Default if the selected pack was removed. */
export async function reconcileResourceOverrides(
  catalog: RegisteredResourcePath[],
  file: Pick<ResourceAssetFileApi, "readOptionalText" | "writeText">,
): Promise<ResourceOverrides> {
  const overrides = await loadResourceOverrides(file);
  const registrations = new Map(catalog.map(item => [item.sourcePath,
    new Set(item.themes.map(theme => theme.themeId))]));
  let changed = false;
  for (const sourcePath of Object.keys(overrides)) {
    const registeredThemeIds = registrations.get(sourcePath);
    if (!registeredThemeIds) {
      delete overrides[sourcePath];
      changed = true;
      continue;
    }
    const choice = overrides[sourcePath];
    if (choice !== DEFAULT_RESOURCE_CHOICE && choice !== SYSTEM_RESOURCE_CHOICE &&
        !registeredThemeIds.has(choice)) {
      overrides[sourcePath] = DEFAULT_RESOURCE_CHOICE;
      changed = true;
    }
  }
  if (changed) await file.writeText(RESOURCE_OVERRIDES_URI, serializeOverrides(overrides));
  return overrides;
}

/** Changes references to a removed pack into the normal-order default. */
export async function removeThemeFromResourceOverrides(
  themeId: string,
  file: Pick<ResourceAssetFileApi, "readOptionalText" | "writeText">,
): Promise<void> {
  const overrides = await loadResourceOverrides(file);
  let changed = false;
  for (const sourcePath of Object.keys(overrides)) {
    if (overrides[sourcePath] === themeId) {
      overrides[sourcePath] = DEFAULT_RESOURCE_CHOICE;
      changed = true;
    }
  }
  if (changed) await file.writeText(RESOURCE_OVERRIDES_URI, serializeOverrides(overrides));
}
