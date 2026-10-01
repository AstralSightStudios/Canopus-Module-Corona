import {
  THEME_DESTINATION_ROOT, isBinFileDestination, isQuickAppIconSource,
  safeAbsoluteResourcePath, safeRelativeResourcePath, safeResourceSource, safeThemeDestination
} from "./resource-path";
import { parseResourcePackManifest } from "./resource-pack";
import type { ResourcePackManifest, ResourcePackMapping } from "./resource-pack";
import {
  enumerateThemeFiles,
  mappingMatchesRelativeFile,
  readThemeFileInventories,
  serializeResourceOrder,
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
const MAX_MAPPING_RULES = 256;
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
  protectedThemes?: string[];
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
  const index = value as StoredGenerationIndex;
  if (index.protectedThemes !== undefined &&
      (!Array.isArray(index.protectedThemes) || index.protectedThemes.some(themeId =>
        typeof themeId !== "string" || !/^[a-z0-9_-]{1,12}$/.test(themeId)) ||
        new Set(index.protectedThemes).size !== index.protectedThemes.length))
    throw new Error("活动资源包保护索引格式无效");
  return index;
}

function directlyMappedThemes(mappings: string): string[] {
  const themes = new Set<string>();
  for (const line of mappings.split(/\r?\n/)) {
    if (!line || line[0] === "#") continue;
    const separator = line.indexOf("\t");
    if (separator <= 0) continue;
    const match = /^themes\/([a-z0-9_-]{1,12})\//.exec(line.slice(separator + 1));
    if (match) themes.add(match[1]);
  }
  return Array.from(themes).sort();
}

