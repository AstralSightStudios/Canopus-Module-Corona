import type { ResourcePackMapping } from "./resource-pack";
import { safeRelativeResourcePath } from "./resource-path";

export const SYSTEM_STYLE_ID = "@system";
export const INSTALLED_THEMES_URI = "internal://files/interconnect-themes.json";
export const RESOURCE_ORDER_URI = "internal://files/resource-order.json";
export const RESOURCE_FILES_URI = "internal://files/resource-files.json";

const VALID_THEME_ID = /^[a-z0-9_-]{1,64}$/;
// Installed inventories are storage-bounded, independent of transfer file-index width.
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

export interface ResourceAssetFileInfo {
  uri?: string;
  length: number;
  type?: string;
  subFiles?: ResourceAssetFileInfo[];
}

export interface ResourceAssetFileApi extends ResourceOrderFileApi {
  readFileInfo(uri: string, recursive?: boolean): Promise<ResourceAssetFileInfo>;
  listDirectory(uri: string): Promise<Array<{ uri: string; length: number }>>;
  readArrayBuffer(uri: string, position?: number, length?: number): Promise<Uint8Array>;
  writeArrayBuffer(uri: string, buffer: Uint8Array, position?: number): Promise<void>;
  copyFile(srcUri: string, dstUri: string): Promise<void>;
  makeDirectory(uri: string, recursive?: boolean): Promise<void>;
  deleteFile(uri: string): Promise<void>;
  removeDirectory(uri: string): Promise<void>;
  isFileNotFound(error: unknown): boolean;
}

export interface ThemeFileRecord {
  [themeId: string]: ThemeAssetFile[];
}

function validThemeId(value: unknown): value is string {
  return typeof value === "string" && VALID_THEME_ID.test(value);
}

function validRelativePath(value: unknown): value is string {
  return typeof value === "string" && value.length <= 255 && !value.endsWith("/") &&
    safeRelativeResourcePath(value);
}

