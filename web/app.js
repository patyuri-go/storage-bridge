'use strict';

const ui = {
  select: document.getElementById('storage-select'),
  meta: document.getElementById('storage-meta'),
  tree: document.getElementById('tree'),
  refresh: document.getElementById('refresh-button'),
  empty: document.getElementById('empty-state'),
  details: document.getElementById('details'),
  detailsName: document.getElementById('details-name'),
  detailsType: document.getElementById('details-type'),
  detailsPath: document.getElementById('details-path'),
  detailsSize: document.getElementById('details-size'),
  detailsMtime: document.getElementById('details-mtime'),
  selectionBar: document.getElementById('selection-bar'),
  selectionCount: document.getElementById('selection-count'),
  selectionHint: document.getElementById('selection-hint'),
  selectionResult: document.getElementById('selection-result'),
  lookup: document.getElementById('lookup-button'),
  lookupModal: document.getElementById('lookup-modal'),
  lookupMeta: document.getElementById('lookup-meta'),
  lookupBody: document.getElementById('lookup-body'),
  lookupClose: document.getElementById('lookup-close'),
  lookupCopy: document.getElementById('lookup-copy'),
  lookupNote: document.getElementById('lookup-note'),
  scan: document.getElementById('scan-button'),
  scanMode: document.getElementById('scan-mode'),
  selectionClear: document.getElementById('selection-clear'),
};

const state = {
  storages: [],
  currentId: null,
  generation: 0,
  selected: null, // 详情面板当前展示的节点
  selection: new Set(), // Ctrl/⌘ 多选集合
  scanning: false,
  lookingUp: false,
  lookupIds: [], // 弹窗里待复制的实体 id
  mode: 'verify', // 扫描的 mode：verify = 校验模式，full = 完整扫描
};

// REST 调用：成功直接返回 JSON 载荷，失败抛带 message/detail 的 Error
async function request(method, path, body) {
  const options = { method, headers: {} };
  if (body !== undefined) {
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }

  let response;
  try {
    response = await fetch(path, options);
  } catch (cause) {
    throw new Error(`无法连接服务：${cause.message}`);
  }

  const text = await response.text();
  let payload = null;
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch (_) {
      payload = null;
    }
  }

  if (!response.ok) {
    const error = payload && payload.error ? payload.error : null;
    if (error) {
      const message = error.message || error.code || '请求失败';
      const detail = error.detail ? `（${error.detail}）` : '';
      throw new Error(`${message}${detail}`);
    }
    throw new Error(`HTTP ${response.status}`);
  }
  return payload;
}

function apiGet(path) {
  return request('GET', path);
}

function apiPost(path, body) {
  return request('POST', path, body);
}

function storagePath(id, suffix = '') {
  return `/api/storages/${encodeURIComponent(id)}${suffix}`;
}

function setMeta(storage) {
  ui.meta.replaceChildren();
  if (!storage) {
    return;
  }

  const basePath = storage.config && storage.config.base_path ? storage.config.base_path : '/';
  const location = document.createElement('div');
  location.textContent = `${storage.endpoint}${basePath}`;
  ui.meta.append(location);

  const status = document.createElement('div');
  status.textContent = `状态：${storage.state || 'unknown'}`;
  if (storage.state && storage.state !== 'active') {
    status.className = 'warn';
  }
  ui.meta.append(status);
}

function setTreeMessage(message, isError = false, retry = null) {
  ui.tree.replaceChildren();
  const box = document.createElement('div');
  box.className = isError ? 'tree-error' : 'tree-note';

  const text = document.createElement('div');
  text.textContent = message;
  box.append(text);

  if (retry) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = '重试';
    button.addEventListener('click', retry);
    box.append(button);
  }
  ui.tree.append(box);
}

function renderStorageOptions(previousId) {
  ui.select.replaceChildren();
  let preferred = state.storages.find((item) => item.storage_id === previousId);
  if (!preferred) {
    preferred = state.storages.find((item) => item.state === 'active') || state.storages[0];
  }

  for (const storage of state.storages) {
    const option = document.createElement('option');
    option.value = storage.storage_id;
    option.textContent = `${storage.name} — ${storage.endpoint}`;
    if (storage.state && storage.state !== 'active') {
      option.textContent += `（${storage.state}）`;
    }
    ui.select.append(option);
  }

  ui.select.disabled = false;
  ui.select.value = preferred.storage_id;
  return preferred.storage_id;
}

function showEmpty() {
  ui.empty.hidden = false;
  ui.details.hidden = true;
}

