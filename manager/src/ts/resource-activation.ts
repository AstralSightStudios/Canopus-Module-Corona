import { THEME_DESTINATION_ROOT, safeThemeDestination } from "./resource-path";
import { parseResourcePackManifest } from "./resource-pack";
import type { ResourcePackManifest, ResourcePackMapping } from "./resource-pack";
import {
  enumerateThemeFiles,
  mappingMatchesRelativeFile,
  readThemeFileInventories,
  SYSTEM_STYLE_ID
} from "./resource-order";
import type { ResourceAssetFileApi, ThemeAssetFile } from "./resource-order";
import {
  DEFAULT_RESOURCE_CHOICE,
  SYSTEM_RESOURCE_CHOICE
} from "./resource-overrides";
import type { ResourceOverrides, ResourcePackSnapshot } from "./resource-overrides";

export const ACTIVE_MAPPINGS_URI = "internal://files/mappings.tsv";
export const ACTIVE_GENERATIONS_URI = "internal://files/resource-active-generations.json";

const THEME_ROOT_URI = "internal://files/themes/";
const ACTIVE_DIRECTORY_PREFIX = ".active-";
const MAX_MAPPING_RULES = 64;
const MAX_CONFIG_BYTES = 32 * 1024;
const MAX_PATH_BYTES = 256;

interface ThemeRules {
  themeId: string;
  manifest: ResourcePackManifest;
  files: ThemeAssetFile[] | null;
  active?: boolean;
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
  active: boolean;
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
        files: theme.files,
        active: theme.active !== false
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

/** Builds the ordered mappings, then applies per-file overrides before TSV serialization. */
export function planActiveMappings(
  themesHighToLow: ThemeRules[],
  generation: string,
  overrides: ResourceOverrides = Object.create(null) as ResourceOverrides,
): ActiveMappingsPlan {
  if (!/^[a-z0-9_-]{1,32}$/.test(generation)) throw new Error("活动资源代次标识无效");
  const priorityThemes = themesHighToLow.filter(theme => theme.active !== false);
  const groups = collectSourceGroups(priorityThemes);
  const rules: ActiveMappingRule[] = [];
  const copies: ActiveFileCopy[] = [];
  let usedGeneration = false;
  for (let ruleIndex = 0; ruleIndex < groups.length; ruleIndex++) {
    const entries = groups[ruleIndex];
    if (!needsMaterialization(entries)) {
      const winner = entries[0];
      rules.push({ source: winner.mapping.source,
        destination: `${THEME_DESTINATION_ROOT}${winner.themeId}/${winner.mapping.destination}` });
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

    const activeDestination = `${THEME_DESTINATION_ROOT}${ACTIVE_DIRECTORY_PREFIX}${generation}/r${ruleIndex}/`;
    if (!safeThemeDestination(activeDestination)) throw new Error("生成的活动资源目录路径过长");
    rules.push({ source: sourceRoot, destination: activeDestination });

    // Resolve each concrete path by normal pack order, then replace that decision
    // with an explicit pack choice. System choices remain absent from the overlay;
    // the hook then falls back to the original firmware resource.
    const candidatesByTheme = new Map<string, Map<string, { entry: SourceGroupEntry; asset: ThemeAssetFile }>>();
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
          if (!sourcePath.startsWith(sourceRoot))
            throw new Error(`资源映射展开后超出活动根路径：${sourcePath}`);
          if (utf8Length(sourcePath) >= MAX_PATH_BYTES) continue;
          if (!safeAbsolutePath(sourcePath))
            throw new Error(`资源路径展开后无效：${sourcePath}`);
          // A more-specific rule masks this broad rule even if its destination
          // does not contain the concrete file.
          if (themeEntries.some(other => other !== entry &&
              other.mapping.source.length > entry.mapping.source.length &&
              sourceRuleMatches(other.mapping.source, sourcePath))) continue;
          const existing = candidates.get(sourcePath);
          if (!existing || entry.mapping.source.length > existing.entry.mapping.source.length)
            candidates.set(sourcePath, { entry, asset });
        }
      }
      candidatesByTheme.set(themeId, candidates);
    }

