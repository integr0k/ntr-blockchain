const byId = (id) => document.getElementById(id);
const numberFormat = new Intl.NumberFormat('en-US');
let currentSnapshot = null;
let toastTimer = null;
let wallets = [];

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (character) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  })[character]);
}

function ntr(value) {
  return (Number(value || 0) / 1_000_000).toLocaleString('en-US', {
    minimumFractionDigits: 2,
    maximumFractionDigits: 2,
  });
}

function shortHash(value, size = 14) {
  if (!value || value.length <= size) return value || '—';
  return `${value.slice(0, size)}…${value.slice(-6)}`;
}

function relativeTime(timestamp) {
  const seconds = Math.max(0, Math.floor(Date.now() / 1000 - Number(timestamp)));
  if (seconds >= 86_400) {
    return new Intl.DateTimeFormat('en-GB', { day: '2-digit', month: 'short', year: 'numeric' })
      .format(new Date(Number(timestamp) * 1000));
  }
  if (seconds < 5) return 'just now';
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  return `${Math.floor(seconds / 3600)}h ago`;
}

function showToast(message) {
  const toast = byId('toast');
  toast.textContent = message;
  toast.classList.add('visible');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('visible'), 3200);
}

function renderBlocks(blocks) {
  byId('table-count').textContent = `${numberFormat.format(blocks.length)} SHOWN`;
  if (!blocks.length) {
    byId('blocks-table').innerHTML = '<tr><td class="empty-row" colspan="5">No blocks recorded</td></tr>';
    return;
  }
  byId('blocks-table').innerHTML = blocks.map((block) => `
    <tr>
      <td>#${numberFormat.format(block.height)}</td>
      <td class="hash-cell" title="${escapeHtml(block.hash)}">${escapeHtml(shortHash(block.hash, 16))}</td>
      <td class="miner-cell" title="${escapeHtml(block.minerAddress)}">${escapeHtml(block.minerAddress === 'genesis' ? 'GENESIS' : shortHash(block.minerAddress, 17))}</td>
      <td class="reward-cell">${ntr(block.reward)} NTR</td>
      <td class="time-cell">${relativeTime(block.timestamp)}</td>
    </tr>`).join('');
}

function renderBalances(balances) {
  byId('balance-count').textContent = String(balances.length).padStart(2, '0');
  if (!balances.length) {
    byId('balances-list').innerHTML = '<div class="empty-row">No payouts yet</div>';
    return;
  }
  byId('balances-list').innerHTML = balances.map((item, index) => `
    <div class="balance-row">
      <span class="balance-rank">${String(index + 1).padStart(2, '0')}</span>
      <span class="balance-address" title="${escapeHtml(item.address)}">${escapeHtml(shortHash(item.address, 16))}</span>
      <span class="balance-amount">${ntr(item.balance)} <small>NTR</small></span>
    </div>`).join('');
}

function renderPeers(peerData) {
  const peers = peerData.items;
  const counts = peerData.counts;
  const total = peers.length;
  byId('peer-count').textContent = String(counts.connected).padStart(2, '0');
  byId('peer-total').textContent = String(total).padStart(2, '0');
  byId('peer-total-tag').textContent = `${total} ${total === 1 ? 'NODE' : 'NODES'}`;
  if (!total) {
    byId('peer-list').innerHTML = '<div class="empty-row">No peer endpoints</div>';
    return;
  }
  byId('peer-list').innerHTML = peers.map((peer) => `
    <div class="peer-row">
      <span class="peer-endpoint" title="${escapeHtml(peer.host)}:${peer.port}">${escapeHtml(peer.host)}:${peer.port}</span>
      <span class="peer-state"><i class="status-dot ${escapeHtml(peer.status)}"></i>${escapeHtml(peer.status)}</span>
      <button class="remove-peer" type="button" data-peer-id="${peer.id}" title="Remove peer" aria-label="Remove ${escapeHtml(peer.host)}">×</button>
    </div>`).join('');
  document.querySelectorAll('[data-peer-id]').forEach((button) => {
    button.addEventListener('click', () => removePeer(button.dataset.peerId));
  });
}

function renderWallets(walletData) {
  wallets = walletData.wallets || [];
  const select = byId('wallet-select');
  const previous = select.value;
  select.innerHTML = wallets.map((wallet) =>
    `<option value="${escapeHtml(wallet.file)}">${escapeHtml(wallet.file)} · ${escapeHtml(shortHash(wallet.address, 15))}</option>`
  ).join('');
  if (wallets.some((wallet) => wallet.file === previous)) select.value = previous;
  updateSelectedWallet();
}

