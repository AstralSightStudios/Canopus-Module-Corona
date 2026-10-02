import {
  THEME_DESTINATION_ROOT, QUICKAPP_ICON_SOURCE_PREFIX, isBinFileDestination,
  isQuickAppIconSource, safeResourceSource, safeRelativeResourcePath, safeThemeDestination,
  validQuickAppPackage
} from "./resource-path";

export const RESOURCE_PACK_MANIFEST_FILENAME = "corona.json";
export const LEGACY_RESOURCE_PACK_MANIFEST_FILENAME = "canora.json";

/** Only these exact root filenames are manifests. */
export function isResourcePackManifestFilename(path: string): boolean {
  return path === RESOURCE_PACK_MANIFEST_FILENAME || path === LEGACY_RESOURCE_PACK_MANIFEST_FILENAME;
}

/** Neither manifest filename may be used as an asset or an asset directory. */
export function isReservedResourcePackPath(path: string): boolean {
  return isResourcePackManifestFilename(path.split("/")[0]);
}

/** Preserve legacy installed packs, but never hide ambiguity or invalid canonical metadata. */
export async function readResourcePackManifest(
  rootUri: string,
  themeId: string,
  file: { readOptionalText(uri: string): Promise<string | null> },
  checkCurrent: () => void = () => {},
): Promise<ResourcePackManifest | null> {
  const canonical = await file.readOptionalText(`${rootUri}${RESOURCE_PACK_MANIFEST_FILENAME}`);
  checkCurrent();
  const legacy = await file.readOptionalText(`${rootUri}${LEGACY_RESOURCE_PACK_MANIFEST_FILENAME}`);
  checkCurrent();
  if (canonical !== null && legacy !== null)
    throw new Error("资源包不能同时包含 corona.json 和 canora.json");
  const text = canonical === null ? legacy : canonical;
  return text === null ? null : parseResourcePackManifest(text, themeId);
}

const RESOURCE_PACK_FORMAT = "canopus-resource-pack";
const RESOURCE_PACK_VERSION = 1;
const MAX_MANIFEST_BYTES = 64 * 1024;
const MAX_MAPPING_RULES = 256;
const MAX_CONFIG_BYTES = 32 * 1024;
const MAX_PATH_BYTES = 256;

export interface ResourcePackMapping {
  source: string;
  destination: string;
}

export interface ResourcePackManifest {
  format: "canopus-resource-pack";
  formatVersion: 1;
  themeId: string;
  name: string;
  version?: string;
  versionCode?: number;
  author?: string;
  description?: string;
  targets?: string[];
  mappings: ResourcePackMapping[];
}

function isRecord(value: unknown): value is { [key: string]: unknown } {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function utf8Length(value: string): number {
  let size = 0;
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i);
    if (code <= 0x7f) size += 1;
    else if (code <= 0x7ff) size += 2;
    else if (code >= 0xd800 && code <= 0xdbff && i + 1 < value.length &&
             value.charCodeAt(i + 1) >= 0xdc00 && value.charCodeAt(i + 1) <= 0xdfff) {
      size += 4;
      i++;
    } else size += 3;
  }
  return size;
}

function validThemeId(value: unknown): value is string {
  return typeof value === "string" && /^[a-z0-9_-]{1,64}$/.test(value);
}

function requiredText(value: unknown, label: string, maxBytes: number): string {
  if (typeof value !== "string" || !value || utf8Length(value) > maxBytes)
    throw new Error(`${label} 必须是 1-${maxBytes} 字节的字符串`);
  return value;
}

function optionalText(value: unknown, label: string, maxBytes: number): string | undefined {
  if (value === undefined) return undefined;
  if (typeof value !== "string" || utf8Length(value) > maxBytes)
    throw new Error(`${label} 必须是最多 ${maxBytes} 字节的字符串`);
  return value;
}