/** Reads a package's on-disk files without depending on the optional persisted inventory. */
export async function enumerateThemeFiles(
  themeId: string,
  file: ResourceAssetFileApi,
  isCurrent: () => boolean = () => true,
): Promise<ThemeAssetFile[]> {
  const checkCurrent = () => {
    if (!isCurrent()) throw new Error("资源目录已更新，请重试");
  };
  checkCurrent();
  const rootUri = `internal://files/themes/${themeId}/`;
  const files = new Map<string, ThemeAssetFile>();
  let totalBytes = 0;

  function addFile(uri: string, sizeBytes: number): void {
    let relativePath: string | null = null;
    if (uri.startsWith(rootUri)) relativePath = uri.slice(rootUri.length);
    else if (!uri.startsWith("/") && uri.indexOf("://") < 0)
      relativePath = uri.startsWith("./") ? uri.slice(2) : uri;
    if (relativePath && relativePath.endsWith("/")) relativePath = relativePath.slice(0, -1);
    if (!relativePath || relativePath === "mappings.tsv") return;
    if (!validRelativePath(relativePath)) throw new Error(`资源包 ${themeId} 文件路径无效：${relativePath}`);
    if (!Number.isSafeInteger(sizeBytes) || sizeBytes < 0 || sizeBytes > 64 * 1024 * 1024)
      throw new Error(`资源包 ${themeId} 文件大小无效：${relativePath}`);
    const existing = files.get(relativePath);
    if (existing) {
      if (existing.sizeBytes !== sizeBytes) throw new Error(`资源包 ${themeId} 文件清单重复：${relativePath}`);
      return;
    }
    files.set(relativePath, { relativePath, sizeBytes });
    totalBytes += sizeBytes;
    if (totalBytes > 64 * 1024 * 1024)
      throw new Error(`资源包 ${themeId} 文件清单超出限制`);
  }

  try {
    const root = await file.readFileInfo(rootUri, true);
    checkCurrent();
    if (root.type === "dir" && Array.isArray(root.subFiles) && root.subFiles.length) {
      let complete = true;
      const visit = (entries: ResourceAssetFileInfo[]) => {
        for (const entry of entries) {
          if (entry.type === "dir") {
            if (Array.isArray(entry.subFiles)) visit(entry.subFiles);
            continue;
          }
          if (entry.type !== "file" || typeof entry.uri !== "string") {
            complete = false;
            continue;
          }
          addFile(entry.uri, entry.length);
        }
      };
      visit(root.subFiles);
      if (complete && files.size) return Array.from(files.values()).sort((a, b) =>
        a.relativePath.localeCompare(b.relativePath));
    }
  } catch (_error) {
    // Fall back to list/get for runtimes without recursive get support.
  }

  checkCurrent();
  files.clear();
  totalBytes = 0;
  async function walk(directoryUri: string, depth: number): Promise<void> {
    checkCurrent();
    if (depth > 16) throw new Error(`资源包 ${themeId} 目录层级过深`);
    const entries = await file.listDirectory(directoryUri);
    checkCurrent();
    for (let i = 0; i < entries.length; i++) {
      checkCurrent();
      const entry = entries[i];
      let uri = entry.uri;
      if (!uri.startsWith(rootUri)) {
        if (uri.startsWith("/") || uri.indexOf("://") >= 0)
          throw new Error(`资源包 ${themeId} 文件 URI 越界`);
        uri = `${directoryUri}${uri.startsWith("./") ? uri.slice(2) : uri}`;
      }
      if (!uri.startsWith(rootUri)) throw new Error(`资源包 ${themeId} 文件 URI 越界`);
      if (uri.endsWith("/")) {
        await walk(uri, depth + 1);
        continue;
      }
      const relativePath = uri.slice(rootUri.length);
      if (!relativePath || relativePath === "mappings.tsv") continue;
      const info = await file.readFileInfo(uri);
      checkCurrent();
      if (info.type === "dir") {
        await walk(`${uri}/`, depth + 1);
        continue;
      }
      const sizeBytes = Number.isSafeInteger(entry.length) && entry.length >= 0
        ? entry.length : info.length;
      addFile(uri, sizeBytes);
    }
  }
  await walk(rootUri, 0);
  return Array.from(files.values()).sort((a, b) => a.relativePath.localeCompare(b.relativePath));
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

/** Resolves a read-only ordering snapshot without writing migration results. */
export function resolveResourceOrder(installedThemeIds: string[], text: string | null): string[] {
  if (installedThemeIds.some(item => !validThemeId(item)) ||
      new Set(installedThemeIds).size !== installedThemeIds.length)
    throw new Error("已安装资源包索引无效");
  return text === null
    ? [...installedThemeIds, SYSTEM_STYLE_ID]
    : normalizedOrder(parseOrder(text), installedThemeIds);
}

/** Loads/migrates ordering; newly discovered package IDs are inserted at highest priority. */
export async function loadResourceOrder(
  installedThemeIds: string[],
  file: ResourceOrderFileApi,
): Promise<string[]> {
  const text = await file.readOptionalText(RESOURCE_ORDER_URI);
  const order = resolveResourceOrder(installedThemeIds, text);
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
  if (!validThemeId(themeId) ||
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
    if (!validThemeId(themeId) || !Array.isArray(entries) ||
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

/** Reads and validates the shared inventory once for a whole catalog load. */
export async function readThemeFileInventories(
  file: ResourceOrderFileApi,
): Promise<ThemeFileRecord | null> {
  const text = await file.readOptionalText(RESOURCE_FILES_URI);
  return text === null ? null : parseThemeFileRecords(text);
}

export async function readThemeFileInventory(
  themeId: string,
  file: ResourceOrderFileApi,
): Promise<ThemeAssetFile[] | null> {
  const record = await readThemeFileInventories(file);
  return record ? record[themeId] || null : null;
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