function updateSelectedWallet() {
  const wallet = wallets.find((item) => item.file === byId('wallet-select').value);
  byId('wallet-address').textContent = wallet?.address || 'Create or select a wallet';
  byId('wallet-balance').innerHTML = `${ntr(wallet?.balance || 0)} <small>NTR</small>`;
  byId('copy-wallet-address').disabled = !wallet;
}

function renderMempool(transactions = []) {
  byId('mempool-count').textContent = `${transactions.length} PENDING`;
  if (!transactions.length) {
    byId('mempool-list').innerHTML = '<div class="empty-row pending-empty">No pending wallet transactions</div>';
    return;
  }
  byId('mempool-list').innerHTML = `
    <table class="mempool-table">
      <thead><tr><th>TXID</th><th>TO</th><th>AMOUNT</th><th>STATE</th></tr></thead>
      <tbody>${transactions.map((transaction) => `
        <tr>
          <td class="pending-hash" title="${escapeHtml(transaction.txid)}">${escapeHtml(shortHash(transaction.txid, 14))}</td>
          <td class="miner-cell" title="${escapeHtml(transaction.toAddress)}">${escapeHtml(shortHash(transaction.toAddress, 17))}</td>
          <td>${ntr(transaction.amount)} NTR</td>
          <td class="pending-state">${escapeHtml(transaction.status)}</td>
        </tr>`).join('')}</tbody>
    </table>`;
}

function drawChart(samples = []) {
  const canvas = byId('hash-chart');
  const bounds = canvas.getBoundingClientRect();
  if (!bounds.width || !bounds.height) return;
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.round(bounds.width * ratio);
  canvas.height = Math.round(bounds.height * ratio);
  const context = canvas.getContext('2d');
  context.scale(ratio, ratio);
  const width = bounds.width;
  const height = bounds.height;
  const values = samples.length ? samples : Array(48).fill(0);
  const max = Math.max(1, ...values);
  const points = values.map((value, index) => ({
    x: index * width / Math.max(1, values.length - 1),
    y: height - 8 - (value / max) * (height - 20),
  }));

  context.strokeStyle = '#e6ebe6';
  context.lineWidth = 1;
  for (let row = 1; row <= 3; row += 1) {
    const y = row * height / 4;
    context.beginPath();
    context.moveTo(0, y);
    context.lineTo(width, y);
    context.stroke();
  }

  context.beginPath();
  points.forEach((point, index) => index ? context.lineTo(point.x, point.y) : context.moveTo(point.x, point.y));
  context.lineTo(width, height);
  context.lineTo(0, height);
  context.closePath();
  const fill = context.createLinearGradient(0, 0, 0, height);
  fill.addColorStop(0, 'rgba(91, 125, 226, .2)');
  fill.addColorStop(1, 'rgba(91, 125, 226, 0)');
  context.fillStyle = fill;
  context.fill();

  context.beginPath();
  points.forEach((point, index) => index ? context.lineTo(point.x, point.y) : context.moveTo(point.x, point.y));
  context.strokeStyle = '#5b7de2';
  context.lineWidth = 2;
  context.stroke();
  const last = points.at(-1);
  if (last && Number(values.at(-1)) > 0) {
    context.beginPath();
    context.arc(last.x, last.y, 3, 0, Math.PI * 2);
    context.fillStyle = '#c4664d';
    context.fill();
  }
}

function render(data) {
  currentSnapshot = data;
  byId('connection-state').textContent = 'CONNECTED';
  byId('connection-state').classList.add('connected');
  byId('chain-height').textContent = numberFormat.format(data.chain.height).padStart(6, '0');
  byId('hash-rate').textContent = numberFormat.format(data.mining.hashRate);
  byId('chart-current').textContent = `${numberFormat.format(data.mining.hashRate)} H/s`;
  byId('minted').textContent = ntr(data.chain.minted);
  byId('blocks-count').textContent = numberFormat.format(data.chain.blocks);
  byId('footer-difficulty').textContent = data.chain.difficulty;
  byId('difficulty-label').textContent = '0'.repeat(data.chain.difficulty);
  byId('candidate-height').textContent = `#${numberFormat.format(data.chain.height + 1)}`;
  byId('nonce').textContent = numberFormat.format(data.mining.nonce);
  byId('candidate-hash').textContent = data.mining.candidateHash;
  byId('miner-state').textContent = data.mining.enabled ? 'MINING' : 'STANDBY';
  byId('miner-pulse').classList.toggle('active', data.mining.enabled);
  byId('toggle-mining').innerHTML = data.mining.enabled
    ? 'Pause mining <span aria-hidden="true">Ⅱ</span>'
    : 'Start mining <span aria-hidden="true">↗</span>';
  const progress = data.mining.enabled ? data.mining.progress : 0;
  byId('progress-fill').style.width = `${progress}%`;
  document.querySelector('.work-progress').setAttribute('aria-valuenow', progress.toFixed(1));
  byId('progress-label').textContent = `${progress.toFixed(1)}%`;
  if (document.activeElement !== byId('miner-address')) {
    byId('miner-address').value = data.mining.minerAddress;
  }
  if (data.mining.lastReward) {
    byId('last-payout').textContent = `${ntr(data.mining.lastReward.amount)} NTR · #${data.mining.lastReward.height}`;
    byId('last-payout-address').textContent = data.mining.lastReward.address;
  }
  renderBlocks(data.blocks);
  renderBalances(data.balances);
  renderPeers(data.peers);
  renderMempool(data.mempool || []);
  drawChart(data.mining.samples);
}