function parseMappings(value: unknown, quickappIcons: unknown, themeId: string): ResourcePackMapping[] {
  if (!Array.isArray(value) || value.length > MAX_MAPPING_RULES)
    throw new Error(`mappings 必须是最多 ${MAX_MAPPING_RULES} 条的数组`);
  if (quickappIcons !== undefined && !Array.isArray(quickappIcons))
    throw new Error("quickappIcons 必须是数组");
  const icons: unknown[] = quickappIcons === undefined ? [] : quickappIcons as unknown[];
  if (value.length + icons.length > MAX_MAPPING_RULES)
    throw new Error(`mappings 与 quickappIcons 合计最多 ${MAX_MAPPING_RULES} 条`);
  const normalizedIcons = icons.map((item, index) => {
    if (!isRecord(item) || !validQuickAppPackage(item.package))
      throw new Error(`quickappIcons[${index}].package 必须是字符串`);
    return { source: `${QUICKAPP_ICON_SOURCE_PREFIX}${item.package}`, destination: item.destination };
  });

  const seenSources = new Set<string>();
  const mappings = [...value, ...normalizedIcons].map((item, index) => {
    if (!isRecord(item)) throw new Error(`mappings[${index}] 格式无效`);
    const source = requiredText(item.source, `mappings[${index}].source`, MAX_PATH_BYTES - 1);
    const destination = requiredText(item.destination,
      `mappings[${index}].destination`, MAX_PATH_BYTES - 1);
    if (!safeResourceSource(source) || !safeRelativeResourcePath(destination))
      throw new Error(`mappings[${index}] source 必须为绝对路径或有效的 QuickApp 图标键，destination 必须为安全的包内相对路径`);
    if (isReservedResourcePackPath(destination))
      throw new Error(`mappings[${index}].destination 不能使用 corona.json 或 canora.json 清单路径`);
    if (isQuickAppIconSource(source) && !isBinFileDestination(destination))
      throw new Error(`mappings[${index}] QuickApp 图标目标必须为 BIN 文件`);
    const resolvedDestination = `${THEME_DESTINATION_ROOT}${themeId}/${destination}`;
    if (!safeThemeDestination(resolvedDestination))
      throw new Error(`mappings[${index}].destination 展开后超过设备路径限制`);
    if (!isQuickAppIconSource(source) && (source.endsWith("/") !== destination.endsWith("/")))
      throw new Error(`mappings[${index}] 源路径与目标路径的目录/文件类型不一致`);
    if (seenSources.has(source)) throw new Error(`mappings[${index}] source 重复`);
    seenSources.add(source);
    return { source, destination };
  });

  const serialized = serializeMappings(mappings, themeId);
  if (utf8Length(serialized) > MAX_CONFIG_BYTES)
    throw new Error("生成的 mappings.tsv 超过 32 KiB");
  return mappings;
}

function serializeMappings(mappings: ResourcePackMapping[], themeId: string): string {
  const destinationRoot = `${THEME_DESTINATION_ROOT}${themeId}/`;
  return mappings.map(rule =>
    `${rule.source}\t${destinationRoot}${rule.destination}\n`).join("");
}

/** Parse and validate the on-device corona.json (or legacy canora.json) before registering or displaying a pack. */
export function parseResourcePackManifest(text: string, expectedThemeId?: string): ResourcePackManifest {
  if (typeof text !== "string" || text.charCodeAt(0) === 0xfeff ||
      utf8Length(text) > MAX_MANIFEST_BYTES)
    throw new Error("corona.json 编码无效或超过 64 KiB");

  let value: unknown;
  try { value = JSON.parse(text); }
  catch (_error) { throw new Error("corona.json JSON 格式无效"); }
  if (!isRecord(value) || value.format !== RESOURCE_PACK_FORMAT ||
      value.formatVersion !== RESOURCE_PACK_VERSION)
    throw new Error("不支持的资源包格式或版本");
  if (!validThemeId(value.themeId) ||
      (expectedThemeId !== undefined && value.themeId !== expectedThemeId))
    throw new Error("资源包 themeId 无效或与接收目录不匹配");

  const name = requiredText(value.name, "name", 128);
  const version = optionalText(value.version, "version", 64);
  const versionCode = value.versionCode;
  if (versionCode !== undefined &&
      (typeof versionCode !== "number" || !Number.isSafeInteger(versionCode) || versionCode < 0))
    throw new Error("versionCode 必须是非负安全整数");
  const author = optionalText(value.author, "author", 128);
  const description = optionalText(value.description, "description", 1024);
  let targets: string[] | undefined;
  if (value.targets !== undefined) {
    if (!Array.isArray(value.targets) || value.targets.length > 16)
      throw new Error("targets 必须是最多 16 项的数组");
    targets = value.targets.map((target, index) =>
      requiredText(target, `targets[${index}]`, 128));
  }
  const mappings = parseMappings(value.mappings, value.quickappIcons, value.themeId);

  // Consume quickappIcons into canonical mappings only. Re-parsing serialized
  // normalized manifests must neither duplicate rules nor discard pack metadata.

  return {
    format: RESOURCE_PACK_FORMAT,
    formatVersion: RESOURCE_PACK_VERSION,
    themeId: value.themeId,
    name,
    ...(version === undefined ? {} : { version }),
    ...(versionCode === undefined ? {} : { versionCode: versionCode as number }),
    ...(author === undefined ? {} : { author }),
    ...(description === undefined ? {} : { description }),
    ...(targets === undefined ? {} : { targets }),
    mappings
  };
}

/** Reject mapping destinations that are not backed by an installed package file. */
export function validateResourcePackFiles(
  manifest: ResourcePackManifest,
  relativePaths: string[],
): void {
  for (const mapping of manifest.mappings) {
    const exists = mapping.destination.endsWith("/")
      ? relativePaths.some(path => path.startsWith(mapping.destination))
      : relativePaths.indexOf(mapping.destination) >= 0;
    if (!exists) throw new Error(`映射目标 ${mapping.destination} 不存在于资源包文件中`);
  }
}

/** Build the module's active TSV configuration from validated manifest rules. */
export function serializeResourcePackMappings(manifest: ResourcePackManifest): string {
  return serializeMappings(manifest.mappings, manifest.themeId);
}
