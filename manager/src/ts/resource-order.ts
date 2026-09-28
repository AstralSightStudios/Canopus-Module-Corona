import type { ResourcePackMapping } from "./resource-pack";

export const SYSTEM_STYLE_ID = "@system";
export const INSTALLED_THEMES_URI = "internal://files/interconnect-themes.json";
export const RESOURCE_ORDER_URI = "internal://files/resource-order.json";
export const RESOURCE_FILES_URI = "internal://files/resource-files.json";

const VALID_THEME_ID = /^[a-z0-9_-]{1,12}$/;
const MAX_THEME_FILES = 128;
const MAX_FILE_INDEX_BYTES = 2 * 1024 * 1024;

export interface ResourceOrderFileApi {
  readOptionalText(uri: string): Promise<string | null>;
  writeText(uri: string, text: string): Promise<void>;
}

export async function readInstalledThemeIds(file: Pick<ResourceOrderFileApi, "readOptionalText">): Promise<string[]> {
  const text = await file.readOptionalText(INSTALLED_THEMES_URI);
  if (text === null) return [];
  let value: unknown;
  try { value = JSON.parse(text); }
  catch (_error) { throw new Error("已安装资源包索引损坏"); }
  if (!Array.isArray(value) || value.some(item => !validThemeId(item)) ||
      new Set(value).size !== value.length)
    throw new Error("已安装资源包索引格式无效");
  return value as string[];
}

export interface ThemeAssetFile {
  relativePath: string;
  sizeBytes: number;
}

export interface ResourceAssetFileApi extends ResourceOrderFileApi {
  readFileInfo(uri: string): Promise<{ length: number; type?: string }>;
  listDirectory(uri: string): Promise<Array<{ uri: string; length: number }>>;
  readArrayBuffer(uri: string, position?: number, length?: number): Promise<Uint8Array>;
  writeArrayBuffer(uri: string, buffer: Uint8Array, position?: number): Promise<void>;
  makeDirectory(uri: string, recursive?: boolean): Promise<void>;
  deleteFile(uri: string): Promise<void>;
  removeDirectory(uri: string): Promise<void>;
  isFileNotFound(error: unknown): boolean;
}

interface ThemeFileRecord {
  [themeId: string]: ThemeAssetFile[];
}

function validThemeId(value: unknown): value is string {
  return typeof value === "string" && VALID_THEME_ID.test(value);
}

function validRelativePath(value: unknown): value is string {
  if (typeof value !== "string" || !value || value[0] === "/" || value.length > 255) return false;
  const segments = value.split("/");
  return segments.every(segment => segment !== "" && segment !== "." && segment !== ".." &&
    !/[\\:\u0000-\u001f\u007f]/.test(segment));
}

function parseOrder(text: string): string[] {
  let value: unknown;
  try { value = JSON.parse(text); }
  catch (_error) { throw new Error("资源排序记录损坏"); }
  if (typeof value !== "object" || value === null || Array.isArray(value) ||
      (value as { version?: unknown }).version !== 1 ||
      !Array.isArray((value as { order?: unknown }).order))
    throw new Error("资源排序记录格式无效");
  const order = (value as { order: unknown[] }).order;
  if (order.filter(item => item === SYSTEM_STYLE_ID).length !== 1 ||
      order.some(item => item !== SYSTEM_STYLE_ID && !validThemeId(item)) ||
      new Set(order).size !== order.length)
    throw new Error("资源排序记录包含无效或重复项目");
  return order as string[];
}

export function serializeResourceOrder(order: string[]): string {
  if (order.filter(item => item === SYSTEM_STYLE_ID).length !== 1 ||
      order.some(item => item !== SYSTEM_STYLE_ID && !validThemeId(item)) ||
      new Set(order).size !== order.length)
    throw new Error("资源顺序必须包含唯一的系统样式项及不重复的资源包");
  return JSON.stringify({ version: 1, order });
}

function normalizedOrder(order: string[], installedThemeIds: string[]): string[] {
  const installed = new Set(installedThemeIds);
  const present = new Set(order.filter(item => item !== SYSTEM_STYLE_ID && installed.has(item)));
  const newItems = installedThemeIds.filter(item => !present.has(item)).reverse();
  const retained = order.filter(item => item === SYSTEM_STYLE_ID || installed.has(item));
  return [...newItems, ...retained];
}

/** Loads/migrates ordering; newly discovered package IDs are inserted at highest priority. */
export async function loadResourceOrder(
  installedThemeIds: string[],
  file: ResourceOrderFileApi,
): Promise<string[]> {
  if (installedThemeIds.some(item => !validThemeId(item)) ||
      new Set(installedThemeIds).size !== installedThemeIds.length)
    throw new Error("已安装资源包索引无效");
  const text = await file.readOptionalText(RESOURCE_ORDER_URI);
  const order = text === null
    ? [...installedThemeIds, SYSTEM_STYLE_ID]
    : normalizedOrder(parseOrder(text), installedThemeIds);
  const serialized = serializeResourceOrder(order);
  if (text !== serialized) await file.writeText(RESOURCE_ORDER_URI, serialized);
  return order;
}