function showDetails(entry) {
  ui.detailsName.textContent = entry.name;
  ui.detailsType.textContent = entry.isDir ? '文件夹' : '文件';
  ui.detailsPath.textContent = entry.relPath;
  ui.detailsPath.title = entry.relPath;
  ui.detailsSize.textContent = entry.isDir ? '—' : formatBytes(entry.size);
  ui.detailsMtime.textContent = entry.mtime
    ? new Date(entry.mtime * 1000).toLocaleString('zh-CN')
    : '未知';

  ui.empty.hidden = true;
  ui.details.hidden = false;
}

function rowOf(node) {
  return node.querySelector(':scope > .node-row');
}

function clearSelection() {
  for (const node of state.selection) {
    const row = rowOf(node);
    if (row) {
      row.classList.remove('selected');
    }
  }
  state.selection.clear();
  state.selected = null;
  setScanResult('');
  updateSelectionBar();
}

function updateSelectionBar() {
  const nodes = [...state.selection];
  if (!nodes.length) {
    ui.selectionBar.hidden = true;
    return;
  }

  const dirs = nodes.filter((node) => node._entry.isDir).length;
  const files = nodes.length - dirs;
  const parts = [];
  if (files) {
    parts.push(`${files} 个文件`);
  }
  if (dirs) {
    parts.push(`${dirs} 个文件夹（递归）`);
  }

  ui.selectionCount.textContent = `已选 ${nodes.length} 项`;
  ui.selectionHint.textContent = parts.join(' · ');
  ui.selectionBar.hidden = false;
}

function setScanResult(text, { error = false, title = '' } = {}) {
  if (!text) {
    ui.selectionResult.hidden = true;
    ui.selectionResult.textContent = '';
    ui.selectionResult.title = '';
    return;
  }
  ui.selectionResult.textContent = text;
  ui.selectionResult.title = title;
  ui.selectionResult.classList.toggle('error', error);
  ui.selectionResult.hidden = false;
}

// additive=true 为 Ctrl/⌘ 点击：切换该节点，不展开
function selectNode(node, additive) {
  if (additive && state.selection.has(node)) {
    state.selection.delete(node);
    const row = rowOf(node);
    if (row) {
      row.classList.remove('selected');
    }
    if (state.selected === node) {
      state.selected = null;
      showEmpty();
    }
    updateSelectionBar();
    return;
  }

  if (!additive) {
    for (const other of state.selection) {
      if (other === node) {
        continue;
      }
      const row = rowOf(other);
      if (row) {
        row.classList.remove('selected');
      }
    }
    state.selection.clear();
  }

  state.selection.add(node);
  const row = rowOf(node);
  if (row) {
    row.classList.add('selected');
  }
  state.selected = node;
  showDetails(node._entry);
  updateSelectionBar();
}

function createNode(entry, depth) {
  const node = document.createElement('div');
  node.className = 'node';
  node.dataset.path = entry.relPath || '/';
  node.dataset.dir = entry.isDir ? '1' : '0';
  node.dataset.depth = String(depth);
  node.dataset.loaded = '0';
  node.dataset.loading = '0';
  node._entry = entry;

  const row = document.createElement('div');
  row.className = 'node-row';
  row.style.setProperty('--depth', String(depth));
  row.tabIndex = 0;
  row.setAttribute('role', 'treeitem');
  row.title = entry.relPath;
  if (entry.isDir) {
    row.setAttribute('aria-expanded', 'false');
  }

  const twisty = document.createElement(entry.isDir ? 'button' : 'span');
  twisty.className = entry.isDir ? 'twisty' : 'twisty leaf';
  if (entry.isDir) {
    twisty.type = 'button';
    twisty.tabIndex = -1;
    twisty.textContent = '▸';
    twisty.setAttribute('aria-hidden', 'true');
  }

  const icon = document.createElement('span');
  icon.className = 'icon';
  icon.textContent = entry.isDir ? '📁' : '📄';
  icon.setAttribute('aria-hidden', 'true');

  const name = document.createElement('span');
  name.className = 'node-name';
  name.textContent = entry.name;

  row.append(twisty, icon, name);

  const children = document.createElement('div');
  children.className = 'children';
  children.hidden = true;

  node.append(row, children);
  return node;
}