/** Protects every possibly resident direct package until an acknowledged switch. */
export async function readProtectedThemeIds(
  file: Pick<ResourceAssetFileApi, "readOptionalText">,
): Promise<string[]> {
  return parseGenerationIndex(await file.readOptionalText(ACTIVE_GENERATIONS_URI)).protectedThemes || [];
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

function needsFileResolution(entries: SourceGroupEntry[]): boolean {
  return entries.length > 1 && entries.some(entry => entry.mapping.source.endsWith("/"));
}

function themesNeedingInventory(themesHighToLow: ThemeRules[]): Set<string> {
  const needed = new Set<string>();
  for (const group of collectSourceGroups(themesHighToLow)) {
    if (needsFileResolution(group)) group.forEach(entry => needed.add(entry.themeId));
  }
  return needed;
}

interface ResolvedFile {
  sourcePath: string;
  entry: SourceGroupEntry;
  asset: ThemeAssetFile;
}

interface ResolvedGroup {
  ruleIndex: number;
  sourceRoot: string;
  files: ResolvedFile[];
}

function serializeMappings(rules: ActiveMappingRule[]): string {
  return rules.length ? rules.map(rule => `${rule.source}\t${rule.destination}\n`).join("")
    : "# No active resource replacements.\n";
}

function validMappingRule(rule: ActiveMappingRule): boolean {
  if (!safeResourceSource(rule.source)) return false;
  if (rule.destination === SYSTEM_RESOURCE_CHOICE) return !rule.source.endsWith("/");
  if (isQuickAppIconSource(rule.source) && !isBinFileDestination(rule.destination)) return false;
  return safeThemeDestination(rule.destination) &&
    rule.source.endsWith("/") === rule.destination.endsWith("/");
}

/** Prefer exact winning-file rules; materialize all overlapping groups only if needed. */
export function planActiveMappings(
  themesHighToLow: ThemeRules[],
  generation: string,
  overrides: ResourceOverrides = Object.create(null) as ResourceOverrides,
): ActiveMappingsPlan {
  if (!/^[a-z0-9_-]{1,32}$/.test(generation)) throw new Error("活动资源代次标识无效");
  // Validate before grouping: virtual icon keys must never enter directory expansion.
  for (const theme of themesHighToLow) {
    for (const mapping of theme.manifest.mappings) {
      if (!safeRelativeResourcePath(mapping.destination) ||
          !validMappingRule({ source: mapping.source,
            destination: `${THEME_DESTINATION_ROOT}${theme.themeId}/${mapping.destination}` }))
        throw new Error("合并后的资源映射路径无效");
    }
  }
  const priorityThemes = themesHighToLow.filter(theme => theme.active !== false);
  const groups = collectSourceGroups(priorityThemes);
  let rules: ActiveMappingRule[] = [];
  const fallbackRules: ActiveMappingRule[] = [];
  const resolvedGroups: ResolvedGroup[] = [];
  for (let ruleIndex = 0; ruleIndex < groups.length; ruleIndex++) {
    const entries = groups[ruleIndex];
    if (!needsFileResolution(entries)) {
      const winner = entries[0];
      const rule = { source: winner.mapping.source,
        destination: `${THEME_DESTINATION_ROOT}${winner.themeId}/${winner.mapping.destination}` };
      rules.push(rule);
      fallbackRules.push(rule);
      continue;
    }

    const directorySources = entries.map(entry => entry.mapping.source).filter(source => source.endsWith("/"));
    const sourceRoot = directorySources.sort((left, right) => left.length - right.length)[0];
    if (!sourceRoot || entries.some(entry => !entry.mapping.source.startsWith(sourceRoot)))
      throw new Error("无法合并重叠的资源映射路径");
    for (const entry of entries) {
      if (entry.files === null)
        throw new Error(`资源包 ${entry.themeId} 缺少文件清单，无法生成叠加资源`);
    }

    fallbackRules.push({ source: sourceRoot,
      destination: `${THEME_DESTINATION_ROOT}${ACTIVE_DIRECTORY_PREFIX}${generation}/r${ruleIndex}/` });
    const resolvedGroup: ResolvedGroup = { ruleIndex, sourceRoot, files: [] };
    resolvedGroups.push(resolvedGroup);

    // Resolve longest prefix within each pack before applying pack priority.
    // Emit only concrete winners: absent files and masked broad paths must
    // still fall through to the original firmware resource.
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
          if (!safeAbsoluteResourcePath(sourcePath))
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
      if (!sourcePath.slice(sourceRoot.length))
        throw new Error(`资源映射未指向具体文件：${sourcePath}`);
      resolvedGroup.files.push({ sourcePath, entry: winner.entry, asset: winner.asset });
      rules.push({ source: sourcePath,
        destination: `${THEME_DESTINATION_ROOT}${winner.entry.themeId}/${winner.asset.relativePath}` });
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
    if (!validMappingRule({ source: sourcePath, destination }))
      throw new Error(`混搭微调资源路径超过模块限制：${sourcePath}`);
    overrideRules.push({ source: sourcePath, destination });
  }
  if (overrideRules.some(rule => !validMappingRule(rule)))
    throw new Error("合并后的资源映射路径无效");
  const overriddenPaths = new Set(overrideRules.map(rule => rule.source));
  rules = rules.filter(rule => !overriddenPaths.has(rule.source)).concat(overrideRules);
  for (const group of resolvedGroups)
    group.files = group.files.filter(winner => !overriddenPaths.has(winner.sourcePath));
  const directMappings = rules.length <= MAX_MAPPING_RULES ? serializeMappings(rules) : null;
  if (directMappings !== null && utf8Length(directMappings) <= MAX_CONFIG_BYTES &&
      rules.every(validMappingRule)) {
    for (const group of resolvedGroups) {
      for (const winner of group.files) {
        if (winner.asset.sizeBytes === 0)
          throw new Error(`不能叠加空资源文件：${THEME_ROOT_URI}${winner.entry.themeId}/${winner.asset.relativePath}`);
      }
    }
    return { mappings: directMappings, generation: null, copies: [] };
  }

  // Retain the established full-overlay fallback instead of greedily choosing
  // groups: bounded TSV size and immutable-generation cleanup stay predictable.
  rules = fallbackRules.filter(rule => !overriddenPaths.has(rule.source)).concat(overrideRules);
  if (rules.length > MAX_MAPPING_RULES)
    throw new Error(`合并后映射超过模块上限 ${MAX_MAPPING_RULES} 条`);
  const mappings = serializeMappings(rules);
  if (utf8Length(mappings) > MAX_CONFIG_BYTES)
    throw new Error("生成的活动 mappings.tsv 超过 32 KiB");
  if (rules.some(rule => !validMappingRule(rule))) throw new Error("合并后的资源映射路径无效");
  const copies: ActiveFileCopy[] = [];
  for (const group of resolvedGroups) {
    for (const winner of group.files) {
      const suffix = winner.sourcePath.slice(group.sourceRoot.length);
      const destination = `${THEME_DESTINATION_ROOT}${ACTIVE_DIRECTORY_PREFIX}${generation}/r${group.ruleIndex}/${suffix}`;
      if (!safeThemeDestination(destination))
        throw new Error(`活动资源路径超过模块限制：${destination}`);
      copies.push({
        sourceUri: `${THEME_ROOT_URI}${winner.entry.themeId}/${winner.asset.relativePath}`,
        destinationUri: `internal://files/${destination}`,
        sizeBytes: winner.asset.sizeBytes
      });
    }
  }
  return { mappings, generation: resolvedGroups.length ? generation : null, copies };
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

  // Inventory is authoritative until receiver/delete invalidation. Await native
  // completion and propagate its errors; per-asset source/destination stats are unnecessary.
  await file.copyFile(copy.sourceUri, copy.destinationUri);
}

/** Prepares a plan before publication; unchanged snapshot inputs reuse the last published plan. */
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

  const canonicalOrder = serializeResourceOrder(order);
  if (!/^[a-z0-9_-]{1,32}$/.test(generation)) throw new Error("活动资源代次标识无效");
  const cacheKey = JSON.stringify([canonicalOrder,
    Object.keys(overrides).sort().map(sourcePath => [sourcePath, overrides[sourcePath]])]);
  const activeThemeIds = order.slice(0, systemIndex);
  const installedThemeIds = order.filter(themeId => themeId !== SYSTEM_STYLE_ID);
  const activeIds = new Set(activeThemeIds);
  const selectedIds = new Set(Object.keys(overrides).map(sourcePath => overrides[sourcePath])
    .filter(choice => choice !== DEFAULT_RESOURCE_CHOICE && choice !== SYSTEM_RESOURCE_CHOICE &&
      installedThemeIds.includes(choice)));
  if (snapshot) {
    // Fail closed even on a cache hit. Identity checks are cheap; inventories
    // themselves are immutable until the shared catalog is invalidated.
    for (const themeId of new Set([...snapshot.installedThemeIds, ...installedThemeIds])) {
      const theme = snapshot.themes.get(themeId);
      if (!snapshot.installedThemeIds.includes(themeId) || !theme ||
          theme.themeId !== themeId || !theme.manifest || theme.manifest.themeId !== themeId ||
          !Array.isArray(theme.manifest.mappings) || !Array.isArray(theme.files))
        throw new Error(`资源快照缺少资源包 ${themeId}，无法生成映射`);
    }
    if (snapshot.activeMappings?.key === cacheKey) return snapshot.activeMappings.plan;
  }
  const themesHighToLow: ThemeRules[] = [];
  const loadManifest = async (themeId: string, required: boolean): Promise<ResourcePackManifest | null> => {
    if (snapshot) return snapshot.themes.get(themeId)!.manifest;
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

  // Inventories resolve overlapping groups for exact rules or the overlay fallback.
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
    const previousMappings = await file.readOptionalText(ACTIVE_MAPPINGS_URI);
    const protectedThemes = Array.from(new Set([
      ...(generationIndex.protectedThemes || []),
      ...directlyMappedThemes(previousMappings || ""),
      ...directlyMappedThemes(plan.mappings)
    ])).sort();
    // Either native write can fail after truncation. Preserve dependencies on
    // disk first, and never reuse a cache that could skip repairing the TSV.
    if (snapshot) delete snapshot.activeMappings;
    if (JSON.stringify(protectedThemes) !== JSON.stringify(generationIndex.protectedThemes || [])) {
      generationIndex.protectedThemes = protectedThemes;
      await file.writeText(ACTIVE_GENERATIONS_URI, JSON.stringify(generationIndex));
    }
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
  // Retain only the latest published plan: acknowledged cleanup may remove any
  // earlier overlay. Signal/receipt failure outside this function does not evict it.
  if (snapshot) snapshot.activeMappings = { key: cacheKey,
    plan: plan.copies.length ? { mappings: plan.mappings, generation: plan.generation, copies: [] } : plan };
  return plan;
}

/** Removes old immutable overlay trees only after the module acknowledged the new TSV. */
export async function cleanupInactiveGenerations(
  activeGeneration: string | null,
  file: ResourceAssetFileApi,
  acknowledgedMappings?: string,
): Promise<void> {
  const text = await file.readOptionalText(ACTIVE_GENERATIONS_URI);
  const index = parseGenerationIndex(text);
  const mappings = acknowledgedMappings === undefined
    ? await file.readOptionalText(ACTIVE_MAPPINGS_URI) : acknowledgedMappings;
  if (mappings === null && index.protectedThemes?.length)
    throw new Error("缺少已确认的活动映射，保留资源包保护");
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
  const next: StoredGenerationIndex = { version: 1, generations: retained };
  const protectedThemes = directlyMappedThemes(mappings || "");
  if (protectedThemes.length) next.protectedThemes = protectedThemes;
  const serialized = JSON.stringify(next);
  if ((text !== null || retained.length || protectedThemes.length) && text !== serialized)
    await file.writeText(ACTIVE_GENERATIONS_URI, serialized);
}
