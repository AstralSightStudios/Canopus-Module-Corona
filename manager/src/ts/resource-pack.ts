const RESOURCE_PACK_FORMAT = "canopus-resource-pack";
const RESOURCE_PACK_VERSION = 1;
const MAX_MANIFEST_BYTES = 64 * 1024;
const MAX_MAPPING_RULES = 64;
const MAX_CONFIG_BYTES = 32 * 1024;
const MAX_PATH_BYTES = 256;
const THEME_ROOT = "/data/quickapp/files/ng.lst.corona/themes/";

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
  return typeof value === "string" && /^[a-z0-9_-]{1,12}$/.test(value);
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

function validAbsolutePath(value: string): boolean {
  if (!value || value[0] !== "/" || utf8Length(value) >= MAX_PATH_BYTES) return false;
  let segmentStart = 1;
  for (let i = 1; i <= value.length; i++) {
    if (i < value.length) {
      const code = value.charCodeAt(i);
      if (code < 32 || code === 127 || value[i] === "\\" || value[i] === ":") return false;
    }
    if (i === value.length || value[i] === "/") {
      const segment = value.slice(segmentStart, i);
      if ((!segment && i < value.length) || segment === "." || segment === "..") return false;
      segmentStart = i + 1;
    }
  }
  return true;
}

function validRelativeDestination(value: string): boolean {
  if (!value || value[0] === "/" || utf8Length(value) >= MAX_PATH_BYTES) return false;
  let segmentStart = 0;
  for (let i = 0; i <= value.length; i++) {
    if (i < value.length) {
      const code = value.charCodeAt(i);
      if (code < 32 || code === 127 || value[i] === "\\" || value[i] === ":") return false;
    }
    if (i === value.length || value[i] === "/") {
      const segment = value.slice(segmentStart, i);
      if ((!segment && i < value.length) || segment === "." || segment === "..") return false;
      segmentStart = i + 1;
    }
  }
  return true;
}

function parseMappings(value: unknown, themeId: string): ResourcePackMapping[] {
  if (!Array.isArray(value) || value.length > MAX_MAPPING_RULES)
    throw new Error(`mappings 必须是最多 ${MAX_MAPPING_RULES} 条的数组`);

  const seenSources = new Set<string>();
  const mappings = value.map((item, index) => {
    if (!isRecord(item)) throw new Error(`mappings[${index}] 格式无效`);
    const source = requiredText(item.source, `mappings[${index}].source`, MAX_PATH_BYTES - 1);
    const destination = requiredText(item.destination,
      `mappings[${index}].destination`, MAX_PATH_BYTES - 1);
    if (!validAbsolutePath(source) || !validRelativeDestination(destination))
      throw new Error(`mappings[${index}] source 必须为绝对路径，destination 必须为安全的包内相对路径`);
    const resolvedDestination = `${THEME_ROOT}${themeId}/${destination}`;
    if (!validAbsolutePath(resolvedDestination))
      throw new Error(`mappings[${index}].destination 展开后超过设备路径限制`);
    if ((source[source.length - 1] === "/") !==
        (destination[destination.length - 1] === "/"))
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
  const destinationRoot = `${THEME_ROOT}${themeId}/`;
  return mappings.map(rule =>
    `${rule.source}\t${destinationRoot}${rule.destination}\n`).join("");
}

/** Parse and validate the on-device canora.json before registering or displaying a pack. */
export function parseResourcePackManifest(text: string, expectedThemeId?: string): ResourcePackManifest {
  if (typeof text !== "string" || text.charCodeAt(0) === 0xfeff ||
      utf8Length(text) > MAX_MANIFEST_BYTES)
    throw new Error("canora.json 编码无效或超过 64 KiB");

  let value: unknown;
  try { value = JSON.parse(text); }
  catch (_error) { throw new Error("canora.json JSON 格式无效"); }
  if (!isRecord(value) || value.format !== RESOURCE_PACK_FORMAT ||
      value.formatVersion !== RESOURCE_PACK_VERSION)
    throw new Error("不支持的资源包格式或版本");
  if (!validThemeId(value.themeId) ||
      (expectedThemeId !== undefined && value.themeId !== expectedThemeId))
    throw new Error("资源包 themeId 无效或与接收目录不匹配");

  const name = requiredText(value.name, "name", 128);
  const version = optionalText(value.version, "version", 64);
  const author = optionalText(value.author, "author", 128);
  const description = optionalText(value.description, "description", 1024);
  let targets: string[] | undefined;
  if (value.targets !== undefined) {
    if (!Array.isArray(value.targets) || value.targets.length > 16)
      throw new Error("targets 必须是最多 16 项的数组");
    targets = value.targets.map((target, index) =>
      requiredText(target, `targets[${index}]`, 128));
  }
  const mappings = parseMappings(value.mappings, value.themeId);

  return {
    format: RESOURCE_PACK_FORMAT,
    formatVersion: RESOURCE_PACK_VERSION,
    themeId: value.themeId,
    name,
    ...(version === undefined ? {} : { version }),
    ...(author === undefined ? {} : { author }),
    ...(description === undefined ? {} : { description }),
    ...(targets === undefined ? {} : { targets }),
    mappings
  };
}

/** Build the module's active TSV configuration from validated manifest rules. */
export function serializeResourcePackMappings(manifest: ResourcePackManifest): string {
  return serializeMappings(manifest.mappings, manifest.themeId);
}