/** Adds a newly received theme at the top; replacing an ordered theme preserves its slot. */
export async function registerThemeInResourceOrder(
  themeId: string,
  previouslyInstalledIds: string[],
  file: ResourceOrderFileApi,
): Promise<void> {
  if (!validThemeId(themeId)) throw new Error("资源包标识无效");
  const text = await file.readOptionalText(RESOURCE_ORDER_URI);
  const baseline = text === null
    ? [...previouslyInstalledIds, SYSTEM_STYLE_ID]
    : parseOrder(text);
  const replacement = baseline.indexOf(themeId) >= 0;
  const allowed = new Set([...previouslyInstalledIds, themeId]);
  let order = baseline.filter(item => item === SYSTEM_STYLE_ID || allowed.has(item));
  const present = new Set(order);
  const missing = previouslyInstalledIds.filter(item => !present.has(item)).reverse();
  order = [...missing, ...order];
  if (!replacement) order = [themeId, ...order.filter(item => item !== themeId)];
  await file.writeText(RESOURCE_ORDER_URI, serializeResourceOrder(order));
}

export async function removeThemeFromResourceOrder(
  themeId: string,
  file: ResourceOrderFileApi,
): Promise<void> {
  const text = await file.readOptionalText(RESOURCE_ORDER_URI);
  if (text === null) return;
  const order = parseOrder(text).filter(item => item !== themeId);
  if (!order.includes(SYSTEM_STYLE_ID)) order.push(SYSTEM_STYLE_ID);
  await file.writeText(RESOURCE_ORDER_URI, serializeResourceOrder(order));
}

/** Persists the transfer's file inventory, enabling directory-rule pack overlays. */
export async function writeThemeFileInventory(
  themeId: string,
  files: ThemeAssetFile[],
  file: ResourceOrderFileApi,
): Promise<void> {
  if (!validThemeId(themeId) || files.length > MAX_THEME_FILES ||
      files.some(item => !validRelativePath(item.relativePath) ||
        !Number.isSafeInteger(item.sizeBytes) || item.sizeBytes < 0 ||
        item.sizeBytes > 64 * 1024 * 1024) ||
      new Set(files.map(item => item.relativePath)).size !== files.length)
    throw new Error("资源包文件清单无效");
  const text = await file.readOptionalText(RESOURCE_FILES_URI);
  const records = text === null ? {} : parseThemeFileRecords(text);
  records[themeId] = files.map(item => ({ relativePath: item.relativePath, sizeBytes: item.sizeBytes }));
  const serialized = JSON.stringify({ version: 1, themes: records });
  if (serialized.length > MAX_FILE_INDEX_BYTES) throw new Error("资源文件索引超过 2 MiB");
  await file.writeText(RESOURCE_FILES_URI, serialized);
}

function parseThemeFileRecords(text: string): ThemeFileRecord {
  if (text.length > MAX_FILE_INDEX_BYTES) throw new Error("资源文件索引超过 2 MiB");
  let value: unknown;
  try { value = JSON.parse(text); }
  catch (_error) { throw new Error("资源文件索引损坏"); }
  if (typeof value !== "object" || value === null || Array.isArray(value) ||
      (value as { version?: unknown }).version !== 1 ||
      typeof (value as { themes?: unknown }).themes !== "object" ||
      (value as { themes?: unknown }).themes === null ||
      Array.isArray((value as { themes?: unknown }).themes))
    throw new Error("资源文件索引格式无效");
  const source = (value as { themes: { [key: string]: unknown } }).themes;
  const result: ThemeFileRecord = {};
  for (const themeId of Object.keys(source)) {
    const entries = source[themeId];
    if (!validThemeId(themeId) || !Array.isArray(entries) || entries.length > MAX_THEME_FILES ||
        entries.some(item => typeof item !== "object" || item === null ||
          !validRelativePath((item as { relativePath?: unknown }).relativePath) ||
          !Number.isSafeInteger((item as { sizeBytes?: unknown }).sizeBytes) ||
          (item as { sizeBytes: number }).sizeBytes < 0 ||
          (item as { sizeBytes: number }).sizeBytes > 64 * 1024 * 1024) ||
        new Set(entries.map(item => (item as ThemeAssetFile).relativePath).values()).size !== entries.length)
      throw new Error(`资源文件索引中的 ${themeId} 条目无效`);
    result[themeId] = entries as ThemeAssetFile[];
  }
  return result;
}

export async function readThemeFileInventory(
  themeId: string,
  file: ResourceOrderFileApi,
): Promise<ThemeAssetFile[] | null> {
  const text = await file.readOptionalText(RESOURCE_FILES_URI);
  if (text === null) return null;
  const record = parseThemeFileRecords(text);
  return record[themeId] || null;
}

export async function removeThemeFileInventory(
  themeId: string,
  file: ResourceOrderFileApi,
): Promise<void> {
  const text = await file.readOptionalText(RESOURCE_FILES_URI);
  if (text === null) return;
  const records = parseThemeFileRecords(text);
  if (!records[themeId]) return;
  delete records[themeId];
  await file.writeText(RESOURCE_FILES_URI, JSON.stringify({ version: 1, themes: records }));
}

export function mappingMatchesRelativeFile(mapping: ResourcePackMapping, relativePath: string): boolean {
  if (mapping.destination.endsWith("/")) return relativePath.startsWith(mapping.destination);
  return relativePath === mapping.destination;
}