function joinPath(base, name) {
  if (!base || base === '/') {
    return `/${name}`;
  }
  return `${base.replace(/\/+$/, '')}/${name}`;
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes < 1024) {
    return `${bytes} B`;
  }
  const units = ['KiB', 'MiB', 'GiB', 'TiB', 'PiB'];
  let amount = bytes;
  let index = -1;
  do {
    amount /= 1024;
    index += 1;
  } while (amount >= 1024 && index < units.length - 1);
  return `${amount.toFixed(amount >= 10 ? 0 : 1)} ${units[index]}`;
}

async function loadChildren(node, generation) {
  if (node.dataset.dir !== '1' || node.dataset.loaded === '1' || node.dataset.loading === '1') {
    return;
  }

  const path = node.dataset.path;
  const children = node.querySelector(':scope > .children');
  node.dataset.loading = '1';

  const loading = document.createElement('div');
  loading.className = 'tree-note';
  loading.textContent = '加载中…';
  children.append(loading);

  try {
    const query = new URLSearchParams({ path });
    const data = await apiGet(storagePath(state.currentId, `/files?${query}`));
    if (generation !== state.generation) {
      return;
    }

    loading.remove();
    children.replaceChildren();

    const entries = Array.isArray(data.entries) ? data.entries : [];
    if (!entries.length) {
      const empty = document.createElement('div');
      empty.className = 'tree-note';
      empty.textContent = '空文件夹';
      children.append(empty);
    } else {
      for (const item of entries) {
        const childPath = item.rel_path || joinPath(path, item.name);
        const child = createNode(
          {
            name: item.name,
            relPath: childPath,
            isDir: Boolean(item.is_dir),
            size: item.size || 0,
            mtime: item.mtime || 0,
            uri: item.uri || '',
          },
          Number(node.dataset.depth || 0) + 1,
        );
        children.append(child);
      }
    }
    node.dataset.loaded = '1';
  } catch (error) {
    if (generation !== state.generation) {
      return;
    }

    loading.remove();
    const box = document.createElement('div');
    box.className = 'tree-error';

    const text = document.createElement('div');
    text.textContent = `加载失败：${error.message}`;
    box.append(text);

    const retry = document.createElement('button');
    retry.type = 'button';
    retry.textContent = '重试';
    retry.addEventListener('click', () => {
      box.remove();
      node.dataset.loading = '0';
      loadChildren(node, generation);
    });
    box.append(retry);
    children.append(box);
  } finally {
    node.dataset.loading = '0';
  }
}

// focus=false 时只展开/收起，不改变选中态（用于打开库时自动展开根目录）
function toggleNode(node, expand, generation, focus = true) {
  if (node.dataset.dir !== '1') {
    if (focus) {
      selectNode(node, false);
    }
    return;
  }

  const children = node.querySelector(':scope > .children');
  const open = typeof expand === 'boolean' ? expand : children.hidden;
  children.hidden = !open;
  node.classList.toggle('open', open);

  const twisty = node.querySelector(':scope > .node-row > .twisty');
  if (twisty) {
    twisty.textContent = open ? '▾' : '▸';
  }
  const row = node.querySelector(':scope > .node-row');
  row.setAttribute('aria-expanded', String(open));

  if (open) {
    loadChildren(node, generation);
  }
  if (focus) {
    selectNode(node, false);
  }
}

function openStorage(storageId) {
  const storage = state.storages.find((item) => item.storage_id === storageId);
  if (!storage) {
    return;
  }

  state.currentId = storageId;
  state.generation += 1;
  state.selection.clear();
  state.selected = null;
  setScanResult('');
  updateSelectionBar();

  if (ui.select.value !== storageId) {
    ui.select.value = storageId;
  }

  setMeta(storage);
  showEmpty();
  ui.tree.replaceChildren();

  const root = createNode(
    {
      name: '根目录',
      relPath: '/',
      isDir: true,
      size: 0,
      mtime: 0,
      uri: '',
    },
    0,
  );
  ui.tree.append(root);
  toggleNode(root, true, state.generation, false);
}

async function refreshStorages() {
  const previousId = state.currentId;
  ui.refresh.disabled = true;
  ui.select.disabled = true;
  setMeta(null);

  try {
    const data = await apiGet('/api/storages');
    state.storages = Array.isArray(data) ? data : [];

    if (!state.storages.length) {
      state.currentId = null;
      const option = document.createElement('option');
      option.textContent = '（无资源库）';
      ui.select.replaceChildren(option);
      ui.select.disabled = true;
      setTreeMessage('还没有资源库。先通过 POST /api/storages 创建。');
      showEmpty();
      return;
    }

    const selectedId = renderStorageOptions(previousId);
    openStorage(selectedId);
  } catch (error) {
    state.storages = [];
    state.currentId = null;
    const option = document.createElement('option');
    option.textContent = '加载失败';
    ui.select.replaceChildren(option);
    ui.select.disabled = true;
    setMeta(null);
    setTreeMessage(`加载资源库失败：${error.message}`, true, refreshStorages);
    showEmpty();
  } finally {
    ui.refresh.disabled = false;
  }
}

