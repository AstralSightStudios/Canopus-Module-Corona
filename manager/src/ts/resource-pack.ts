import {
  THEME_DESTINATION_ROOT, QUICKAPP_ICON_SOURCE_PREFIX, isBinFileDestination,
  isQuickAppIconSource, safeResourceSource, safeRelativeResourcePath, safeThemeDestination,
  validQuickAppPackage, utf8PathLength
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

export type BackgroundSetting = { mode: "system" } | { mode: "blur" } | { mode: "image"; image: string };

export interface BackgroundsDeclaration {
  launcher?: BackgroundSetting;
  controlCenter?: BackgroundSetting;
  notifications?: BackgroundSetting;
  widgets?: BackgroundSetting;
}

export const CALENDAR_ITEM_NAMES = ["month", "day", "weekday"] as const;
export type CalendarItemName = typeof CALENDAR_ITEM_NAMES[number];
export type CalendarAlign = "top_left" | "top_mid" | "top_right" | "center";
/** Canonical parsed items: optional JSON placement/font properties are expanded to native defaults. */
export type CalendarItem = { type: "none" } | {
  type: "text" | "image";
  x: number;
  y: number;
  align: CalendarAlign;
  size: number;
  color?: string;
  // weekday entries are Monday first; omitted/empty text context uses the system date.
  context?: string[];
};
export type CalendarDeclaration = Partial<Record<CalendarItemName, CalendarItem>>;

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
  backgrounds?: BackgroundsDeclaration;
  calendar?: CalendarDeclaration;
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
  if (!mappings.length) return "# empty mappings\n";
  const destinationRoot = `${THEME_DESTINATION_ROOT}${themeId}/`;
  return mappings.map(rule =>
    `${rule.source}\t${destinationRoot}${rule.destination}\n`).join("");
}

/** Reject lone UTF-16 surrogates before native UTF-8 serialization (never replace them silently). */
function validCalendarUnicode(value: string): boolean {
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdbff) {
      const next = value.charCodeAt(++i);
      if (!(next >= 0xdc00 && next <= 0xdfff)) return false;
    } else if (code >= 0xdc00 && code <= 0xdfff) return false;
  }
  return true;
}

