import { parseResourcePackManifest } from "./resource-pack";
import type { ResourcePackManifest, ResourcePackMapping } from "./resource-pack";
import {
  mappingMatchesRelativeFile,
  readThemeFileInventory,
  SYSTEM_STYLE_ID,
  writeThemeFileInventory
} from "./resource-order";
import type { ResourceAssetFileApi, ThemeAssetFile } from "./resource-order";

export const ACTIVE_MAPPINGS_URI = "internal://files/mappings.tsv";
export const ACTIVE_GENERATIONS_URI = "internal://files/resource-active-generations.json";

const INSTALLED_THEMES_URI = "internal://files/interconnect-themes.json";
const THEME_ROOT_URI = "internal://files/themes/";
const NATIVE_ROOT = "/data/quickapp/files/ng.lst.corona/";
const NATIVE_THEME_ROOT = `${NATIVE_ROOT}themes/`;
const ACTIVE_DIRECTORY_PREFIX = ".active-";
const MAX_MAPPING_RULES = 64;
const MAX_CONFIG_BYTES = 32 * 1024;
const MAX_PATH_BYTES = 256;
const COPY_CHUNK_BYTES = 16 * 1024;

interface ThemeRules {
  themeId: string;
  manifest: ResourcePackManifest;
  files: ThemeAssetFile[] | null;
}

export interface ActiveMappingRule {
  source: string;
  destination: string;
}

export interface ActiveFileCopy {
  sourceUri: string;
  destinationUri: string;
  sizeBytes: number;
}

export interface ActiveMappingsPlan {
  mappings: string;
  generation: string | null;
  copies: ActiveFileCopy[];
}

interface SourceGroupEntry {
  themeId: string;
  themeIndex: number;
  mappingIndex: number;
  mapping: ResourcePackMapping;
  files: ThemeAssetFile[] | null;
}