ui.select.addEventListener('change', () => {
  if (ui.select.value) {
    openStorage(ui.select.value);
  }
});

ui.refresh.addEventListener('click', () => {
  refreshStorages();
});

// 选中的目录会递归扫描，剔除「已被上级目录覆盖」的路径，避免重复读
function dedupeTargets(folders, files) {
  const kept = [];
  for (const path of [...new Set(folders)].sort()) {
    const covered = kept.some((parent) => parent === '/' || path === parent || path.startsWith(`${parent}/`));
    if (!covered) {
      kept.push(path);
    }
  }
  const keptFiles = [...new Set(files)].filter(
    (file) => !kept.some((dir) => dir === '/' || file === dir || file.startsWith(`${dir}/`)),
  );
  return { folders: kept, files: keptFiles };
}

async function runScan() {
  if (state.scanning || !state.selection.size) {
    return;
  }

  const folders = [];
  const files = [];
  for (const node of state.selection) {
    const path = node._entry.relPath || '/';
    if (node._entry.isDir) {
      folders.push(path);
    } else {
      files.push(path);
    }
  }
  const targets = dedupeTargets(folders, files);
  const mode = state.mode === 'full' ? 'full' : 'verify';

  state.scanning = true;
  ui.scan.disabled = true;
  ui.scan.textContent = '提交中…';
  ui.scanMode.disabled = true;
  ui.selectionClear.disabled = true;
  setScanResult('正在提交扫描任务…');

  try {
    const data = await apiPost(storagePath(state.currentId, '/scans'), {
      files: targets.files,
      folders: targets.folders,
      mode,
    });
    const queued = (data && data.queued) || 0;
    const label = (data && data.mode === 'full') ? '完整扫描' : '校验模式';
    setScanResult(`已提交 · ${queued} 个任务在后台扫描（${label}）`, {
      title:
        mode === 'verify'
          ? '校验模式：同名同上级目录且大小一致的文件会被跳过，不重新算哈希。结果写入 PostgreSQL，进度看服务端日志'
          : '完整扫描：所有文件都重新算哈希。结果写入 PostgreSQL，进度看服务端日志',
    });
  } catch (error) {
    setScanResult(`提交失败：${error.message}`, { error: true });
  } finally {
    state.scanning = false;
    ui.scan.disabled = false;
    ui.scan.textContent = '扫描';
    ui.scanMode.disabled = false;
    ui.selectionClear.disabled = false;
  }
}

// 选中的目录/文件路径去重后交给后端递归遍历（后端还会再做嵌套去重）
function selectedPaths() {
  return [...new Set([...state.selection].map((node) => node._entry.relPath || '/'))];
}

async function runLookup() {
  if (state.lookingUp || !state.selection.size) {
    return;
  }

  const paths = selectedPaths();
  state.lookingUp = true;
  ui.lookup.disabled = true;
  ui.lookup.textContent = '获取中…';
  setScanResult(`正在获取 ${paths.length} 条路径的实体 id…`);

  try {
    const data = await apiPost(storagePath(state.currentId, '/entities/lookup'), { paths });
    setScanResult('');
    showLookupResult(data, paths.length);
  } catch (error) {
    setScanResult(`获取失败：${error.message}`, { error: true });
  } finally {
    state.lookingUp = false;
    ui.lookup.disabled = false;
    ui.lookup.textContent = '获取';
  }
}

