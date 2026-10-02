import { readResourcePackManifest } from "./resource-pack";
import { safeResourceSource, isDirectoryResourceSource } from "./resource-path";
import type { ResourcePackManifest, ResourcePackMapping } from "./resource-pack";
import type { ActiveMappingsPlan } from "./resource-activation";
import {
  enumerateThemeFiles,
  mappingMatchesRelativeFile,
  readInstalledThemeIds,
  readThemeFileInventories
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

export interface ResourcePackSnapshot {
  installedThemeIds: string[];
  themes: Map<string, {
    themeId: string;
    manifest: ResourcePackManifest;
    files: ThemeAssetFile[];
  }>;
  paths: RegisteredResourcePath[];
  byPath: Map<string, RegisteredResourcePath>;
  // Shared across page bundles; package invalidation replaces the owning snapshot.
  activeMappings?: { key: string; plan: ActiveMappingsPlan };
}

export interface ResourceCatalog extends ResourcePackSnapshot {
  session: object;
  revision: number;
}

interface ResourceCatalogSession {
  revision: number;
  file?: ResourceAssetFileApi;
  catalogLoads: WeakMap<ResourceAssetFileApi, {
    revision: number;
    promise: Promise<ResourceCatalog>;
  }>;
  overrideTransactions: WeakMap<object, Promise<void>>;
  operationQueue: Promise<void>;
}

type ResourceGlobals = typeof globalThis & { resourceCatalogSession?: ResourceCatalogSession };

function createSession(file?: ResourceAssetFileApi): ResourceCatalogSession {
  return { revision: 0, file, catalogLoads: new WeakMap(), overrideTransactions: new WeakMap(),
    operationQueue: Promise.resolve() };
}

// Vela bundles each page separately. Only app-owned globals are shared across bundles.
const localSession = createSession();
function resourceSession(): ResourceCatalogSession {
  return (globalThis as ResourceGlobals).resourceCatalogSession || localSession;
}

/** Keeps reload snapshots immutable relative to receiver writes and package deletion. */
export function withResourceOperation<T>(operation: () => Promise<T>): Promise<T> {
  const session = resourceSession();
  const result = session.operationQueue.then(operation);
  session.operationQueue = result.then(() => {}, () => {});
  return result;
}

/** Native callbacks should belong to the application adapter, not a transient page. */
export function getResourceFileApi<T extends Pick<ResourceAssetFileApi, "readOptionalText">>(
  file: T,
): T | ResourceAssetFileApi {
  return resourceSession().file || file;
}

export function initializeResourceCatalogSession(file: ResourceAssetFileApi): void {
  (globalThis as ResourceGlobals).resourceCatalogSession = createSession(file);
}

export function destroyResourceCatalogSession(): void {
  const globals = globalThis as ResourceGlobals;
  if (globals.resourceCatalogSession) globals.resourceCatalogSession.revision++;
  delete globals.resourceCatalogSession;
}

/** Invalidates both completed snapshots and in-flight loads, including same-ID updates. */
export function invalidateResourceCatalog(): void {
  resourceSession().revision++;
}

export function isResourceCatalogCurrent(catalog: ResourceCatalog): boolean {
  const session = resourceSession();
  return catalog.session === session && catalog.revision === session.revision;
}

/** Shares one read-only catalog load across pages for the current package revision. */
export function getResourceCatalog(file: ResourceAssetFileApi): Promise<ResourceCatalog> {
  const session = resourceSession();
  // Page-local module namespace objects differ even though they address the same files.
  file = session.file || file;
  const existing = session.catalogLoads.get(file);
  if (existing && existing.revision === session.revision) return existing.promise;
  const revision = session.revision;
  const isCurrent = () => resourceSession() === session && revision === session.revision;
  const retryCurrent = () => {
    if (session.file && !resourceSession().file) throw new Error("资源目录会话已结束");
    return getResourceCatalog(file);
  };
  const promise: Promise<ResourceCatalog> = (async () => {
    try {
      const installedThemeIds = await readInstalledThemeIds(file);
      if (!isCurrent()) return retryCurrent();
      const snapshot = await loadResourcePackSnapshot(installedThemeIds, file, isCurrent);
      if (!isCurrent()) return retryCurrent();
      return { ...snapshot, session, revision };
    } catch (error) {
      if (session.catalogLoads.get(file)?.revision === revision) session.catalogLoads.delete(file);
      if (!isCurrent()) return retryCurrent();
      throw error;
    }
  })();
  session.catalogLoads.set(file, { revision, promise });
  return promise;
}

function overrideTransaction<T>(file: object, operation: () => Promise<T>): Promise<T> {
  const transactions = resourceSession().overrideTransactions;
  const previous = transactions.get(file) || Promise.resolve();
  const result = previous.then(operation);
  // Keep failed writes visible to their caller without poisoning the next transaction.
  transactions.set(file, result.then(() => {}, () => {}));
  return result;
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

export function validSourcePath(value: unknown): value is string {
  return typeof value === "string" && !isDirectoryResourceSource(value) && safeResourceSource(value);
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
  return isDirectoryResourceSource(source) ? resourcePath.startsWith(source) : resourcePath === source;
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

function mergeThemeRegistrations(
  byPath: Map<string, Map<string, ResourcePackOption>>,
  manifest: ResourcePackManifest,
  inventory: ThemeAssetFile[],
): void {
  // Keep synchronous iteration separate from awaits: the Vela bytecode compiler rejects
  // the production minifier's yield/comma expression in a for-of iterable.
  for (const registration of registeredFilesForTheme(manifest.mappings, inventory)) {
    const sourcePath = registration.sourcePath;
    let themes = byPath.get(sourcePath);
    if (!themes) {
      themes = new Map<string, ResourcePackOption>();
      byPath.set(sourcePath, themes);
    }
    const themeId = manifest.themeId;
    const previewUri = registration.relativePath.toLowerCase().endsWith(".bin")
      ? `${THEME_ROOT_URI}${themeId}/${registration.relativePath}` : "";
    themes.set(themeId, { themeId, name: manifest.name, previewUri });
    if (byPath.size > MAX_CATALOG_PATHS)
      throw new Error("注册的替换路径超过 Manager 列表上限");
  }
}

/** Loads a fresh operation snapshot, never reusing the mix-page session cache. */
export async function loadResourcePackSnapshot(
  installedThemeIds: string[],
  file: ResourceAssetFileApi,
  isCurrent: () => boolean = () => true,
): Promise<ResourcePackSnapshot> {
  const checkCurrent = () => {
    if (!isCurrent()) throw new Error("资源目录已更新，请重试");
  };
  checkCurrent();
  installedThemeIds = installedThemeIds.slice();
  const themes: ResourcePackSnapshot["themes"] = new Map();
  if (!installedThemeIds.length)
    return { installedThemeIds, themes, paths: [], byPath: new Map() };
  const manifests = new Array<ResourcePackManifest>(installedThemeIds.length);
  let nextIndex = 0;
  let failed = false;
  async function readManifests(): Promise<void> {
    try {
      while (!failed && nextIndex < installedThemeIds.length) {
        checkCurrent();
        const index = nextIndex++;
        const themeId = installedThemeIds[index];
        let manifest: ResourcePackManifest | null;
        try { manifest = await readResourcePackManifest(`${THEME_ROOT_URI}${themeId}/`, themeId, file, checkCurrent); }
        catch (error) {
          throw new Error(`资源包 ${themeId} 的 manifest 无效：${String((error as Error).message || error)}`);
        }
        checkCurrent();
        if (failed) return;
        if (manifest === null) throw new Error(`资源包 ${themeId} 缺少 corona.json，无法读取混搭路径`);
        manifests[index] = manifest;
      }
    } catch (error) {
      failed = true;
      throw error;
    }
  }
  const [inventories] = await Promise.all([
    // Missing, corrupt or unreadable optional indexes still fall back to disk metadata.
    readThemeFileInventories(file).catch(() => null),
    Promise.all(Array.from({ length: Math.min(2, installedThemeIds.length) }, () => readManifests()))
  ]);
  checkCurrent();
  const byPath = new Map<string, Map<string, ResourcePackOption>>();
  for (let index = 0; index < installedThemeIds.length; index++) {
    checkCurrent();
    const themeId = installedThemeIds[index];
    const manifest = manifests[index];
    let inventory = inventories ? inventories[themeId] : null;
    const inventoryCoversMappings = inventory && manifest.mappings.some(mapping =>
      inventory!.some(asset => mappingMatchesRelativeFile(mapping, asset.relativePath)));
    if (manifest.mappings.length && !inventoryCoversMappings) {
      // Scan at most one package at a time; activation reuses the resolved fallback.
      inventory = await enumerateThemeFiles(themeId, file, isCurrent);
      checkCurrent();
    }
    const files = inventory || [];
    themes.set(themeId, { themeId, manifest, files });
    mergeThemeRegistrations(byPath, manifest, files);
  }
  const paths = Array.from(byPath.keys()).sort().map(sourcePath => ({
    sourcePath,
    themes: Array.from((byPath.get(sourcePath) as Map<string, ResourcePackOption>).values())
  }));
  return { installedThemeIds, themes, paths,
    byPath: new Map(paths.map(item => [item.sourcePath, item])) };
}

/** Lists concrete replacement files through the shared fresh-snapshot loader. */
export async function loadRegisteredResourcePaths(
  installedThemeIds: string[],
  file: ResourceAssetFileApi,
  isCurrent: () => boolean = () => true,
): Promise<RegisteredResourcePath[]> {
  return (await loadResourcePackSnapshot(installedThemeIds, file, isCurrent)).paths;
}

async function readOverrides(
  file: Pick<ResourceAssetFileApi, "readOptionalText">,
): Promise<ResourceOverrides> {
  return parseOverrides(await file.readOptionalText(RESOURCE_OVERRIDES_URI));
}

export async function loadResourceOverrides(
  file: Pick<ResourceAssetFileApi, "readOptionalText">,
): Promise<ResourceOverrides> {
  const session = resourceSession();
  file = session.file || file;
  // Returning pages must observe saves already queued by the selection page.
  await (session.overrideTransactions.get(file) || Promise.resolve());
  return readOverrides(file);
}

/** Resolves display choices without cleaning or writing a possibly outdated snapshot. */
export function resolveResourceChoice(
  registration: RegisteredResourcePath,
  overrides: ResourceOverrides,
): string {
  const choice = overrides[registration.sourcePath] || DEFAULT_RESOURCE_CHOICE;
  return choice === DEFAULT_RESOURCE_CHOICE || choice === SYSTEM_RESOURCE_CHOICE ||
    registration.themes.some(theme => theme.themeId === choice) ? choice : DEFAULT_RESOURCE_CHOICE;
}

export async function saveResourceOverride(
  sourcePath: string,
  choice: string,
  file: Pick<ResourceAssetFileApi, "readOptionalText" | "writeText">,
): Promise<void> {
  if (!validSourcePath(sourcePath) || !validChoice(choice))
    throw new Error("混搭微调路径或选项无效");
  file = resourceSession().file || file;
  await overrideTransaction(file, async () => {
    const overrides = await readOverrides(file);
    if (overrides[sourcePath] === choice) return;
    overrides[sourcePath] = choice;
    await file.writeText(RESOURCE_OVERRIDES_URI, serializeOverrides(overrides));
  });
}

/** Removes stale paths and falls back to Default if the selected pack was removed. */
export async function reconcileResourceOverrides(
  catalog: RegisteredResourcePath[],
  file: Pick<ResourceAssetFileApi, "readOptionalText" | "writeText">,
): Promise<ResourceOverrides> {
  file = resourceSession().file || file;
  return overrideTransaction(file, async () => {
    const overrides = await readOverrides(file);
    const registrations = new Map(catalog.map(item => [item.sourcePath, item]));
    let changed = false;
    for (const sourcePath of Object.keys(overrides)) {
      const registration = registrations.get(sourcePath);
      if (!registration) {
        delete overrides[sourcePath];
        changed = true;
        continue;
      }
      const choice = resolveResourceChoice(registration, overrides);
      if (choice !== overrides[sourcePath]) {
        overrides[sourcePath] = choice;
        changed = true;
      }
    }
    if (changed) await file.writeText(RESOURCE_OVERRIDES_URI, serializeOverrides(overrides));
    return overrides;
  });
}

/** Changes references to a removed pack into the normal-order default. */
export async function removeThemeFromResourceOverrides(
  themeId: string,
  file: Pick<ResourceAssetFileApi, "readOptionalText" | "writeText">,
): Promise<void> {
  file = resourceSession().file || file;
  await overrideTransaction(file, async () => {
    const overrides = await readOverrides(file);
    let changed = false;
    for (const sourcePath of Object.keys(overrides)) {
      if (overrides[sourcePath] === themeId) {
        overrides[sourcePath] = DEFAULT_RESOURCE_CHOICE;
        changed = true;
      }
    }
    if (changed) await file.writeText(RESOURCE_OVERRIDES_URI, serializeOverrides(overrides));
  });
}