async function postJson(path, payload) {
  const response = await fetch(path, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(payload),
  });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || 'Request failed');
  return data;
}

byId('toggle-mining').addEventListener('click', async () => {
  const enabled = !currentSnapshot?.mining.enabled;
  try {
    const result = await postJson('/api/mining', { enabled, address: byId('miner-address').value.trim() });
    render(result);
    showToast(enabled ? 'Worker started. Searching for a valid block hash.' : 'Worker paused. Ledger is saved.');
  } catch (error) {
    showToast(error.message);
  }
});

byId('apply-address').addEventListener('click', async () => {
  try {
    const result = await postJson('/api/mining', {
      enabled: Boolean(currentSnapshot?.mining.enabled),
      address: byId('miner-address').value.trim(),
    });
    render(result);
    showToast('Payout address applied.');
  } catch (error) {
    showToast(error.message);
  }
});

byId('peer-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  try {
    await postJson('/api/peers', {
      host: byId('peer-host').value.trim(),
      port: Number(byId('peer-port').value),
    });
    byId('peer-host').value = '';
    showToast('Peer request recorded; checking TCP reachability.');
  } catch (error) {
    showToast(error.message);
  }
});

byId('wallet-select').addEventListener('change', updateSelectedWallet);

byId('create-wallet').addEventListener('click', async () => {
  try {
    const result = await postJson('/api/wallets', {});
    renderWallets(result);
    showToast('Wallet created and saved locally.');
  } catch (error) {
    showToast(error.message);
  }
});

byId('copy-wallet-address').addEventListener('click', async () => {
  const wallet = wallets.find((item) => item.file === byId('wallet-select').value);
  if (!wallet) return;
  try {
    await navigator.clipboard.writeText(wallet.address);
    showToast('Public address copied.');
  } catch {
    showToast('Clipboard access is unavailable in this browser.');
  }
});

byId('wallet-send-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const wallet = byId('wallet-select').value;
  if (!wallet) {
    showToast('Create or select a wallet first.');
    return;
  }
  try {
    const result = await postJson('/api/transactions', {
      wallet,
      recipient: byId('wallet-recipient').value.trim(),
      amount: byId('wallet-amount').value,
    });
    byId('wallet-send-form').reset();
    showToast(result.broadcast
      ? 'Signed transaction sent to a peer; waiting for block confirmation.'
      : 'Signed transaction is in the local mempool; waiting for block confirmation.');
    await update_state();
    await update_wallets();
  } catch (error) {
    showToast(error.message);
  }
});

async function removePeer(id) {
  try {
    const response = await fetch(`/api/peers/${encodeURIComponent(id)}`, { method: 'DELETE' });
    if (!response.ok) throw new Error('Could not remove peer');
    showToast('Peer removed from registry.');
  } catch (error) {
    showToast(error.message);
  }
}

async function update_state() {
  try {
    const response = await fetch('/api/state');
    if (!response.ok) throw new Error('Dashboard request failed');
    render(await response.json());
  } catch {
    byId('connection-state').textContent = 'RECONNECTING';
    byId('connection-state').classList.remove('connected');
  }
}

async function update_wallets() {
  try {
    const response = await fetch('/api/wallets');
    if (!response.ok) throw new Error('Wallet list request failed');
    renderWallets(await response.json());
  } catch (error) {
    showToast(error.message);
  }
}

function updateClock() {
  byId('clock').textContent = new Intl.DateTimeFormat('en-GB', {
    hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
  }).format(new Date());
}
updateClock();
setInterval(updateClock, 1000);
window.addEventListener('resize', () => {
  if (currentSnapshot) drawChart(currentSnapshot.mining.samples);
});
update_state();
setInterval(update_state, 500);
update_wallets();
setInterval(update_wallets, 5000);