function showLookupResult(data, pathCount) {
  const files = Array.isArray(data && data.files) ? data.files : [];
  const errors = Array.isArray(data && data.errors) ? data.errors : [];
  const scanned = Number(data && data.scanned) || 0;
  const missing = Number(data && data.missing) || 0;

  const meta = [`选了 ${pathCount} 条路径`, `扫到 ${scanned} 个文件`, `命中 ${files.length} 个实体`];
  if (missing > 0) {
    meta.push(`${missing} 个文件没有实体记录`);
  }
  if (data && data.truncated) {
    meta.push('遍历超时，结果不完整');
  }
  if (errors.length) {
    meta.push(`${Number(data && data.error_count) || errors.length} 条路径读取失败`);
  }
  ui.lookupMeta.textContent = meta.join(' · ');

  ui.lookupBody.replaceChildren();
  if (!files.length) {
    const empty = document.createElement('div');
    empty.className = 'lookup-empty';
    empty.textContent = '没有找到对应的实体 id。';
    ui.lookupBody.append(empty);
  } else {
    for (const item of files) {
      const row = document.createElement('div');
      row.className = 'lookup-row';

      const path = document.createElement('span');
      path.className = 'lookup-path';
      path.textContent = item.rel_path;
      path.title = item.rel_path;

      const id = document.createElement('code');
      id.className = 'lookup-id';
      id.textContent = item.entity_id;

      row.append(path, id);
      ui.lookupBody.append(row);
    }
  }

  if (errors.length) {
    const box = document.createElement('div');
    box.className = 'lookup-errors';

    const title = document.createElement('div');
    title.className = 'lookup-errors-title';
    title.textContent = '读取失败的路径';
    box.append(title);

    for (const item of errors) {
      const line = document.createElement('div');
      line.className = 'lookup-error';
      line.textContent = `${item.rel_path}：${item.message}`;
      box.append(line);
    }
    ui.lookupBody.append(box);
  }

  state.lookupIds = [...new Set(files.map((item) => item.entity_id))];
  ui.lookupCopy.disabled = state.lookupIds.length === 0;
  ui.lookupNote.textContent = '';
  ui.lookupNote.classList.remove('error');
  ui.lookupModal.hidden = false;
  ui.lookupClose.focus();
}

function closeLookup() {
  ui.lookupModal.hidden = true;
  state.lookupIds = [];
}

// http:// 下 navigator.clipboard 不可用，退回到隐藏 textarea + execCommand
async function writeClipboard(text) {
  if (navigator.clipboard && window.isSecureContext) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (_) {
      // 继续走兜底方案
    }
  }
  try {
    const area = document.createElement('textarea');
    area.value = text;
    area.setAttribute('readonly', '');
    area.style.position = 'fixed';
    area.style.top = '-1000px';
    document.body.append(area);
    area.select();
    const ok = document.execCommand('copy');
    area.remove();
    return ok;
  } catch (_) {
    return false;
  }
}

async function copyLookupIds() {
  if (!state.lookupIds.length) {
    return;
  }
  const ok = await writeClipboard(state.lookupIds.join('\n'));
  ui.lookupNote.textContent = ok
    ? `已复制 ${state.lookupIds.length} 个实体 id`
    : '复制失败，请手动选择复制';
  ui.lookupNote.classList.toggle('error', !ok);
}

ui.tree.addEventListener('click', (event) => {
  const row = event.target.closest('.node-row');
  if (!row || !ui.tree.contains(row)) {
    return;
  }
  const node = row.parentElement;
  if (event.ctrlKey || event.metaKey) {
    selectNode(node, true);
    return;
  }
  if (node.dataset.dir === '1') {
    toggleNode(node, undefined, state.generation);
  } else {
    selectNode(node, false);
  }
});

ui.tree.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') {
    clearSelection();
    showEmpty();
    return;
  }
  if (event.key !== 'Enter' && event.key !== ' ') {
    return;
  }
  const row = event.target.closest('.node-row');
  if (!row) {
    return;
  }
  event.preventDefault();
  const node = row.parentElement;
  if (event.ctrlKey || event.metaKey) {
    selectNode(node, true);
    return;
  }
  if (node.dataset.dir === '1') {
    toggleNode(node, undefined, state.generation);
  } else {
    selectNode(node, false);
  }
});

ui.scanMode.addEventListener('change', () => {
  state.mode = ui.scanMode.value === 'full' ? 'full' : 'verify';
  setScanResult('');
});

ui.scan.addEventListener('click', () => {
  runScan();
});

ui.selectionClear.addEventListener('click', () => {
  clearSelection();
  showEmpty();
});

ui.lookup.addEventListener('click', () => {
  runLookup();
});

ui.lookupClose.addEventListener('click', () => {
  closeLookup();
});

ui.lookupCopy.addEventListener('click', () => {
  copyLookupIds();
});

ui.lookupModal.addEventListener('click', (event) => {
  if (event.target === ui.lookupModal) {
    closeLookup();
  }
});

document.addEventListener(
  'keydown',
  (event) => {
    if (event.key === 'Escape' && !ui.lookupModal.hidden) {
      event.preventDefault();
      event.stopPropagation();
      closeLookup();
    }
  },
  true,
);

refreshStorages();