function parseCalendar(value: unknown, themeId: string): CalendarDeclaration {
  if (!isRecord(value) || Object.keys(value).some(key =>
      !(CALENDAR_ITEM_NAMES as readonly string[]).includes(key)))
    throw new Error("calendar 必须仅包含 month/day/weekday（背景仍使用普通映射）");
  const result: CalendarDeclaration = {};
  const counts = { month: 12, day: 31, weekday: 7 };
  for (const name of CALENDAR_ITEM_NAMES) {
    const item = value[name];
    if (item === undefined) continue;
    const label = `calendar.${name}`;
    if (!isRecord(item) || !["none", "text", "image"].includes(item.type as string))
      throw new Error(`${label}.type 必须为 text/image/none`);
    if (item.type === "none") {
      if (Object.keys(item).some(key => key !== "type"))
        throw new Error(`${label} none 只允许 type`);
      result[name] = { type: "none" };
      continue;
    }
    if (Object.keys(item).some(key =>
        !["type", "x", "y", "align", "size", "color", "context"].includes(key)))
      throw new Error(`${label} 包含未知字段`);
    // A partial declaration keeps the item's native placement/font, not zeroed properties.
    const x = item.x === undefined ? 0 : item.x;
    const y = item.y === undefined ? (name === "day" ? 34 : 17) : item.y;
    const align = item.align === undefined ? "top_mid" : item.align;
    const size = item.size === undefined ? 0 : item.size;
    if (typeof x !== "number" || !Number.isInteger(x) || x < -112 || x > 112 ||
        typeof y !== "number" || !Number.isInteger(y) || y < -112 || y > 112)
      throw new Error(`${label}.x/y 必须为-112..112 的整数偏移`);
    if (!["top_left", "top_mid", "top_right", "center"].includes(align as string))
      throw new Error(`${label}.align 无效`);
    if (typeof size !== "number" || !Number.isInteger(size) ||
        (size !== 0 && (size < 8 || size > 72)))
      throw new Error(`${label}.size 必须为 0 或 8..72`);
    if (item.type === "image" && (size !== 0 || item.color !== undefined))
      throw new Error(`${label} 图片使用原始尺寸，不允许非零 size 或 color`);
    if (item.color !== undefined &&
        (typeof item.color !== "string" || !/^#[0-9a-fA-F]{6}$/.test(item.color)))
      throw new Error(`${label}.color 必须为 #RRGGBB`);
    const context = item.context;
    if (context !== undefined && (!Array.isArray(context) ||
        (context.length !== counts[name] && !(item.type === "text" && context.length === 0))))
      throw new Error(`${label}.context 必须为 ${counts[name]} 项数组（text 可省略或为空）`);
    if (item.type === "image" && (!Array.isArray(context) || context.length !== counts[name]))
      throw new Error(`${label}.context 图片必须恰有 ${counts[name]} 项`);
    if (Array.isArray(context)) {
      for (const entry of context) {
        if (typeof entry !== "string") throw new Error(`${label}.context 必须为字符串数组`);
        if (!validCalendarUnicode(entry) || /[\u0000-\u001f\u007f-\u009f]/.test(entry))
          throw new Error(`${label}.context 不能包含控制字符或无效 Unicode`);
        if (item.type === "text") {
          if (utf8PathLength(entry) > 63)
            throw new Error(`${label}.context 文本最多 63 UTF-8 字节且不能包含控制字符`);
        } else if (!isBinFileDestination(entry) || isReservedResourcePackPath(entry) ||
            !safeThemeDestination(`${THEME_DESTINATION_ROOT}${themeId}/${entry}`)) {
          throw new Error(`${label}.context 图片必须为安全的包内 BIN 文件路径，展开后小于 256 字节`);
        }
      }
    }
    result[name] = {
      type: item.type as "text" | "image", x, y,
      align: align as CalendarAlign, size,
      ...(item.color === undefined ? {} : { color: (item.color as string).toUpperCase() }),
      ...(context === undefined ? {} : { context: (context as string[]).slice() })
    };
  }
  return result;
}

/** All package files referenced by calendar images, including duplicate date entries. */
export function calendarImagePaths(calendar?: CalendarDeclaration): string[] {
  const paths: string[] = [];
  for (const name of CALENDAR_ITEM_NAMES) {
    const item = calendar?.[name];
    if (item?.type === "image") paths.push(...item.context || []);
  }
  return paths;
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
  function parseBackgroundSetting(val: unknown, themeId: string, label: string): BackgroundSetting {
    if (typeof val === "string") {
      if (val === "system") return { mode: "system" };
      if (val === "blur") return { mode: "blur" };
      val = { mode: "image", image: val };
    }
    if (!isRecord(val) || (val.mode !== "system" && val.mode !== "blur" && val.mode !== "image") ||
        Object.keys(val).some(key => key !== "mode" && !(val.mode === "image" && key === "image")))
      throw new Error(`${label} 格式无效`);
    if (val.mode === "system") return { mode: "system" };
    if (val.mode === "blur") return { mode: "blur" };
    const image = val.image;
    if (typeof image !== "string" || !safeRelativeResourcePath(image) || image.endsWith("/") ||
        isReservedResourcePackPath(image) ||
        !safeThemeDestination(`${THEME_DESTINATION_ROOT}${themeId}/${image}`))
      throw new Error(`${label}.image 必须是安全的包内文件路径`);
    return { mode: "image", image };
  }

  let backgrounds: BackgroundsDeclaration | undefined;
  if (value.backgrounds !== undefined) {
    if (!isRecord(value.backgrounds) ||
        Object.keys(value.backgrounds).some(key =>
          key !== "launcher" && key !== "controlCenter" && key !== "notifications" && key !== "widgets"))
      throw new Error("backgrounds 格式无效");
    backgrounds = {};
    if (value.backgrounds.launcher !== undefined)
      backgrounds.launcher = parseBackgroundSetting(value.backgrounds.launcher, value.themeId, "backgrounds.launcher");
    if (value.backgrounds.controlCenter !== undefined)
      backgrounds.controlCenter = parseBackgroundSetting(value.backgrounds.controlCenter, value.themeId, "backgrounds.controlCenter");
    if (value.backgrounds.notifications !== undefined)
      backgrounds.notifications = parseBackgroundSetting(value.backgrounds.notifications, value.themeId, "backgrounds.notifications");
    if (value.backgrounds.widgets !== undefined)
      backgrounds.widgets = parseBackgroundSetting(value.backgrounds.widgets, value.themeId, "backgrounds.widgets");
  }
  // Legacy input is normalized here; internal consumers only use backgrounds.
  if (value.launcherBackground !== undefined) {
    const lb = parseBackgroundSetting(value.launcherBackground, value.themeId, "launcherBackground");
    if (!backgrounds) backgrounds = {};
    if (!backgrounds.launcher) backgrounds.launcher = lb;
  }
  const calendar = value.calendar === undefined ? undefined : parseCalendar(value.calendar, value.themeId);

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
    mappings,
    ...(calendar === undefined ? {} : { calendar }),
    ...(backgrounds === undefined ? {} : { backgrounds })
  };
}

/** Reject mapping destinations that are not backed by an installed package file. */
export function validateResourcePackFiles(
  manifest: ResourcePackManifest,
  relativePaths: string[],
): void {
  for (const path of calendarImagePaths(manifest.calendar)) {
    if (!relativePaths.includes(path))
      throw new Error(`calendar 图片 ${path} 不存在于资源包文件中`);
  }
  if (manifest.backgrounds) {
    for (const [key, bg] of Object.entries(manifest.backgrounds) as [string, BackgroundSetting][]) {
      if (bg.mode === "image" && !relativePaths.includes(bg.image))
        throw new Error(`backgrounds.${key}.image 不存在于资源包文件中`);
    }
  }
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