interface StoredGenerationIndex {
  version: 1;
  generations: string[];
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

function safeAbsolutePath(value: string): boolean {
  if (!value || value[0] !== "/" || utf8Length(value) >= MAX_PATH_BYTES) return false;
  const segments = value.slice(1).split("/");
  return segments.every((segment, index) =>
    (segment !== "" || index === segments.length - 1) && segment !== "." && segment !== ".." &&
    !/[\\:\u0000-\u001f\u007f]/.test(segment));
}

function parseGenerationIndex(text: string | null): StoredGenerationIndex {
  if (text === null) return { version: 1, generations: [] };
  let value: unknown;
  try { value = JSON.parse(text); }
  catch (_error) { throw new Error("活动资源代次索引损坏"); }
  if (typeof value !== "object" || value === null || Array.isArray(value) ||
      (value as { version?: unknown }).version !== 1 ||
      !Array.isArray((value as { generations?: unknown }).generations) ||
      (value as { generations: unknown[] }).generations.some(item =>
        typeof item !== "string" || !/^[a-z0-9_-]{1,32}$/.test(item)) ||
      new Set((value as { generations: string[] }).generations).size !==
        (value as { generations: string[] }).generations.length)
    throw new Error("活动资源代次索引格式无效");
  return value as StoredGenerationIndex;
}

function sourceRuleMatches(source: string, resourcePath: string): boolean {
  return source.endsWith("/") ? resourcePath.startsWith(source) : resourcePath === source;
}

function mappingsOverlap(left: ResourcePackMapping, right: ResourcePackMapping): boolean {
  if (left.source === right.source) return true;
  if (left.source.endsWith("/") && right.source.startsWith(left.source)) return true;
  if (right.source.endsWith("/") && left.source.startsWith(right.source)) return true;
  return false;
}

function collectSourceGroups(themesHighToLow: ThemeRules[]): SourceGroupEntry[][] {
  const groups: SourceGroupEntry[][] = [];
  for (let themeIndex = 0; themeIndex < themesHighToLow.length; themeIndex++) {
    const theme = themesHighToLow[themeIndex];
    for (let mappingIndex = 0; mappingIndex < theme.manifest.mappings.length; mappingIndex++) {
      const entry: SourceGroupEntry = {
        themeId: theme.themeId,
        themeIndex,
        mappingIndex,
        mapping: theme.manifest.mappings[mappingIndex],
        files: theme.files
      };
      const merged = [entry];
      let joined = true;
      while (joined) {
        joined = false;
        for (let i = groups.length - 1; i >= 0; i--) {
          if (groups[i].some(existing => merged.some(candidate =>
            mappingsOverlap(existing.mapping, candidate.mapping)))) {
            merged.push(...groups.splice(i, 1)[0]);
            joined = true;
            break;
          }
        }
      }
      merged.sort((a, b) => a.themeIndex - b.themeIndex || a.mappingIndex - b.mappingIndex);
      groups.push(merged);
    }
  }
  return groups;
}

function needsMaterialization(entries: SourceGroupEntry[]): boolean {
  return entries.length > 1 && entries.some(entry => entry.mapping.source.endsWith("/"));
}

function themesNeedingInventory(themesHighToLow: ThemeRules[]): Set<string> {
  const needed = new Set<string>();
  for (const group of collectSourceGroups(themesHighToLow)) {
    if (needsMaterialization(group)) group.forEach(entry => needed.add(entry.themeId));
  }
  return needed;
}

/** Builds an order-aware static overlay for every overlapping source-path group. */
export function planActiveMappings(
  themesHighToLow: ThemeRules[],
  generation: string,
): ActiveMappingsPlan {
  if (!/^[a-z0-9_-]{1,32}$/.test(generation)) throw new Error("活动资源代次标识无效");
  const groups = collectSourceGroups(themesHighToLow);
  if (groups.length > MAX_MAPPING_RULES)
    throw new Error(`合并后映射超过模块上限 ${MAX_MAPPING_RULES} 条`);

  const rules: ActiveMappingRule[] = [];
  const copies: ActiveFileCopy[] = [];
  let usedGeneration = false;
  for (let ruleIndex = 0; ruleIndex < groups.length; ruleIndex++) {
    const entries = groups[ruleIndex];
    if (!needsMaterialization(entries)) {
      const winner = entries[0];
      rules.push({ source: winner.mapping.source,
        destination: `${NATIVE_THEME_ROOT}${winner.themeId}/${winner.mapping.destination}` });
      continue;
    }

    usedGeneration = true;
    const directorySources = entries.map(entry => entry.mapping.source).filter(source => source.endsWith("/"));
    const sourceRoot = directorySources.sort((left, right) => left.length - right.length)[0];
    if (!sourceRoot || entries.some(entry => !entry.mapping.source.startsWith(sourceRoot)))
      throw new Error("无法合并重叠的资源映射路径");
    for (const entry of entries) {
      if (entry.files === null)
        throw new Error(`资源包 ${entry.themeId} 缺少文件清单，无法生成叠加资源`);
    }

    const activeDestination = `${NATIVE_THEME_ROOT}${ACTIVE_DIRECTORY_PREFIX}${generation}/r${ruleIndex}/`;
    if (!safeAbsolutePath(activeDestination)) throw new Error("生成的活动资源目录路径过长");
    rules.push({ source: sourceRoot, destination: activeDestination });

    // Resolve each concrete source path by pack priority. Within one pack, the
    // longest matching manifest source retains the hook's existing specificity rule.
    const winners = new Map<string, { entry: SourceGroupEntry; asset: ThemeAssetFile }>();
    const themeIds = Array.from(new Set(entries.map(entry => entry.themeId)));
    for (const themeId of themeIds) {
      const themeEntries = entries.filter(entry => entry.themeId === themeId);
      const candidates = new Map<string, { entry: SourceGroupEntry; asset: ThemeAssetFile }>();
      for (const entry of themeEntries) {
        for (const asset of entry.files || []) {
          if (!mappingMatchesRelativeFile(entry.mapping, asset.relativePath)) continue;
          const suffix = entry.mapping.destination.endsWith("/")
            ? asset.relativePath.slice(entry.mapping.destination.length) : "";
          if (entry.mapping.destination.endsWith("/") && !suffix) continue;
          const sourcePath = `${entry.mapping.source}${suffix}`;
          if (!sourcePath.startsWith(sourceRoot) || !safeAbsolutePath(sourcePath))
            throw new Error(`资源路径展开后超过模块限制：${sourcePath}`);
          // A more-specific rule masks this broad rule in the module even when
          // its own package destination lacks this particular file.
          if (themeEntries.some(other => other !== entry &&
              other.mapping.source.length > entry.mapping.source.length &&
              sourceRuleMatches(other.mapping.source, sourcePath))) continue;
          const existing = candidates.get(sourcePath);
          if (!existing || entry.mapping.source.length > existing.entry.mapping.source.length)
            candidates.set(sourcePath, { entry, asset });
        }
      }
      // Theme IDs are traversed in high-to-low order, so first concrete file wins.
      candidates.forEach((candidate, sourcePath) => {
        if (!winners.has(sourcePath)) winners.set(sourcePath, candidate);
      });
    }

    winners.forEach((winner, sourcePath) => {
      const suffix = sourcePath.slice(sourceRoot.length);
      if (!suffix) throw new Error(`资源映射未指向具体文件：${sourcePath}`);
      const nativeDestination = `${activeDestination}${suffix}`;
      if (!safeAbsolutePath(nativeDestination))
        throw new Error(`活动资源路径超过模块限制：${nativeDestination}`);
      copies.push({
        sourceUri: `${THEME_ROOT_URI}${winner.entry.themeId}/${winner.asset.relativePath}`,
        destinationUri: `internal://files/themes/${ACTIVE_DIRECTORY_PREFIX}${generation}/r${ruleIndex}/${suffix}`,
        sizeBytes: winner.asset.sizeBytes
      });
    });
  }

  const mappings = rules.length
    ? rules.map(rule => `${rule.source}\t${rule.destination}\n`).join("")
    : "# No active resource packs above the system style.\n";
  if (utf8Length(mappings) > MAX_CONFIG_BYTES)
    throw new Error("生成的活动 mappings.tsv 超过 32 KiB");
  if (rules.some(rule => !safeAbsolutePath(rule.source) || !safeAbsolutePath(rule.destination) ||
      (rule.source.endsWith("/") !== rule.destination.endsWith("/"))))
    throw new Error("合并后的资源映射路径无效");

  return { mappings, generation: usedGeneration ? generation : null, copies };
}

async function enumerateThemeFiles(
  themeId: string,
  file: ResourceAssetFileApi,
): Promise<ThemeAssetFile[]> {
  const rootUri = `${THEME_ROOT_URI}${themeId}/`;
  const files: ThemeAssetFile[] = [];
  let totalBytes = 0;
  async function walk(directoryUri: string, depth: number): Promise<void> {
    if (depth > 16) throw new Error(`资源包 ${themeId} 目录层级过深`);
    const entries = await file.listDirectory(directoryUri);
    for (const entry of entries) {
      if (!entry.uri.startsWith(rootUri))
        throw new Error(`资源包 ${themeId} 的文件 URI 越界`);
      const relativePath = entry.uri.slice(rootUri.length).replace(/\/$/, "");
      const info = await file.readFileInfo(entry.uri);
      if (info.type === "dir") {
        await walk(entry.uri.endsWith("/") ? entry.uri : `${entry.uri}/`, depth + 1);
        continue;
      }
      if (!relativePath || relativePath === "mappings.tsv") continue;
      files.push({ relativePath, sizeBytes: info.length });
      totalBytes += info.length;
      if (files.length > 128 || totalBytes > 64 * 1024 * 1024)
        throw new Error(`资源包 ${themeId} 文件清单超过 CRPack 限制`);
    }
  }
  await walk(rootUri, 0);
  return files;
}

async function copyAsset(
  copy: ActiveFileCopy,
  file: ResourceAssetFileApi,
): Promise<void> {
  if (copy.sizeBytes === 0) throw new Error(`不能叠加空资源文件：${copy.sourceUri}`);
  const separator = copy.destinationUri.lastIndexOf("/");
  await file.makeDirectory(copy.destinationUri.slice(0, separator + 1), true);
  try { await file.deleteFile(copy.destinationUri); }
  catch (error) { if (!file.isFileNotFound(error)) throw error; }

  for (let position = 0; position < copy.sizeBytes; position += COPY_CHUNK_BYTES) {
    const length = Math.min(COPY_CHUNK_BYTES, copy.sizeBytes - position);
    const bytes = await file.readArrayBuffer(copy.sourceUri, position, length);
    if (bytes.length !== length) throw new Error(`资源文件读取长度不一致：${copy.sourceUri}`);
    await file.writeArrayBuffer(copy.destinationUri, bytes, position);
  }
}

/** Materializes overlays before mappings.tsv is changed, preserving the prior active generation. */
export async function regenerateActiveMappings(
  order: string[],
  generation: string,
  file: ResourceAssetFileApi,
): Promise<ActiveMappingsPlan> {
  const systemIndex = order.indexOf(SYSTEM_STYLE_ID);
  if (systemIndex < 0 || order.lastIndexOf(SYSTEM_STYLE_ID) !== systemIndex)
    throw new Error("资源顺序必须包含唯一的系统样式分界");

  const themeIds = order.slice(0, systemIndex);
  const themesHighToLow: ThemeRules[] = [];
  for (const themeId of themeIds) {
    const text = await file.readOptionalText(`${THEME_ROOT_URI}${themeId}/canora.json`);
    if (text === null) throw new Error(`资源包 ${themeId} 的 canora.json 不存在，无法生成映射`);
    let manifest: ResourcePackManifest;
    try { manifest = parseResourcePackManifest(text, themeId); }
    catch (error) { throw new Error(`资源包 ${themeId} 的 manifest 无效：${String((error as Error).message || error)}`); }
    themesHighToLow.push({ themeId, manifest, files: null });
  }

  // File lists are only needed when source prefixes overlap across active mappings.
  const inventoryThemeIds = themesNeedingInventory(themesHighToLow);
  for (const theme of themesHighToLow) {
    if (!inventoryThemeIds.has(theme.themeId)) continue;
    let inventory = await readThemeFileInventory(theme.themeId, file);
    if (inventory === null) {
      inventory = await enumerateThemeFiles(theme.themeId, file);
      await writeThemeFileInventory(theme.themeId, inventory, file);
    }
    theme.files = inventory;
  }

  const plan = planActiveMappings(themesHighToLow, generation);
  const previousText = await file.readOptionalText(ACTIVE_GENERATIONS_URI);
  const generationIndex = parseGenerationIndex(previousText);
  if (plan.generation && generationIndex.generations.indexOf(plan.generation) >= 0)
    throw new Error("活动资源代次标识重复，请重新触发重载");
  if (plan.generation) {
    generationIndex.generations.push(plan.generation);
    await file.writeText(ACTIVE_GENERATIONS_URI, JSON.stringify(generationIndex));
  }

  try {
    for (const copy of plan.copies) await copyAsset(copy, file);
    await file.writeText(ACTIVE_MAPPINGS_URI, plan.mappings);
  } catch (error) {
    if (plan.generation) {
      let removed = false;
      try {
        await file.removeDirectory(`${THEME_ROOT_URI}${ACTIVE_DIRECTORY_PREFIX}${plan.generation}/`);
        removed = true;
      } catch (cleanupError) {
        removed = file.isFileNotFound(cleanupError);
      }
      if (removed) {
        generationIndex.generations = generationIndex.generations.filter(item => item !== plan.generation);
        try {
          await file.writeText(ACTIVE_GENERATIONS_URI, JSON.stringify(generationIndex));
        } catch (_indexError) {
          // A stale registry entry is safe; the next acknowledged reload retries cleanup.
        }
      }
    }
    throw error;
  }
  return plan;
}

/** Removes old immutable overlay trees only after the module acknowledged the new TSV. */
export async function cleanupInactiveGenerations(
  activeGeneration: string | null,
  file: ResourceAssetFileApi,
): Promise<void> {
  const text = await file.readOptionalText(ACTIVE_GENERATIONS_URI);
  const index = parseGenerationIndex(text);
  const retained: string[] = [];
  for (const generation of index.generations) {
    if (generation === activeGeneration) {
      retained.push(generation);
      continue;
    }
    try {
      await file.removeDirectory(`${THEME_ROOT_URI}${ACTIVE_DIRECTORY_PREFIX}${generation}/`);
    } catch (error) {
      if (!file.isFileNotFound(error)) retained.push(generation);
    }
  }
  if (retained.length) await file.writeText(ACTIVE_GENERATIONS_URI,
    JSON.stringify({ version: 1, generations: retained }));
  else if (text !== null) await file.writeText(ACTIVE_GENERATIONS_URI,
    JSON.stringify({ version: 1, generations: [] }));
}
