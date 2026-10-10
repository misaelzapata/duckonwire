const presets = JSON.parse(document.querySelector('#presets-data').textContent);
const editor = document.querySelector('#sql');
const run = document.querySelector('#run');
const notice = document.querySelector('#notice');
const meta = document.querySelector('#result-meta');
const results = document.querySelector('#results');
const pagination = document.querySelector('#pagination');
const pageSize = document.querySelector('#page-size');
const previousPage = document.querySelector('#previous-page');
const nextPage = document.querySelector('#next-page');
let selected = null;
let selection = 0;
let queryPage = null;

function showNotice(text, error = false) {
  notice.textContent = text;
  notice.classList.toggle('error', error);
}

async function api(path, options = {}) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 35000);
  try {
    const response = await fetch(path, { ...options, signal: controller.signal });
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || 'The request could not be completed.');
    return body;
  } finally {
    clearTimeout(timeout);
  }
}

function renderRows(data, title) {
  const table = document.createElement('table');
  const head = table.createTHead().insertRow();
  data.columns.forEach(name => {
    const item = document.createElement('th');
    item.textContent = name;
    head.append(item);
  });
  const body = table.createTBody();
  data.rows.forEach(row => {
    const item = body.insertRow();
    row.forEach(value => {
      const cell = item.insertCell();
      cell.textContent = value === null ? 'null' : String(value);
      if (value === null) cell.className = 'null';
    });
  });
  results.replaceChildren(table);
  document.querySelector('#result-title').textContent = title;
  meta.textContent = `${data.row_count} rows · ${data.elapsed_ms} ms on server`;
  showNotice(data.truncated
    ? data.next_offset === null
      ? 'Narrow the query or use an ID filter to explore further.'
      : 'More rows are available.'
    : data.row_count ? '' : 'The query returned no rows.');
}

function updatePagination(data) {
  pagination.hidden = !queryPage;
  if (!queryPage) return;
  const first = data.row_count ? data.offset + 1 : 0;
  const last = data.row_count ? data.offset + data.row_count : 0;
  document.querySelector('#page-range').textContent = data.row_count
    ? `Rows ${first.toLocaleString()}–${last.toLocaleString()}` : 'No rows on this page';
  previousPage.disabled = data.offset === 0;
  nextPage.disabled = data.next_offset === null;
}

function quoteIdentifier(value) {
  return '"' + value.replaceAll('"', '""') + '"';
}

async function selectTable(table, button) {
  const current = ++selection;
  selected = table;
  queryPage = null;
  pagination.hidden = true;
  document.querySelectorAll('.table-button').forEach(item => item.classList.remove('active'));
  button.classList.add('active');
  meta.textContent = 'Loading table preview…';
  try {
    const data = await api('/table?' + new URLSearchParams({ schema: table.schema, name: table.name }));
    if (current !== selection) return;
    document.querySelector('#details').classList.add('open');
    document.querySelector('#detail-title').textContent = table.schema + '.' + table.name;
    const columns = document.querySelector('#columns');
    columns.replaceChildren();
    data.columns.forEach(column => {
      const item = document.createElement('div');
      item.className = 'column';
      item.textContent = column.name;
      const type = document.createElement('span');
      type.textContent = column.type;
      item.append(type);
      if (column.nullable) {
        const nullable = document.createElement('em');
        nullable.textContent = 'null';
        item.append(nullable);
      }
      columns.append(item);
    });
    renderRows(data.preview, 'Table preview');
  } catch (error) {
    if (current === selection) {
      showNotice(error.name === 'AbortError' ? 'The preview reached its time limit.' : error.message, true);
      meta.textContent = 'Preview unavailable';
    }
  }
}

document.querySelector('#use-table').addEventListener('click', () => {
  if (!selected) return;
  editor.value = 'SELECT *\nFROM ' + quoteIdentifier(selected.schema) + '.' + quoteIdentifier(selected.name);
  editor.focus();
});

document.querySelectorAll('[data-preset]').forEach(button => {
  button.addEventListener('click', () => {
    editor.value = presets[Number(button.dataset.preset)].sql;
    editor.focus();
  });
});

async function executePage(source, limit, offset) {
  if (run.disabled) return;
  const current = ++selection;
  run.disabled = true;
  previousPage.disabled = true;
  nextPage.disabled = true;
  run.textContent = 'Running…';
  showNotice('');
  meta.textContent = 'Query in progress';
  try {
    const data = await api('/query', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sql: source, limit, offset }),
    });
    if (current !== selection) return;
    queryPage = { source, limit, offset: data.offset, nextOffset: data.next_offset };
    renderRows(data, 'Query results');
    updatePagination(data);
  } catch (error) {
    if (current === selection) {
      showNotice(error.name === 'AbortError'
        ? 'The request reached its time limit. Try a smaller query.' : error.message, true);
      meta.textContent = 'Query not completed';
      if (queryPage) {
        previousPage.disabled = queryPage.offset === 0;
        nextPage.disabled = queryPage.nextOffset === null;
      }
    }
  } finally {
    run.disabled = false;
    run.textContent = 'Run query →';
  }
}

document.querySelector('#query-form').addEventListener('submit', event => {
  event.preventDefault();
  executePage(editor.value, Number(pageSize.value), 0);
});

previousPage.addEventListener('click', () => {
  if (queryPage) executePage(queryPage.source, queryPage.limit, Math.max(0, queryPage.offset - queryPage.limit));
});
nextPage.addEventListener('click', () => {
  if (queryPage && queryPage.nextOffset !== null) executePage(queryPage.source, queryPage.limit, queryPage.nextOffset);
});

editor.addEventListener('keydown', event => {
  if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) {
    event.preventDefault();
    document.querySelector('#query-form').requestSubmit();
  }
});

(async () => {
  try {
    const data = await api('/tables');
    const container = document.querySelector('#tables');
    container.replaceChildren();
    document.querySelector('#table-count').textContent = data.tables.length;
    data.tables.forEach(table => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'table-button';
      const icon = document.createElement('span');
      icon.className = 'table-icon';
      icon.setAttribute('aria-hidden', 'true');
      const label = document.createElement('span');
      const name = document.createElement('strong');
      const schema = document.createElement('small');
      name.textContent = table.name;
      schema.textContent = table.schema + ' · ' + table.type;
      label.append(name, schema);
      button.append(icon, label);
      button.addEventListener('click', () => selectTable(table, button));
      container.append(button);
    });
    const preferred = data.tables.findIndex(table => table.name === 'products');
    const index = preferred < 0 ? 0 : preferred;
    if (data.tables[index]) await selectTable(data.tables[index], container.children[index]);
    else container.textContent = 'No tables are available.';
  } catch (error) {
    document.querySelector('#tables').textContent = 'Catalog unavailable';
    document.querySelector('#table-count').textContent = '—';
    showNotice(error.message, true);
  }
})();
