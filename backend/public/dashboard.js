const $ = id => document.getElementById(id);
const titles = { overview: 'Overview', assistant: 'My assistant', providers: 'API status', devices: 'Devices', conversations: 'Conversations', settings: 'Settings' };
let state = null;
let activePage = 'overview';
let toastTimer;
let previewNotice = '';
const text = (tag, content, className) => { const node = document.createElement(tag); node.textContent = content; if (className) node.className = className; return node; };
const empty = (container, message) => container.replaceChildren(text('div', message, 'empty'));
const date = value => value ? new Date(value).toLocaleString(undefined, { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' }) : 'Not connected yet';
function toast(message) { $('toast').textContent = message; $('toast').hidden = false; clearTimeout(toastTimer); toastTimer = setTimeout(() => { $('toast').hidden = true; }, 4500); }
function notice(message) { const value = message || previewNotice; $('notice').textContent = value; $('notice').hidden = !value; }
async function api(path, options = {}) {
  const response = await fetch('/api/admin/' + path, { cache: 'no-store', ...options,
    headers: { 'Content-Type': 'application/json', ...options.headers } });
  const result = await response.json();
  if (!response.ok) {
    if (response.status === 401 && path !== 'login') { $('console').hidden = true; $('login').hidden = false; }
    throw new Error(result.error || 'Please try again.');
  }
  return result;
}
function button(label, action, style = 'secondary') {
  const node = text('button', label, style); node.type = 'button';
  node.onclick = async () => { node.disabled = true; try { await action(); } catch (error) { toast(error.message); } finally { node.disabled = false; } };
  return node;
}
function navigate(view) {
  if (!titles[view]) view = 'overview';
  activePage = view;
  document.querySelectorAll('[data-page]').forEach(node => { node.hidden = node.dataset.page !== view; });
  document.querySelectorAll('[data-view]').forEach(node => node.classList.toggle('active', node.dataset.view === view));
  $('breadcrumb').textContent = titles[view];
  history.replaceState(null, '', '#' + view);
  if (view === 'conversations' && state) loadConversations().catch(error => toast(error.message));
}
document.querySelectorAll('[data-view]').forEach(node => { node.onclick = () => navigate(node.dataset.view); });
document.querySelectorAll('[data-go]').forEach(node => { node.onclick = () => navigate(node.dataset.go); });
window.addEventListener('hashchange', () => navigate(location.hash.slice(1)));

function renderSessions() {
  const rows = state.summary.recentSessions;
  if (!rows.length) return empty($('recentSessions'), 'Your first voice session will appear here. Connect your ESP32 to get started.');
  const table = document.createElement('table');
  const head = document.createElement('thead'); const header = document.createElement('tr');
  for (const label of ['Device', 'Started', 'Session']) header.append(text('th', label));
  head.append(header); table.append(head);
  const body = document.createElement('tbody');
  for (const session of rows) {
    const row = document.createElement('tr');
    row.append(text('td', state.devices.find(device => device.id === session.device_id)?.name || session.device_id), text('td', date(session.started_at)));
    const cell = document.createElement('td'); cell.append(text('span', session.ended_at ? 'Ended' : 'Opened', 'status-chip' + (session.ended_at ? ' off' : '')));
    row.append(cell); body.append(row);
  }
  table.append(body); $('recentSessions').replaceChildren(table);
}
function renderKeys() {
  const visible = state.profile.keys;
  if (!visible.length) return empty($('keysList'), 'Set GEMINI_API_KEYS in Vercel Production environment variables, then redeploy.');
  const nodes = visible.map((key, i) => {
    const row = text('div', '', 'key-row');
    row.append(text('span', `Key ${i + 1}`), text('code', key.mask));
    const seconds = state.keys.find(item => item.id === key.id)?.cooldownSeconds || 0;
    row.append(text('span', seconds ? `Cooldown · ${seconds}s` : 'No cooldown', 'status-chip' + (seconds ? ' off' : '')));
    return row;
  });
  $('keysList').replaceChildren(...nodes);
}
function revealToken(value) { $('deviceToken').value = value; $('secretDialog').showModal(); }
function confirmAction(message) {
  $('confirmMessage').textContent = message;
  return new Promise(resolve => {
    const dialog = $('confirmDialog');
    const done = () => { dialog.removeEventListener('close', done); resolve(dialog.returnValue === 'confirm'); };
    dialog.returnValue = 'cancel'; dialog.addEventListener('close', done); dialog.showModal();
  });
}
$('secretDialog').addEventListener('close', () => { $('deviceToken').value = ''; });
function renderDevices() {
  if (!state.devices.length) empty($('devicesList'), 'No devices yet. Your first authenticated ESP32 connection will appear here.');
  else $('devicesList').replaceChildren(...state.devices.map(device => {
    const card = text('article', '', 'panel device-card');
    const online = device.enabled && device.state !== 'offline' && device.last_seen && Date.now() - Date.parse(device.last_seen) < 60000;
    card.append(text('span', !device.enabled ? 'Access paused' : online ? device.state : 'Offline', 'status-chip' + (online ? '' : ' off')));
    const name = document.createElement('input'); name.value = device.name; name.maxLength = 80; name.setAttribute('aria-label', `Name for ${device.id}`); card.append(name);
    card.append(text('p', device.id, 'device-id'), text('p', 'Last seen · ' + date(device.last_seen), 'fine'));
    const actions = text('div', '', 'device-actions');
    actions.append(button('Save name', async () => { await api('devices/' + encodeURIComponent(device.id), { method: 'POST', body: JSON.stringify({ name: name.value, enabled: device.enabled }) }); await load(); toast('Device name saved.'); }),
      button(device.enabled ? 'Pause access' : 'Enable access', async () => { await api('devices/' + encodeURIComponent(device.id), { method: 'POST', body: JSON.stringify({ name: device.name, enabled: !device.enabled }) }); await load(); toast(device.enabled ? 'Device access paused. Active sessions close within 20 seconds.' : 'Device access enabled.'); }),
      button('Rotate token', async () => { if (!await confirmAction('Create a new token? This device will need setup again to reconnect.')) return; const result = await api('devices/' + encodeURIComponent(device.id) + '/token', { method: 'POST', body: '{}' }); revealToken(result.token); }));
    card.append(actions); return card;
  }));
  const selected = $('historyDevice').value;
  $('historyDevice').replaceChildren(new Option('All devices', ''), ...state.devices.map(device => new Option(device.name, device.id)));
  if (state.devices.some(device => device.id === selected)) $('historyDevice').value = selected;
}
function render() {
  const profile = state.profile;
  $('heroName').textContent = profile.name;
  $('heroBadge').textContent = state.backend.configured ? 'VOICE SETTINGS READY' : 'PROVIDER SETUP REQUIRED';
  $('metricConnected').textContent = state.summary.connected;
  $('metricDevices').textContent = `${state.summary.devices} device${state.summary.devices === 1 ? '' : 's'} in your workspace`;
  $('metricSessions').textContent = state.summary.sessions;
  $('metricKeys').textContent = profile.keys.length;
  const cooling = state.keys.filter(key => key.cooldownSeconds > 0).length;
  $('metricCooldown').textContent = cooling ? `${cooling} key${cooling === 1 ? '' : 's'} cooling down` : 'Automatic rate-limit rotation';
  $('voiceName').textContent = profile.voice; $('modelName').textContent = profile.model;
  $('assistantName').value = profile.name; $('assistantModel').value = profile.model;
  $('assistantVoice').value = profile.voice; $('instructions').value = profile.instructions;
  $('cooldown').value = profile.keyCooldownMs / 1000; $('historyEnabled').checked = profile.historyEnabled;
  $('deviceBackend').value = state.backend.base; $('settingsBackend').textContent = state.backend.base || 'Set PUBLIC_BASE_URL in Vercel';
  renderSessions(); renderKeys(); renderDevices();
}
async function load() {
  try { state = await api('state'); render(); notice(''); if (activePage === 'conversations') await loadConversations(); }
  catch (error) { notice(error.message); throw error; }
}
function profileBody() {
  if (!state) throw new Error('Connect your database before saving settings.');
  return { revision: state.profile.revision, name: $('assistantName').value, model: $('assistantModel').value,
    voice: $('assistantVoice').value, instructions: $('instructions').value, historyEnabled: $('historyEnabled').checked,
    keyCooldownSeconds: Number($('cooldown').value) };
}
async function save(event) {
  event.preventDefault(); const submit = event.currentTarget.querySelector('button[type=submit]'); submit.disabled = true;
  try { await api('profile', { method: 'POST', body: JSON.stringify(profileBody()) }); await load(); toast('Saved. New voice sessions will use your settings.'); }
  catch (error) { toast(error.message); } finally { submit.disabled = false; }
}
$('assistantForm').onsubmit = save;
$('loginForm').onsubmit = async event => {
  event.preventDefault(); const button = event.currentTarget.querySelector('button'); button.disabled = true;
  try { await api('login', { method: 'POST', body: JSON.stringify({ token: $('adminToken').value }) }); $('adminToken').value = ''; $('login').hidden = true; $('console').hidden = false; navigate(location.hash.slice(1)); await load(); }
  catch (error) { if (!$('login').hidden) $('loginStatus').textContent = error.message; else notice(error.message); }
  finally { button.disabled = false; }
};
async function logout() { try { await api('logout', { method: 'POST', body: '{}' }); } catch (error) { toast(error.message); return; } state = null; $('console').hidden = true; $('login').hidden = false; $('loginStatus').textContent = 'Signed out.'; }
$('logout').onclick = logout; $('mobileLogout').onclick = logout;
$('refresh').onclick = async () => { try { await load(); if (activePage === 'conversations') await loadConversations(); toast('Workspace refreshed.'); } catch (error) { toast(error.message); } };
$('deviceForm').onsubmit = async event => {
  event.preventDefault(); try { const result = await api('devices', { method: 'POST', body: JSON.stringify({ id: $('deviceId').value.trim(), name: $('deviceName').value.trim() }) }); $('deviceForm').reset(); await load(); revealToken(result.token); } catch (error) { toast(error.message); }
};
async function copy(field) { try { await navigator.clipboard.writeText($(field).value); toast('Copied.'); } catch { $(field).select(); toast('Select and copy this value.'); } }
$('copyBackend').onclick = () => copy('deviceBackend'); $('copyToken').onclick = () => copy('deviceToken');
async function loadConversations() {
  const messages = await api('conversations?device=' + encodeURIComponent($('historyDevice').value));
  if (!messages.length) return empty($('conversationsList'), 'No saved conversations yet. Connect your ESP32 and ask your first question.');
  $('conversationsList').replaceChildren(...messages.reverse().map(message => {
    const card = text('article', '', 'conversation-message ' + (message.role === 'assistant' ? 'assistant' : 'user'));
    const head = text('div', '', 'message-header');
    head.append(text('span', message.role === 'assistant' ? state.profile.name : state.devices.find(device => device.id === message.device_id)?.name || message.device_id), text('span', date(message.created_at)));
    card.append(head, text('p', message.text)); return card;
  }));
}
$('loadHistory').onclick = () => loadConversations().catch(error => toast(error.message));
$('historyDevice').onchange = () => loadConversations().catch(error => toast(error.message));
$('clearHistory').onclick = async () => {
  if (!await confirmAction('Permanently delete saved transcripts for the selected device, or all devices if none is selected?')) return;
  try { await api('conversations?device=' + encodeURIComponent($('historyDevice').value), { method: 'DELETE' }); await loadConversations(); toast('Conversation history cleared.'); } catch (error) { toast(error.message); }
};
try {
  const session = await api('session');
  if (session.localPreview) previewNotice = 'Local UI preview · Test data only. Voice is not connected.';
  if (!session.adminConfigured) { $('loginStatus').textContent = 'Set ADMIN_TOKEN privately in Vercel, then redeploy to enable sign-in.'; $('loginForm').querySelector('button').disabled = true; }
  else if (session.authenticated) { $('login').hidden = true; $('console').hidden = false; navigate(location.hash.slice(1)); await load(); }
  else $('loginStatus').textContent = 'Your keys and conversations stay in your private workspace.';
} catch (error) { if (!$('login').hidden) $('loginStatus').textContent = error.message; else notice(error.message); }