    const sourcePaths = new Set<string>();
    candidatesByTheme.forEach(candidates => candidates.forEach((_candidate, sourcePath) => {
      sourcePaths.add(sourcePath);
    }));
    sourcePaths.forEach(sourcePath => {
      let winner: { entry: SourceGroupEntry; asset: ThemeAssetFile } | undefined;
      for (const themeId of themeIds) {
        const candidate = candidatesByTheme.get(themeId)?.get(sourcePath);
        if (candidate) {
          winner = candidate;
          break;
        }
      }
      if (!winner) return;
      const suffix = sourcePath.slice(sourceRoot.length);
      if (!suffix) throw new Error(`资源映射未指向具体文件：${sourcePath}`);
      const expandedDestination = `${activeDestination}${suffix}`;
      if (!safeThemeDestination(expandedDestination))
        throw new Error(`活动资源路径超过模块限制：${expandedDestination}`);
      copies.push({
        sourceUri: `${THEME_ROOT_URI}${winner.entry.themeId}/${winner.asset.relativePath}`,
        destinationUri: `internal://files/themes/${ACTIVE_DIRECTORY_PREFIX}${generation}/r${ruleIndex}/${suffix}`,
        sizeBytes: winner.asset.sizeBytes
      });
    });
  }

  const overrideRules: ActiveMappingRule[] = [];
  for (const sourcePath of Object.keys(overrides).sort()) {
    const choice = overrides[sourcePath];
    if (choice === DEFAULT_RESOURCE_CHOICE) continue;
    if (choice === SYSTEM_RESOURCE_CHOICE) {
      overrideRules.push({ source: sourcePath, destination: SYSTEM_RESOURCE_CHOICE });
      continue;
    }
    const theme = themesHighToLow.find(item => item.themeId === choice);
    if (!theme) throw new Error(`混搭微调资源包不存在：${choice}`);
    const mapping = theme.manifest.mappings.filter(item =>
      sourceRuleMatches(item.source, sourcePath))
      .sort((left, right) => right.source.length - left.source.length)[0];
    if (!mapping) throw new Error(`混搭微调资源包未注册路径：${sourcePath}`);
    const relativeDestination = mapping.destination.endsWith("/")
      ? `${mapping.destination}${sourcePath.slice(mapping.source.length)}`
      : mapping.destination;
    if (!relativeDestination || relativeDestination.endsWith("/"))
      throw new Error(`混搭微调资源目标不是文件：${sourcePath}`);
    const destination = `${THEME_DESTINATION_ROOT}${theme.themeId}/${relativeDestination}`;
    if (!safeAbsolutePath(sourcePath) || !safeThemeDestination(destination))
      throw new Error(`混搭微调资源路径超过模块限制：${sourcePath}`);
    overrideRules.push({ source: sourcePath, destination });
  }
  for (const overrideRule of overrideRules) {
    for (let index = rules.length - 1; index >= 0; index--) {
      if (rules[index].source === overrideRule.source) rules.splice(index, 1);
    }
    rules.push(overrideRule);
  }

  if (rules.length > MAX_MAPPING_RULES)
    throw new Error(`合并后映射超过模块上限 ${MAX_MAPPING_RULES} 条`);
  const mappings = rules.length
    ? rules.map(rule => `${rule.source}\t${rule.destination}\n`).join("")
    : "# No active resource replacements.\n";
  if (utf8Length(mappings) > MAX_CONFIG_BYTES)
    throw new Error("生成的活动 mappings.tsv 超过 32 KiB");
  const invalidRule = rules.some(rule => {
    if (!safeAbsolutePath(rule.source)) return true;
    if (rule.destination === SYSTEM_RESOURCE_CHOICE) return rule.source.endsWith("/");
    return !safeThemeDestination(rule.destination) ||
      rule.source.endsWith("/") !== rule.destination.endsWith("/");
  });
  if (invalidRule) throw new Error("合并后的资源映射路径无效");

  return { mappings, generation: usedGeneration ? generation : null, copies };
}

async function ensureDirectory(uri: string, file: ResourceAssetFileApi): Promise<void> {
  try {
    await file.makeDirectory(uri, true);
  } catch (error) {
    try {
      const info = await file.readFileInfo(uri);
      if (info.type === "dir") return;
    } catch (_checkError) {
      // Preserve the original mkdir error when the path cannot be verified.
    }
    throw error;
  }
}

async function copyAsset(
  copy: ActiveFileCopy,
  file: ResourceAssetFileApi,
  preparedDirectories: Set<string>,
): Promise<void> {
  if (copy.sizeBytes === 0) throw new Error(`不能叠加空资源文件：${copy.sourceUri}`);
  const separator = copy.destinationUri.lastIndexOf("/");
  const directory = copy.destinationUri.slice(0, separator + 1);
  if (!preparedDirectories.has(directory)) {
    await ensureDirectory(directory, file);
    preparedDirectories.add(directory);
  }

  const source = await file.readFileInfo(copy.sourceUri);
  if (source.length !== copy.sizeBytes || (source.type !== undefined && source.type !== "file"))
    throw new Error(`资源文件大小与快照不一致：${copy.sourceUri}`);
  // The native worker truncates an existing destination; errors must not publish mappings.
  await file.copyFile(copy.sourceUri, copy.destinationUri);
  const destination = await file.readFileInfo(copy.destinationUri);
  if (destination.length !== copy.sizeBytes ||
      (destination.type !== undefined && destination.type !== "file"))
    throw new Error(`资源文件复制大小不一致：${copy.destinationUri}`);
}

/** Materializes overlays before mappings.tsv is changed, preserving the prior active generation. */
export async function regenerateActiveMappings(
  order: string[],
  generation: string,
  file: ResourceAssetFileApi,
  overrides: ResourceOverrides = Object.create(null) as ResourceOverrides,
  snapshot?: ResourcePackSnapshot,
): Promise<ActiveMappingsPlan> {
  const systemIndex = order.indexOf(SYSTEM_STYLE_ID);
  if (systemIndex < 0 || order.lastIndexOf(SYSTEM_STYLE_ID) !== systemIndex)
    throw new Error("资源顺序必须包含唯一的系统样式分界");

  const activeThemeIds = order.slice(0, systemIndex);
  const installedThemeIds = order.filter(themeId => themeId !== SYSTEM_STYLE_ID);
  const activeIds = new Set(activeThemeIds);
  const selectedIds = new Set(Object.keys(overrides).map(sourcePath => overrides[sourcePath])
    .filter(choice => choice !== DEFAULT_RESOURCE_CHOICE && choice !== SYSTEM_RESOURCE_CHOICE &&
      installedThemeIds.includes(choice)));
  const themesHighToLow: ThemeRules[] = [];
  const loadManifest = async (themeId: string, required: boolean): Promise<ResourcePackManifest | null> => {
    if (snapshot) {
      const theme = snapshot.themes.get(themeId);
      if (!snapshot.installedThemeIds.includes(themeId) || !theme ||
          theme.themeId !== themeId || theme.manifest.themeId !== themeId ||
          !Array.isArray(theme.files))
        throw new Error(`资源快照缺少资源包 ${themeId}，无法生成映射`);
      return theme.manifest;
    }
    const text = await file.readOptionalText(`${THEME_ROOT_URI}${themeId}/canora.json`);
    if (text === null) {
      if (required) throw new Error(`资源包 ${themeId} 的 canora.json 不存在，无法生成映射`);
      return null;
    }
    try { return parseResourcePackManifest(text, themeId); }
    catch (error) {
      if (!required) return null;
      throw new Error(`资源包 ${themeId} 的 manifest 无效：${String((error as Error).message || error)}`);
    }
  };
  for (const themeId of activeThemeIds) {
    const manifest = await loadManifest(themeId, true);
    if (manifest) themesHighToLow.push({ themeId, manifest, files: null, active: true });
  }
  for (const themeId of order.slice(systemIndex + 1)) {
    if (!selectedIds.has(themeId) || activeIds.has(themeId)) continue;
    const manifest = await loadManifest(themeId, false);
    if (!manifest) continue;
    const selectedPaths = Object.keys(overrides).filter(sourcePath => overrides[sourcePath] === themeId);
    const mappings = manifest.mappings.filter(mapping =>
      selectedPaths.some(sourcePath => sourceRuleMatches(mapping.source, sourcePath)));
    if (mappings.length)
      themesHighToLow.push({ themeId, manifest: { ...manifest, mappings }, files: null, active: false });
  }

  // File lists are needed for overlapping groups and directory overrides that
  // must be materialized to preserve a system-resource hole.
  const inventoryThemeIds = themesNeedingInventory(
    themesHighToLow.filter(theme => theme.active !== false));
  // Legacy callers also read the shared optional index at most once per operation.
  const inventories = !snapshot && inventoryThemeIds.size
    ? await readThemeFileInventories(file).catch(() => null) : null;
  for (const theme of themesHighToLow) {
    if (!inventoryThemeIds.has(theme.themeId)) continue;
    if (snapshot) {
      theme.files = snapshot.themes.get(theme.themeId)!.files;
      continue;
    }
    let inventory = inventories ? inventories[theme.themeId] : null;
    if (!inventory || !inventory.length) inventory = await enumerateThemeFiles(theme.themeId, file);
    theme.files = inventory;
  }

  const plan = planActiveMappings(themesHighToLow, generation, overrides);
  const previousText = await file.readOptionalText(ACTIVE_GENERATIONS_URI);
  const generationIndex = parseGenerationIndex(previousText);
  if (plan.generation && generationIndex.generations.indexOf(plan.generation) >= 0)
    throw new Error("活动资源代次标识重复，请重新触发重载");
  if (plan.generation) {
    generationIndex.generations.push(plan.generation);
    await file.writeText(ACTIVE_GENERATIONS_URI, JSON.stringify(generationIndex));
  }

  try {
    const preparedDirectories = new Set<string>();
    for (const copy of plan.copies) await copyAsset(copy, file, preparedDirectories);
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
